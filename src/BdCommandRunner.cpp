// BdCommandRunner.cpp — see BdCommandRunner.h. Ported from BdCommandRunner.mm.

#include "BdCommandRunner.h"
#include <gio/gio.h>
#include <unistd.h>

// ── Error-classification regexes (identical patterns to the macOS build) ────
static GRegex *rxBlockedBy, *rxNotFound, *rxAlreadyClaimed, *rxCycle,
              *rxLocked, *rxNotInProject, *rxAutoPushWarning, *rxVersion;

static void initRegexesOnce() {
    static gsize once = 0;
    if (g_once_init_enter(&once)) {
        GRegexCompileFlags cf = G_REGEX_CASELESS;
        rxBlockedBy       = g_regex_new("blocked by open issues \\[([^\\]]+)\\]", cf, (GRegexMatchFlags)0, nullptr);
        rxNotFound        = g_regex_new("issue not found|no issue with id|no issue found|not found: [a-z]+-", cf, (GRegexMatchFlags)0, nullptr);
        rxAlreadyClaimed  = g_regex_new("already claimed by|currently (?:assigned|claimed)", cf, (GRegexMatchFlags)0, nullptr);
        rxCycle           = g_regex_new("cycle detected|would create (?:a )?cycle|circular dependency", cf, (GRegexMatchFlags)0, nullptr);
        rxLocked          = g_regex_new("database is locked|lock (?:is )?held|file lock|could not acquire lock", cf, (GRegexMatchFlags)0, nullptr);
        rxNotInProject    = g_regex_new("no beads database found|database name must not be empty|no beads configuration", cf, (GRegexMatchFlags)0, nullptr);
        rxAutoPushWarning = g_regex_new("dolt auto-push failed|git command failed|could not read username for|force-with-lease=refs/dolt|device not configured", cf, (GRegexMatchFlags)0, nullptr);
        rxVersion         = g_regex_new("version\\s+([0-9][\\w.\\-]+)", cf, (GRegexMatchFlags)0, nullptr);
        g_once_init_leave(&once, 1);
    }
}
static bool rxMatch(GRegex *r, const std::string &s) {
    return r && g_regex_match(r, s.c_str(), (GRegexMatchFlags)0, nullptr);
}

// ── BdResult ────────────────────────────────────────────────────────────────
BdResult::~BdResult() { if (json) json_node_free(json); }

BdErrorKind BdResult::errorKind() const {
    if (ok) return BdErrNone;
    std::string s = errorMessage.empty() ? rawStderr : errorMessage;
    gchar *low = g_ascii_strdown(s.c_str(), -1);
    std::string ls = low ? low : "";
    g_free(low);
    if (ls.empty()) return BdErrGeneric;
    initRegexesOnce();
    if (rxMatch(rxNotInProject, ls))    return BdErrNotInProject;
    if (rxMatch(rxBlockedBy, ls))       return BdErrBlockedByDeps;
    if (rxMatch(rxAlreadyClaimed, ls))  return BdErrAlreadyClaimed;
    if (rxMatch(rxCycle, ls))           return BdErrCycle;
    if (rxMatch(rxLocked, ls))          return BdErrLocked;
    if (rxMatch(rxNotFound, ls))        return BdErrNotFound;
    return BdErrGeneric;
}

std::vector<std::string> BdResult::blockerIds() const {
    if (errorKind() != BdErrBlockedByDeps) return {};
    std::string src = errorMessage.empty() ? rawStderr : errorMessage;
    gchar *low = g_ascii_strdown(src.c_str(), -1);
    std::string ls = low ? low : "";
    g_free(low);
    initRegexesOnce();
    GMatchInfo *mi = nullptr;
    std::vector<std::string> out;
    if (g_regex_match(rxBlockedBy, ls.c_str(), (GRegexMatchFlags)0, &mi)) {
        gchar *inside = g_match_info_fetch(mi, 1);
        if (inside) {
            gchar **parts = g_strsplit(inside, ",", -1);
            for (int i = 0; parts[i]; i++) {
                gchar *t = g_strstrip(parts[i]);
                if (*t) out.push_back(t);
            }
            g_strfreev(parts);
            g_free(inside);
        }
    }
    if (mi) g_match_info_free(mi);
    return out;
}

// ── path resolution (Linux: no Homebrew) ────────────────────────────────────
static std::string resolveBdBinary() {
    const char *home = g_get_home_dir();
    std::vector<std::string> candidates = {
        std::string(home) + "/.local/bin/bd",
        std::string(home) + "/bin/bd",
        std::string(home) + "/go/bin/bd",
        "/usr/local/bin/bd",
        "/usr/bin/bd",
    };
    for (const auto &p : candidates)
        if (g_file_test(p.c_str(), G_FILE_TEST_IS_EXECUTABLE)) return p;
    const char *envPath = g_getenv("PATH");
    if (envPath) {
        gchar **dirs = g_strsplit(envPath, ":", -1);
        std::string found;
        for (int i = 0; dirs[i] && found.empty(); i++) {
            std::string p = std::string(dirs[i]) + "/bd";
            if (g_file_test(p.c_str(), G_FILE_TEST_IS_EXECUTABLE)) found = p;
        }
        g_strfreev(dirs);
        if (!found.empty()) return found;
    }
    return "";
}

static bool stderrLineIsAutoPushWarning(const std::string &line) {
    if (line.empty()) return true;
    initRegexesOnce();
    gchar *low = g_ascii_strdown(line.c_str(), -1);
    bool m = rxMatch(rxAutoPushWarning, low ? low : "");
    g_free(low);
    return m;
}

// Extract a user-visible error from stderr (JSON {"error":…} first, else the
// first non-warning line). Mirrors the macOS warning-block skip logic.
static std::string extractStderrError(const std::string &stderrStr) {
    std::string trimmed = beads::trim(stderrStr);
    if (trimmed.empty()) return "";
    JsonNode *n = beads::parseJson(trimmed.c_str(), trimmed.size());
    if (n && JSON_NODE_HOLDS_OBJECT(n)) {
        JsonObject *o = json_node_get_object(n);
        if (json_object_has_member(o, "error")) {
            const char *e = json_object_get_string_member(o, "error");
            if (e && *e) { std::string r = e; json_node_free(n); return r; }
        }
    }
    if (n) json_node_free(n);
    bool inWarn = false;
    gchar **lines = g_strsplit(trimmed.c_str(), "\n", -1);
    std::string result;
    for (int i = 0; lines[i] && result.empty(); i++) {
        std::string t = beads::trim(lines[i]);
        if (t.empty()) { inWarn = false; continue; }
        if (g_str_has_prefix(t.c_str(), "Warning:") || g_str_has_prefix(t.c_str(), "warning:") ||
            g_str_has_prefix(t.c_str(), "Hint:") || g_str_has_prefix(t.c_str(), "Run:") ||
            g_str_has_prefix(t.c_str(), "- ")) { inWarn = true; continue; }
        if (stderrLineIsAutoPushWarning(t)) { inWarn = true; continue; }
        if (inWarn) continue;
        result = t;
    }
    g_strfreev(lines);
    return result;
}

static std::vector<std::string> collectStderrWarnings(const std::string &stderrStr) {
    std::vector<std::string> out;
    if (stderrStr.empty()) return out;
    gchar **lines = g_strsplit(stderrStr.c_str(), "\n", -1);
    for (int i = 0; lines[i]; i++) {
        std::string t = beads::trim(lines[i]);
        if (!t.empty() && stderrLineIsAutoPushWarning(t)) out.push_back(t);
    }
    g_strfreev(lines);
    return out;
}

// ── runner ──────────────────────────────────────────────────────────────────
BdCommandRunner::BdCommandRunner(const std::string &projectDir)
    : projectDir_(projectDir) {
    const char *user = g_get_user_name();
    actor_ = std::string("NppBeads/") + (user ? user : "user");
}
BdCommandRunner::~BdCommandRunner() {}

void BdCommandRunner::invalidateCache() { cache_.clear(); }

BdResultPtr BdCommandRunner::cachedFor(const std::string &key, double) {
    auto it = cache_.find(key);
    if (it != cache_.end() && it->second.expiresAt > g_get_monotonic_time())
        return it->second.result;
    return nullptr;
}
void BdCommandRunner::cacheStore(const BdResultPtr &res, const std::string &key, double ttlSec) {
    if (!res->ok) return;
    cache_[key] = { res, g_get_monotonic_time() + (gint64)(ttlSec * G_USEC_PER_SEC) };
}

// Async subprocess result carrier.
struct ExecCtx {
    BdCommandRunner *self;
    std::shared_ptr<BdResult> res;
    bool expectsJson;
    BdCompletion done;
    gint64 t0;
};

static void unwrapArrayToSingle(BdResult *r) {
    if (r->ok && r->json && JSON_NODE_HOLDS_ARRAY(r->json)) {
        JsonArray *a = json_node_get_array(r->json);
        JsonNode *first = json_array_get_length(a) > 0
            ? json_node_copy(json_array_get_element(a, 0)) : nullptr;
        json_node_free(r->json);
        r->json = first;
    }
}

static void on_communicate_done(GObject *src, GAsyncResult *ares, gpointer user) {
    ExecCtx *ctx = static_cast<ExecCtx *>(user);
    GSubprocess *proc = G_SUBPROCESS(src);
    GBytes *outB = nullptr, *errB = nullptr;
    GError *gerr = nullptr;
    BdResult *r = ctx->res.get();

    if (!g_subprocess_communicate_finish(proc, ares, &outB, &errB, &gerr)) {
        r->errorMessage = std::string("bd launch failed: ") + (gerr ? gerr->message : "unknown");
        r->rawStderr = r->errorMessage;
        if (gerr) g_error_free(gerr);
    } else {
        gsize n = 0;
        const char *p = outB ? (const char *)g_bytes_get_data(outB, &n) : nullptr;
        r->rawStdout.assign(p ? p : "", p ? n : 0);
        p = errB ? (const char *)g_bytes_get_data(errB, &n) : nullptr;
        r->rawStderr.assign(p ? p : "", p ? n : 0);
        r->exitCode = g_subprocess_get_exit_status(proc);
        r->elapsed = (g_get_monotonic_time() - ctx->t0) / 1e6;

        std::string parseErr;
        if (ctx->expectsJson) {
            std::string trimmed = beads::trim(r->rawStdout);
            if (!trimmed.empty()) {
                JsonNode *node = beads::parseJson(trimmed.c_str(), trimmed.size());
                if (node) r->json = node;
                else parseErr = "JSON parse error";
            }
        }
        r->warnings = collectStderrWarnings(r->rawStderr);

        std::string stdoutJsonError;
        if (r->json && JSON_NODE_HOLDS_OBJECT(r->json)) {
            JsonObject *o = json_node_get_object(r->json);
            if (json_object_has_member(o, "error")) {
                const char *e = json_object_get_string_member(o, "error");
                if (e && *e) stdoutJsonError = e;
            }
        }
        if (r->exitCode == 0 && parseErr.empty() && stdoutJsonError.empty()) {
            r->ok = true;
        } else {
            r->ok = false;
            if (!stdoutJsonError.empty()) r->errorMessage = stdoutJsonError;
            else if (!parseErr.empty()) r->errorMessage = parseErr;
            else {
                std::string se = extractStderrError(r->rawStderr);
                r->errorMessage = !se.empty() ? se
                    : ("bd exited " + std::to_string(r->exitCode));
            }
        }
    }
    if (outB) g_bytes_unref(outB);
    if (errB) g_bytes_unref(errB);
    g_object_unref(proc);

    std::string argvStr;
    for (size_t i = 0; i < r->argv.size(); i++) { if (i) argvStr += " "; argvStr += r->argv[i]; }
    if (r->ok) g_message("[NppBeads] bd %s -> ok (exit=%d, %.2fs)", argvStr.c_str(), r->exitCode, r->elapsed);
    else       g_warning("[NppBeads] bd %s -> FAIL exit=%d err=%s", argvStr.c_str(), r->exitCode,
                         r->errorMessage.empty() ? "(nil)" : r->errorMessage.c_str());

    BdCompletion done = std::move(ctx->done);
    BdResultPtr res = ctx->res;
    delete ctx;
    if (done) done(res);
}

void BdCommandRunner::execute(std::vector<std::string> args, const std::string *stdinText,
                              bool expectsJson, BdCompletion done) {
    auto res = std::make_shared<BdResult>();
    res->argv = args;

    std::string bd = bdPath_.empty() ? resolveBdBinary() : bdPath_;
    if (bd.empty()) {
        res->errorMessage = "bd binary not found in PATH";
        res->rawStderr = res->errorMessage;
        if (done) done(res);
        return;
    }

    // Build argv: bd [--sandbox] <args...>
    std::vector<std::string> full;
    full.push_back(bd);
    if (useSandbox_) full.push_back("--sandbox");
    for (auto &a : args) full.push_back(a);
    std::vector<const char *> argv;
    for (auto &a : full) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    GSubprocessLauncher *launcher =
        g_subprocess_launcher_new((GSubprocessFlags)(G_SUBPROCESS_FLAGS_STDIN_PIPE |
            G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE));
    g_subprocess_launcher_set_cwd(launcher, projectDir_.c_str());
    g_subprocess_launcher_setenv(launcher, "BEADS_ACTOR", actor_.c_str(), TRUE);
    if (!g_getenv("LANG")) g_subprocess_launcher_setenv(launcher, "LANG", "en_US.UTF-8", TRUE);
    // Ensure PATH covers common bin dirs for any git hooks bd invokes.
    {
        std::string path = g_getenv("PATH") ? g_getenv("PATH") : "";
        for (const char *p : { "/usr/local/bin", "/usr/bin", "/bin" }) {
            if (path.find(p) == std::string::npos) path = std::string(p) + ":" + path;
        }
        g_subprocess_launcher_setenv(launcher, "PATH", path.c_str(), TRUE);
    }

    GError *gerr = nullptr;
    GSubprocess *proc = g_subprocess_launcher_spawnv(launcher, argv.data(), &gerr);
    g_object_unref(launcher);
    if (!proc) {
        res->errorMessage = std::string("bd launch failed: ") + (gerr ? gerr->message : "unknown");
        if (gerr) g_error_free(gerr);
        if (done) done(res);
        return;
    }

    // STDIN_PIPE is always requested, so hand communicate a non-NULL GBytes
    // (empty when there's no stdin) — it writes the bytes then closes stdin,
    // giving bd immediate EOF. Passing NULL trips a GIO assertion.
    GBytes *stdinBytes = (stdinText && !stdinText->empty())
        ? g_bytes_new(stdinText->data(), stdinText->size())
        : g_bytes_new(nullptr, 0);

    ExecCtx *ctx = new ExecCtx{ this, res, expectsJson, std::move(done), g_get_monotonic_time() };
    g_subprocess_communicate_async(proc, stdinBytes, nullptr, on_communicate_done, ctx);
    g_bytes_unref(stdinBytes);
}

// ── probe ────────────────────────────────────────────────────────────────────
void BdCommandRunner::probe(std::function<void(bool, bool)> done, bool force) {
    if (probed_ && !force) {
        bool bdOk = !bdPath_.empty();
        if (done) done(bdOk, projectReady_);
        return;
    }
    bdPath_ = resolveBdBinary();
    if (bdPath_.empty()) {
        probed_ = true; projectReady_ = false;
        if (done) done(false, false);
        return;
    }
    // bd version (plain text) → parse; then bd info → projectReady.
    execute({ "version" }, nullptr, /*json*/false, [this, done](const BdResultPtr &v) {
        if (v->ok && !v->rawStdout.empty()) {
            initRegexesOnce();
            GMatchInfo *mi = nullptr;
            if (g_regex_match(rxVersion, v->rawStdout.c_str(), (GRegexMatchFlags)0, &mi)) {
                gchar *ver = g_match_info_fetch(mi, 1);
                if (ver) { bdVersion_ = ver; g_free(ver); }
            }
            if (mi) g_match_info_free(mi);
        }
        execute({ "info" }, nullptr, false, [this, done](const BdResultPtr &info) {
            projectReady_ = info->ok;
            probed_ = true;
            if (done) done(true, projectReady_);
        });
    });
}

// ── reads ────────────────────────────────────────────────────────────────────
void BdCommandRunner::listAllIssues(BdCompletion done) {
    const std::string key = "list|--all";
    if (auto hit = cachedFor(key, 0.75)) { if (done) done(hit); return; }
    execute({ "list", "--all", "--json" }, nullptr, true, [this, key, done](const BdResultPtr &r) {
        if (r->ok) cacheStore(r, key, 0.75);
        if (done) done(r);
    });
}
void BdCommandRunner::showIssue(const std::string &id, BdCompletion done) {
    if (id.empty()) { auto r = std::make_shared<BdResult>(); r->errorMessage = "issueId is empty"; if (done) done(r); return; }
    const std::string key = "show|" + id;
    if (auto hit = cachedFor(key, 0.25)) { if (done) done(hit); return; }
    execute({ "show", id, "--json" }, nullptr, true, [this, key, done](const BdResultPtr &r) {
        unwrapArrayToSingle(r.get());
        if (r->ok && !r->json) { r->ok = false; r->errorMessage = "bd show returned empty — issue likely not found"; }
        if (r->ok) cacheStore(r, key, 0.25);
        if (done) done(r);
    });
}
void BdCommandRunner::ready(BdCompletion done) {
    const std::string key = "ready";
    if (auto hit = cachedFor(key, 0.75)) { if (done) done(hit); return; }
    execute({ "ready", "--json" }, nullptr, true, [this, key, done](const BdResultPtr &r) {
        if (r->ok) cacheStore(r, key, 0.75);
        if (done) done(r);
    });
}

// ── writes ───────────────────────────────────────────────────────────────────
static std::string joinComma(const std::vector<std::string> &v) {
    std::string out;
    for (size_t i = 0; i < v.size(); i++) { if (i) out += ","; out += v[i]; }
    return out;
}
static bool needsStdin(const char *s) {
    if (!s) return false;
    if (strchr(s, '\n')) return true;
    return strpbrk(s, "\"$`\\") != nullptr;
}

void BdCommandRunner::createIssue(const std::string &title, const char *type,
                                  const int *priority, const char *description,
                                  const std::vector<std::string> &labels, BdCompletion done) {
    if (title.empty()) { auto r = std::make_shared<BdResult>(); r->errorMessage = "title is required"; if (done) done(r); return; }
    std::vector<std::string> args = { "create", title };
    if (type && *type) { args.push_back("-t"); args.push_back(type); }
    if (priority) { args.push_back("-p"); args.push_back(std::to_string(*priority)); }
    std::string stdinText; bool haveStdin = false;
    if (description && *description) {
        if (needsStdin(description)) { args.push_back("--body-file=-"); stdinText = description; haveStdin = true; }
        else { args.push_back("-d"); args.push_back(description); }
    }
    for (auto &l : labels) if (!l.empty()) { args.push_back("-l"); args.push_back(l); }
    args.push_back("--json");
    execute(args, haveStdin ? &stdinText : nullptr, true, [this, done](const BdResultPtr &r) {
        unwrapArrayToSingle(r.get());
        if (r->ok) invalidateCache();
        if (done) done(r);
    });
}

void BdCommandRunner::updateIssue(const std::string &id, const char *title,
                                  const char *description, const char *status,
                                  const int *priority, const char *type, const char *assignee,
                                  const std::vector<std::string> &addLabels,
                                  const std::vector<std::string> &removeLabels, BdCompletion done) {
    if (id.empty()) { auto r = std::make_shared<BdResult>(); r->errorMessage = "issueId is empty"; if (done) done(r); return; }
    std::vector<std::string> args = { "update", id };
    if (title && *title) { args.push_back("--title"); args.push_back(title); }
    if (status && *status) { args.push_back("--status"); args.push_back(status); }
    if (priority) { args.push_back("--priority"); args.push_back(std::to_string(*priority)); }
    if (type && *type) { args.push_back("--type"); args.push_back(type); }
    if (assignee && *assignee) { args.push_back("--assignee"); args.push_back(assignee); }
    std::string stdinText; bool haveStdin = false;
    if (description) {  // nullptr = untouched; "" = clear
        if (needsStdin(description)) { args.push_back("--description-file=-"); stdinText = description; haveStdin = true; }
        else { args.push_back("--description"); args.push_back(description); }
    }
    if (!addLabels.empty()) { args.push_back("--add-label"); args.push_back(joinComma(addLabels)); }
    if (!removeLabels.empty()) { args.push_back("--remove-label"); args.push_back(joinComma(removeLabels)); }
    args.push_back("--json");
    execute(args, haveStdin ? &stdinText : nullptr, true, [this, done](const BdResultPtr &r) {
        unwrapArrayToSingle(r.get());
        if (r->ok) invalidateCache();
        if (done) done(r);
    });
}

void BdCommandRunner::claimIssue(const std::string &id, BdCompletion done) {
    if (id.empty()) { auto r = std::make_shared<BdResult>(); r->errorMessage = "issueId is empty"; if (done) done(r); return; }
    execute({ "update", id, "--claim", "--json" }, nullptr, true, [this, done](const BdResultPtr &r) {
        unwrapArrayToSingle(r.get());
        if (r->ok) invalidateCache();
        if (done) done(r);
    });
}
void BdCommandRunner::closeIssue(const std::string &id, const char *reason, bool force, BdCompletion done) {
    if (id.empty()) { auto r = std::make_shared<BdResult>(); r->errorMessage = "issueId is empty"; if (done) done(r); return; }
    std::vector<std::string> args = { "close", id };
    if (force) args.push_back("--force");
    if (reason && *reason) { args.push_back("--reason"); args.push_back(reason); }
    args.push_back("--json");
    execute(args, nullptr, true, [this, done](const BdResultPtr &r) {
        unwrapArrayToSingle(r.get());
        if (r->ok) invalidateCache();
        if (done) done(r);
    });
}
void BdCommandRunner::reopenIssue(const std::string &id, const char *reason, BdCompletion done) {
    if (id.empty()) { auto r = std::make_shared<BdResult>(); r->errorMessage = "issueId is empty"; if (done) done(r); return; }
    std::vector<std::string> args = { "reopen", id };
    if (reason && *reason) { args.push_back("--reason"); args.push_back(reason); }
    args.push_back("--json");
    execute(args, nullptr, true, [this, done](const BdResultPtr &r) {
        unwrapArrayToSingle(r.get());
        if (r->ok) invalidateCache();
        if (done) done(r);
    });
}
void BdCommandRunner::addDependency(const std::string &dependent, const std::string &dependency,
                                    const std::string &depType, BdCompletion done) {
    if (dependent.empty() || dependency.empty()) { auto r = std::make_shared<BdResult>(); r->errorMessage = "dependentId and dependencyId both required"; if (done) done(r); return; }
    std::string type = depType.empty() ? "blocks" : depType;
    execute({ "dep", "add", dependent, dependency, "--type", type, "--json" }, nullptr, true,
            [this, done](const BdResultPtr &r) { if (r->ok) invalidateCache(); if (done) done(r); });
}
void BdCommandRunner::removeDependency(const std::string &dependent, const std::string &dependency, BdCompletion done) {
    if (dependent.empty() || dependency.empty()) { auto r = std::make_shared<BdResult>(); r->errorMessage = "dependentId and dependencyId both required"; if (done) done(r); return; }
    execute({ "dep", "remove", dependent, dependency, "--json" }, nullptr, true,
            [this, done](const BdResultPtr &r) { if (r->ok) invalidateCache(); if (done) done(r); });
}
void BdCommandRunner::deleteIssue(const std::string &id, BdCompletion done) {
    if (id.empty()) { auto r = std::make_shared<BdResult>(); r->errorMessage = "issueId is empty"; if (done) done(r); return; }
    execute({ "delete", id, "--force", "--json" }, nullptr, true,
            [this, done](const BdResultPtr &r) { if (r->ok) invalidateCache(); if (done) done(r); });
}
void BdCommandRunner::unassignIssue(const std::string &id, BdCompletion done) {
    if (id.empty()) { auto r = std::make_shared<BdResult>(); r->errorMessage = "issueId is empty"; if (done) done(r); return; }
    execute({ "update", id, "--unassign", "--json" }, nullptr, true, [this, done](const BdResultPtr &r) {
        unwrapArrayToSingle(r.get());
        if (r->ok) invalidateCache();
        if (done) done(r);
    });
}
void BdCommandRunner::addComment(const std::string &id, const std::string &body, BdCompletion done) {
    if (id.empty() || body.empty()) { auto r = std::make_shared<BdResult>(); r->errorMessage = id.empty() ? "issueId is empty" : "comment body is empty"; if (done) done(r); return; }
    execute({ "comment", "add", id, "--body-file=-", "--json" }, &body, true,
            [this, done](const BdResultPtr &r) { if (r->ok) invalidateCache(); if (done) done(r); });
}
