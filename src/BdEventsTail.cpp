// BdEventsTail.cpp — see header. Ported from BdEventsTail.mm.
//
// All mutable state is confined to the GLib MAIN loop: GSubprocess async
// callbacks and g_timeout fire there already, so no hopping is needed, but
// every deferred callback still gates on (a) the liveness token — the owner
// may destroy us mid-flight — and (b) the spawn generation — a stop()/
// restart must silence the previous child's stale reads.

#include "BdEventsTail.h"
#include "BeadsRecent.h"
#include "beads_types.h"
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <sys/types.h>
#include <unistd.h>

// ── Orphan sweep (macOS nppmac-8n1, Linux analogue) ─────────────────────────
// A crash / SIGKILL of the host leaves the `bd events tail --follow` child
// running forever (it polls the workspace DB and holds gate locks). Each
// spawn records its child pid per project (BeadsRecent); the NEXT spawn for
// the same project reaps the previous pid ONLY when all three hold:
//   1. it is alive,
//   2. the host process that RECORDED it is dead (macOS can test
//      ppid == launchd; on Linux an orphan re-parents to the NEAREST
//      SUBREAPER — systemd --user, a sandbox supervisor, anything — so the
//      ppid carries no signal. A dead recorder proves the child lost its
//      owner; a live sibling host's healthy tailer has a live recorder and
//      is never touched. Recorder pid reuse fails safe: we skip the reap),
//   3. its command line still is a bd/beads "events tail" invocation —
//      recycled child pids fail this (checked via /proc/<pid>/cmdline,
//      which also covers wrapper scripts where /proc/<pid>/exe would be
//      the interpreter).
static bool isOrphanedBdTail(pid_t pid, pid_t recorder) {
    if (pid <= 1 || kill(pid, 0) != 0) return false;
    if (recorder <= 1 || kill(recorder, 0) == 0) return false;

    gchar *cmdline = nullptr;
    gsize len = 0;
    gchar *cmdPath = g_strdup_printf("/proc/%d/cmdline", (int)pid);
    bool matches = false;
    if (g_file_get_contents(cmdPath, &cmdline, &len, nullptr) && len > 0) {
        // SOME argv entry's basename must be bd/beads-ish (argv[0] for the
        // real binary; argv[1] when bd is a wrapper script and argv[0] is the
        // interpreter), and "events" + "tail" must both appear as argv
        // entries.
        bool bdish = false, hasEvents = false, hasTail = false;
        for (gsize i = 0; i + 1 < len; i += strlen(cmdline + i) + 1) {
            const char *arg = cmdline + i;
            gchar *base = g_path_get_basename(arg);
            if (g_str_has_prefix(base, "bd") || g_str_has_prefix(base, "beads"))
                bdish = true;
            g_free(base);
            if (!strcmp(arg, "events")) hasEvents = true;
            if (!strcmp(arg, "tail"))   hasTail = true;
        }
        matches = bdish && hasEvents && hasTail;
    }
    g_free(cmdline);
    g_free(cmdPath);
    return matches;
}

struct TailCtx {
    std::weak_ptr<int> alive;
    BdEventsTail *self;
    unsigned gen;
    bool isStderr;
};

BdEventsTail::BdEventsTail(const std::string &bdPath, const std::string &projectDir)
    : bdPath_(bdPath), projectDir_(projectDir) {}

BdEventsTail::~BdEventsTail() { stop(); }

void BdEventsTail::start() {
    if (proc_) return;                 // already running
    stopping_ = false;
    brokenFired_ = false;
    restartCount_ = 0;
    if (lastSeq_ < sinceSeq_) lastSeq_ = sinceSeq_;
    // Crash/force-quit sweep: reap a previous run's orphaned child before
    // spawning a sibling (see isOrphanedBdTail above for the guards).
    int recorder = 0;
    if (pid_t prev = (pid_t)BeadsRecent::eventsPidFor(projectDir_, &recorder)) {
        if (isOrphanedBdTail(prev, (pid_t)recorder)) {
            g_message("[NppBeads] reaping orphaned events tail from a "
                      "previous run (pid=%d)", (int)prev);
            kill(prev, SIGTERM);
        }
    }
    spawn();
}

void BdEventsTail::stop() {
    stopping_ = true;
    spawnGen_++;                       // silence every pending async callback
    if (proc_) {
        g_subprocess_force_exit(proc_);
        g_object_unref(proc_);
        proc_ = nullptr;
    }
    if (outStream_) { g_object_unref(outStream_); outStream_ = nullptr; }
    if (errStream_) { g_object_unref(errStream_); errStream_ = nullptr; }
    changeScheduled_ = false;
    pendingChange_ = false;
}

void BdEventsTail::resume() {
    if (!paused_) return;
    paused_ = false;
    if (pendingChange_) {
        pendingChange_ = false;
        if (onChange) onChange();
    }
}

// ─────────────────────────────────────────────────────────────────────
void BdEventsTail::spawn() {
    uint64_t since = lastSeq_ > sinceSeq_ ? lastSeq_ : sinceSeq_;
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        (GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                           G_SUBPROCESS_FLAGS_STDERR_PIPE));
    g_subprocess_launcher_set_cwd(launcher, projectDir_.c_str());
    const char *user = g_get_user_name();
    std::string actor = std::string("NppBeads/") + (user ? user : "user");
    g_subprocess_launcher_setenv(launcher, "BEADS_ACTOR", actor.c_str(), TRUE);

    std::string sinceArg = "--since=" + std::to_string(since);
    const char *argv[] = { bdPath_.c_str(), "events", "tail",
                           sinceArg.c_str(), "--follow", nullptr };
    GError *err = nullptr;
    GSubprocess *proc = g_subprocess_launcher_spawnv(launcher, argv, &err);
    g_object_unref(launcher);
    if (!proc) {
        g_warning("[NppBeads] events tail failed to launch: %s",
                  err ? err->message : "?");
        std::string reason = err ? err->message : "launch failed";
        if (err) g_error_free(err);
        breakWithReason(reason);
        return;
    }
    proc_ = proc;
    // Record the child for the next run's orphan sweep. Deliberately NOT
    // cleared on clean stop (the guards reject a dead/recycled pid), so a
    // terminate between stop() and relaunch is still covered.
    if (const char *ident = g_subprocess_get_identifier(proc))
        BeadsRecent::setEventsPid(projectDir_, atoi(ident), (int)getpid());
    unsigned gen = ++spawnGen_;
    stderrTail_.clear();

    outStream_ = g_data_input_stream_new(g_subprocess_get_stdout_pipe(proc));
    errStream_ = g_data_input_stream_new(g_subprocess_get_stderr_pipe(proc));
    readNextLine(outStream_, gen);
    readNextLine(errStream_, gen);

    auto *wctx = new TailCtx{ alive_, this, gen, false };
    g_subprocess_wait_async(proc, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer user) {
            auto *ctx = static_cast<TailCtx *>(user);
            g_subprocess_wait_finish(G_SUBPROCESS(src), res, nullptr);
            // get_exit_status asserts on signal-terminated children — gate it.
            int status = g_subprocess_get_if_exited(G_SUBPROCESS(src))
                ? g_subprocess_get_exit_status(G_SUBPROCESS(src)) : -1;
            auto alive = ctx->alive.lock();
            BdEventsTail *s = alive ? ctx->self : nullptr;
            unsigned gen = ctx->gen;
            delete ctx;
            if (!s || s->stopping_ || gen != s->spawnGen_) return;
            s->childExited(status);
        }, wctx);

    g_message("[NppBeads] events tail started (since=%llu, pid=%s)",
              (unsigned long long)since, g_subprocess_get_identifier(proc));
}

void BdEventsTail::readNextLine(GDataInputStream *stream, unsigned gen) {
    auto *ctx = new TailCtx{ alive_, this, gen, stream == errStream_ };
    g_object_ref(stream);   // keep the stream alive across stop() for this op
    g_data_input_stream_read_line_async(stream, G_PRIORITY_DEFAULT, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer user) {
            auto *ctx = static_cast<TailCtx *>(user);
            GDataInputStream *stream = G_DATA_INPUT_STREAM(src);
            gsize len = 0;
            char *line = g_data_input_stream_read_line_finish_utf8(
                stream, res, &len, nullptr);
            auto alive = ctx->alive.lock();
            BdEventsTail *s = alive ? ctx->self : nullptr;
            unsigned gen = ctx->gen;
            bool isStderr = ctx->isStderr;
            delete ctx;
            if (!s || s->stopping_ || gen != s->spawnGen_) {
                g_free(line);
                g_object_unref(stream);
                return;
            }
            if (!line) {               // EOF — wait_async handles the exit
                g_object_unref(stream);
                return;
            }
            if (isStderr) {
                s->stderrTail_ += line;
                s->stderrTail_ += '\n';
                if (s->stderrTail_.size() > 2000)
                    s->stderrTail_.erase(0, s->stderrTail_.size() - 2000);
                // Journal switched off (or was never on): bd tells us on
                // stderr. The feed is blind from here — hand over to polling.
                if (strstr(line, "journal is disabled"))
                    s->breakWithReason("events journal is disabled");
            } else {
                if (s->consumeRecordLine(line)) s->scheduleChange();
            }
            g_free(line);
            s->readNextLine(stream, gen);
            g_object_unref(stream);    // readNextLine took its own ref
        }, ctx);
}

bool BdEventsTail::consumeRecordLine(const char *line) {
    JsonNode *node = beads::parseJson(line, -1);
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) {
        if (node) json_node_free(node);
        // Not a record. bd prints a human "note:" when the journal is off;
        // catch it on stdout too (stderr has its own detector).
        if (line && strstr(line, "journal is disabled"))
            breakWithReason("events journal is disabled");
        return false;
    }
    JsonObject *o = json_node_get_object(node);
    bool isRecord = false;
    if (json_object_has_member(o, "seq")) {
        uint64_t s = (uint64_t)json_object_get_int_member(o, "seq");
        if (s > lastSeq_) lastSeq_ = s;
        recordCount_++;
        restartCount_ = 0;             // a real record proves the feed is healthy
        isRecord = true;
    }
    json_node_free(node);
    return isRecord;
}

// Coalesce bursts (a close cascading blocked-state updates arrives as
// several records in one journal poll) into a single onChange.
void BdEventsTail::scheduleChange() {
    if (paused_) { pendingChange_ = true; return; }
    if (changeScheduled_) return;
    changeScheduled_ = true;
    auto *ctx = new TailCtx{ alive_, this, spawnGen_, false };
    g_timeout_add(250, [](gpointer user) -> gboolean {
        auto *ctx = static_cast<TailCtx *>(user);
        auto alive = ctx->alive.lock();
        BdEventsTail *s = alive ? ctx->self : nullptr;
        delete ctx;
        if (!s) return G_SOURCE_REMOVE;
        s->changeScheduled_ = false;
        if (s->stopping_) return G_SOURCE_REMOVE;
        if (s->paused_) { s->pendingChange_ = true; return G_SOURCE_REMOVE; }
        if (s->onChange) s->onChange();
        return G_SOURCE_REMOVE;
    }, ctx);
}

void BdEventsTail::childExited(int status) {
    g_message("[NppBeads] events tail exited (status=%d, restarts=%u) — %s",
              status, restartCount_,
              stderrTail_.empty() ? "(no stderr)" : stderrTail_.c_str());
    if (proc_) { g_object_unref(proc_); proc_ = nullptr; }
    if (outStream_) { g_object_unref(outStream_); outStream_ = nullptr; }
    if (errStream_) { g_object_unref(errStream_); errStream_ = nullptr; }
    if (restartCount_ >= 2) {
        breakWithReason("tail kept exiting (status " + std::to_string(status) + ")");
        return;
    }
    restartCount_++;
    unsigned delayMs = 1000 * restartCount_;
    auto *ctx = new TailCtx{ alive_, this, spawnGen_, false };
    g_timeout_add(delayMs, [](gpointer user) -> gboolean {
        auto *ctx = static_cast<TailCtx *>(user);
        auto alive = ctx->alive.lock();
        BdEventsTail *s = alive ? ctx->self : nullptr;
        delete ctx;
        if (!s || s->stopping_ || s->proc_) return G_SOURCE_REMOVE;
        s->spawn();
        return G_SOURCE_REMOVE;
    }, ctx);
}

void BdEventsTail::breakWithReason(const std::string &reason) {
    if (brokenFired_) return;
    brokenFired_ = true;          // set synchronously — one break per cycle
    // Defer the teardown one main-loop tick: this can be reached from inside
    // a read callback whose stream stop() unrefs.
    auto *ctx = new TailCtx{ alive_, this, spawnGen_, false };
    auto *why = new std::string(reason);
    g_idle_add_full(G_PRIORITY_DEFAULT, [](gpointer user) -> gboolean {
        auto *pair = static_cast<std::pair<TailCtx *, std::string *> *>(user);
        auto *ctx = pair->first;
        std::string reason = *pair->second;
        delete pair->second;
        auto alive = ctx->alive.lock();
        BdEventsTail *s = alive ? ctx->self : nullptr;
        delete ctx;
        delete pair;
        if (!s) return G_SOURCE_REMOVE;
        auto cb = s->onBroken;
        s->stop();
        s->stopping_ = false;     // leave the object restartable
        if (cb) cb(reason);
        return G_SOURCE_REMOVE;
    }, new std::pair<TailCtx *, std::string *>(ctx, why), nullptr);
}
