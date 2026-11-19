// beads_selftest.cpp — headless verification of the NppBeads native layer
// (no WebKit/host). Exits 0 on success, non-zero with the failing check id.

#include "BeadsProjectScanner.h"
#include "JsonlDataSource.h"
#include "BdCommandRunner.h"
#include "BdDataSource.h"
#include "BdEventsTail.h"
#include "BeadsRecent.h"
#include "RemoteJsonlFetcher.h"
#include <glib.h>
#include <libsoup/soup.h>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>

static int g_fail = 0;
#define CHECK(id, cond) do { if (!(cond)) { g_printerr("FAIL %s\n", id); g_fail = 1; } } while (0)

// ── sync tests ──────────────────────────────────────────────────────────────
static void testScanner(const char *root) {
    // root/proj/.beads/issues.jsonl exists; a nested file should resolve to it.
    std::string beads = std::string(root) + "/proj/.beads";
    CHECK("scanner.usable", BeadsProjectScanner::isUsableBeadsDir(beads));
    std::string nested = std::string(root) + "/proj/sub/deep/file.txt";
    auto p = BeadsProjectScanner::findProjectFromPath(nested);
    CHECK("scanner.found", p != nullptr);
    if (p) {
        CHECK("scanner.beadsdir", p->beadsDir == beads);
        CHECK("scanner.root", p->projectRoot == std::string(root) + "/proj");
        CHECK("scanner.jsonl", !p->jsonlPath.empty());
    }
    auto none = BeadsProjectScanner::findProjectFromPath(std::string(root) + "/noproj/x.txt");
    CHECK("scanner.none", none == nullptr);
}

static void testJsonl(const char *root) {
    JsonlDataSource ds;
    ds.bindToPath(std::string(root) + "/proj/.beads/issues.jsonl");
    // The fixture carries 3 issue rows PLUS a bd-1.3 "_type":"memory" record
    // and an id-less issue record — both must be filtered out (19ddebc).
    CHECK("jsonl.count", ds.issueCount() == 3);
    CHECK("jsonl.open", ds.openIssueCount() == 1);
    CHECK("jsonl.blocked", ds.blockedIssueCount() == 1);
    CHECK("jsonl.closed", ds.closedIssueCount() == 1);
    CHECK("jsonl.rawnonempty", !ds.rawText().empty());
    CHECK("jsonl.readonly", !ds.writable());

    // adoptRawText + fedExternally (20c5103): adopted text becomes the truth,
    // reload() no-ops until the flag clears.
    ds.setFedExternally(true);
    ds.adoptRawText("{\"id\":\"bd-x\",\"status\":\"open\"}\n"
                    "{\"id\":\"bd-y\",\"status\":\"open\"}\n"
                    "{\"_type\":\"memory\",\"key\":\"k\"}\n");
    CHECK("adopt.count", ds.issueCount() == 2);
    CHECK("adopt.open", ds.openIssueCount() == 2);
    ds.reload();   // must NOT re-read the file while fed externally
    CHECK("adopt.reloadnoop", ds.issueCount() == 2);
    ds.setFedExternally(false);
    ds.reload();   // back to the file as the truth
    CHECK("adopt.backtofile", ds.issueCount() == 3);
}

static void testScannerDoltOnly(const char *root) {
    // A .beads/ with only an embeddeddolt/ dir (bd 1.3 default: no jsonl
    // until an explicit export) must be usable, with jsonlPath pre-set to
    // the EXPECTED export target.
    std::string beads = std::string(root) + "/doltproj/.beads";
    g_mkdir_with_parents((beads + "/embeddeddolt").c_str(), 0755);
    CHECK("scanner.dolt.usable", BeadsProjectScanner::isUsableBeadsDir(beads));
    auto p = BeadsProjectScanner::projectFromBeadsDir(beads);
    CHECK("scanner.dolt.project", p != nullptr);
    if (p) CHECK("scanner.dolt.jsonlpath", p->jsonlPath == beads + "/issues.jsonl");
}

static void testClassification() {
    // Blocked-by-deps: stderr scrape + blocker parse.
    auto r = std::make_shared<BdResult>();
    r->ok = false; r->exitCode = 1;
    r->rawStderr = "cannot close bd-x: blocked by open issues [bd-a, bd-b] (use --force)";
    CHECK("class.blocked", r->errorKind() == BdErrBlockedByDeps);
    auto blk = r->blockerIds();
    CHECK("class.blockers", blk.size() == 2 && blk[0] == "bd-a" && blk[1] == "bd-b");

    auto nf = std::make_shared<BdResult>();
    nf->ok = false; nf->errorMessage = "issue not found: bd-zzz";
    CHECK("class.notfound", nf->errorKind() == BdErrNotFound);

    auto cy = std::make_shared<BdResult>();
    cy->ok = false; cy->rawStderr = "error: would create a cycle";
    CHECK("class.cycle", cy->errorKind() == BdErrCycle);

    auto lk = std::make_shared<BdResult>();
    lk->ok = false; lk->rawStderr = "database is locked";
    CHECK("class.locked", lk->errorKind() == BdErrLocked);
}

// ── spec parsing (RemoteJsonlFetcher, macOS nppmac-0th/iik parity) ──────────
static void testSpecParsing() {
    std::string d;
    CHECK("spec.gh.short", RemoteJsonlFetcher::specForInput("owner/repo", "", &d)
          == "github.com/owner/repo@main");
    CHECK("spec.gh.disp", d == "owner/repo @ main");
    CHECK("spec.gh.url", RemoteJsonlFetcher::specForInput(
          "https://github.com/o/r.git", "dev", &d) == "github.com/o/r@dev");
    CHECK("spec.gh.www", RemoteJsonlFetcher::specForInput(
          "www.github.com/o/r/", "", &d) == "github.com/o/r@main");
    // Self-hosted forge repo-page URL derives the Gitea/Forgejo raw layout.
    CHECK("spec.gitea", RemoteJsonlFetcher::specForInput(
          "http://192.168.1.6:3021/nextpad-plus-plus/nextpad-plus-plus-linux", "", &d)
          == "http://192.168.1.6:3021/nextpad-plus-plus/nextpad-plus-plus-linux"
             "/raw/branch/main/.beads/issues.jsonl");
    CHECK("spec.gitea.disp",
          d == "192.168.1.6:3021/nextpad-plus-plus/nextpad-plus-plus-linux @ main");
    // Direct raw URL passes through unchanged (more than two path parts).
    const char *raw = "https://host/x/raw/branch/main/.beads/issues.jsonl";
    CHECK("spec.direct", RemoteJsonlFetcher::specForInput(raw, "", &d) == raw);
    CHECK("spec.invalid", RemoteJsonlFetcher::specForInput("foo", "", &d).empty());
    CHECK("spec.empty", RemoteJsonlFetcher::specForInput("  ", "", &d).empty());
}

// ── per-project persistence additions (auto-push, remotes MRU, tail pid) ────
static void testRecentKeys() {
    const std::string root = "/tmp/selftest-proj-x";
    CHECK("recent.autopush.default", !BeadsRecent::autoPushFor(root));
    BeadsRecent::setAutoPush(root, true);
    CHECK("recent.autopush.on", BeadsRecent::autoPushFor(root));
    BeadsRecent::setAutoPush(root, false);
    CHECK("recent.autopush.off", !BeadsRecent::autoPushFor(root));

    BeadsRecent::setEventsPid(root, 4242, 77);
    int rec = 0;
    CHECK("recent.pid", BeadsRecent::eventsPidFor(root, &rec) == 4242);
    CHECK("recent.pid.recorder", rec == 77);

    BeadsRecent::pushRemote("specA", "A");
    BeadsRecent::pushRemote("specB", "B");
    BeadsRecent::pushRemote("specA", "A2");   // dedupe + move to front
    auto remotes = BeadsRecent::loadRemotes();
    CHECK("recent.remotes.count", remotes.size() == 2);
    CHECK("recent.remotes.front",
          remotes.size() == 2 && remotes[0].spec == "specA" &&
          remotes[0].display == "A2" && remotes[1].spec == "specB");
}

// ── orphan sweep helper: double-fork a detached stub tail. The middle child
// exits (and is reaped) — its pid becomes the DEAD RECORDER the sweep keys
// on; the grandchild is the orphaned tail.
struct OrphanPair { pid_t tail = 0; pid_t deadRecorder = 0; };
static OrphanPair spawnOrphanTail(const std::string &bd, const std::string &cwd) {
    OrphanPair out;
    int fds[2];
    if (pipe(fds)) return out;
    pid_t c = fork();
    if (c == 0) {
        pid_t g = fork();
        if (g == 0) {
            close(fds[0]); close(fds[1]);
            if (chdir(cwd.c_str()) != 0) _exit(127);
            setenv("BD_STUB_EVENTS", "ok", 1);
            execl(bd.c_str(), bd.c_str(), "events", "tail", "--since=0",
                  "--follow", (char *)nullptr);
            _exit(127);
        }
        ssize_t n = write(fds[1], &g, sizeof g);
        (void)n;
        _exit(0);
    }
    close(fds[1]);
    pid_t g = 0;
    ssize_t n = read(fds[0], &g, sizeof g);
    close(fds[0]);
    waitpid(c, nullptr, 0);   // middle child reaped → guaranteed-dead recorder
    out.tail = (n == (ssize_t)sizeof g) ? g : 0;
    out.deadRecorder = c;
    return out;
}

// ── async runner test against a fake `bd` on PATH ───────────────────────────
struct AsyncState { GMainLoop *loop; std::shared_ptr<BdCommandRunner> runner; int step = 0; };

static void asyncNext(AsyncState *st);

static void testRunnerAsync(const char *root, const char *fakeBinDir) {
    // Point the resolver at the stub bd (beats any real install).
    std::string stub = std::string(fakeBinDir) + "/bd";
    g_setenv("NPPBEADS_BD", stub.c_str(), TRUE);

    auto *st = new AsyncState;
    st->loop = g_main_loop_new(nullptr, FALSE);
    st->runner = std::make_shared<BdCommandRunner>(std::string(root) + "/proj");

    st->runner->probe([st](bool bdPresent, bool projectReady) {
        CHECK("runner.probe.present", bdPresent);
        CHECK("runner.probe.ready", projectReady);
        CHECK("runner.version", st->runner->bdVersion() == "9.9.9");
        asyncNext(st);
    });
    g_main_loop_run(st->loop);
    g_main_loop_unref(st->loop);
    delete st;
}

static void asyncNext(AsyncState *st) {
    if (st->step == 0) {
        st->step = 1;
        st->runner->listAllIssues([st](const BdResultPtr &res) {
            CHECK("runner.list.ok", res->ok);
            CHECK("runner.list.array", res->json && JSON_NODE_HOLDS_ARRAY(res->json));
            if (res->json && JSON_NODE_HOLDS_ARRAY(res->json))
                CHECK("runner.list.len", json_array_get_length(json_node_get_array(res->json)) == 2);
            asyncNext(st);
        });
    } else if (st->step == 1) {
        st->step = 2;
        // cache hit: second call returns instantly with the same payload.
        st->runner->listAllIssues([st](const BdResultPtr &res) {
            CHECK("runner.list.cached", res->ok);
            asyncNext(st);
        });
    } else if (st->step == 2) {
        st->step = 3;
        // close of a blocked issue → error classification through the CLI path.
        st->runner->closeIssue("bd-x", nullptr, false, [st](const BdResultPtr &res) {
            CHECK("runner.close.failed", !res->ok);
            CHECK("runner.close.blocked", res->errorKind() == BdErrBlockedByDeps);
            auto blk = res->blockerIds();
            CHECK("runner.close.blockers", blk.size() == 2);
            asyncNext(st);
        });
    } else if (st->step == 3) {
        st->step = 4;
        // Also exercise BdDataSource error mapping over the same runner.
        BdDataSource *bd = new BdDataSource(st->runner);
        bd->closeIssue("bd-x", nullptr, false, [st, bd](JsonNode *, const BeadsError &err) {
            CHECK("bdds.close.blocked", err.code == BeadsErrBlockedByDeps);
            CHECK("bdds.close.blockers", err.blockers.size() == 2);
            delete bd;
            asyncNext(st);
        });
    } else if (st->step == 4) {
        st->step = 5;
        // bd 1.3 compat: `update --unassign` answers "unknown flag" — the
        // runner must fall back to `assign <id> ""` and succeed (d701f4f).
        st->runner->unassignIssue("bd-a", [st](const BdResultPtr &res) {
            CHECK("runner.unassign.fallback", res->ok);
            CHECK("runner.unassign.viaassign",
                  !res->argv.empty() && res->argv[0] == "assign");
            asyncNext(st);
        });
    } else if (st->step == 5) {
        st->step = 6;
        // bd 1.0.2 comment interface: `comment <id> --stdin` with the body
        // piped — multi-line + special chars must round-trip (f0aa9c9).
        std::string body = "line one\nline `two` with \"quotes\" and $vars";
        st->runner->addComment("bd-a", body, [st, body](const BdResultPtr &res) {
            CHECK("runner.comment.ok", res->ok);
            bool match = false;
            if (res->json && JSON_NODE_HOLDS_OBJECT(res->json)) {
                JsonObject *o = json_node_get_object(res->json);
                const char *c = json_object_has_member(o, "comment")
                    ? json_object_get_string_member(o, "comment") : nullptr;
                match = c && body == c;
            }
            CHECK("runner.comment.roundtrip", match);
            asyncNext(st);
        });
    } else if (st->step == 6) {
        st->step = 7;
        // show: `--include-comments` answers "unknown flag" on the stub —
        // must fall back to the plain form and still unwrap the array.
        st->runner->showIssue("bd-q", [st](const BdResultPtr &res) {
            CHECK("runner.show.fallback", res->ok);
            bool hasTitle = false;
            if (res->json && JSON_NODE_HOLDS_OBJECT(res->json)) {
                JsonObject *o = json_node_get_object(res->json);
                hasTitle = json_object_has_member(o, "title");
            }
            CHECK("runner.show.object", hasTitle);
            asyncNext(st);
        });
    } else if (st->step == 7) {
        st->step = 8;
        // Dolt render: `bd export --all` to stdout (20c5103).
        CHECK("runner.eventsgate", st->runner->supportsEventsFeed());   // 9.9.9
        st->runner->exportAllJsonl([st](const BdResultPtr &res) {
            CHECK("runner.export.ok", res->ok);
            CHECK("runner.export.text",
                  res->rawStdout.find("bd-d") != std::string::npos);
            asyncNext(st);
        });
    } else if (st->step == 8) {
        st->step = 9;
        // Events tail (5fa21c2): two records -> ONE coalesced onChange with
        // lastSeq at the highest record seq.
        g_setenv("BD_STUB_EVENTS", "ok", TRUE);
        auto *tail = new BdEventsTail(st->runner->bdPath(),
                                      st->runner->projectDir());
        tail->setSinceSeq(40);
        // NB: onChange is invoked through the tail's own std::function member,
        // so deleting the tail inside it would free the running closure (and
        // its captures). Defer the delete one loop tick.
        auto deferDelete = [](BdEventsTail *t) {
            g_idle_add([](gpointer p) -> gboolean {
                auto *t = static_cast<BdEventsTail *>(p);
                t->stop(); delete t; return G_SOURCE_REMOVE;
            }, t);
        };
        tail->onChange = [st, tail, deferDelete]() {
            AsyncState *state = st;   // locals survive the deferred delete
            CHECK("tail.records", tail->recordCount() == 2);
            CHECK("tail.lastseq", tail->lastSeq() == 42);
            deferDelete(tail);
            asyncNext(state);
        };
        tail->onBroken = [st, tail, deferDelete](const std::string &reason) {
            AsyncState *state = st;
            g_printerr("FAIL tail.unexpectedbreak (%s)\n", reason.c_str());
            g_fail = 1;
            deferDelete(tail);
            asyncNext(state);
        };
        tail->start();
    } else if (st->step == 9) {
        st->step = 10;
        // Journal disabled: bd says so on stdout and exits — onBroken must
        // fire exactly once so the owner can fall back to polling.
        g_setenv("BD_STUB_EVENTS", "disabled", TRUE);
        auto *tail = new BdEventsTail(st->runner->bdPath(),
                                      st->runner->projectDir());
        tail->onChange = [] { g_printerr("FAIL tail2.unexpectedchange\n"); g_fail = 1; };
        tail->onBroken = [st, tail](const std::string &reason) {
            CHECK("tail2.brokenreason",
                  reason.find("journal is disabled") != std::string::npos);
            delete tail;
            asyncNext(st);
        };
        tail->start();
    } else if (st->step == 10) {
        st->step = 11;
        // Orphan sweep (macOS nppmac-8n1): a detached previous-run tail with
        // ppid 1 and a bd events-tail cmdline must be reaped by the next
        // start() for the same project.
        g_setenv("BD_STUB_EVENTS", "ok", TRUE);
        OrphanPair orphan = spawnOrphanTail(st->runner->bdPath(), st->runner->projectDir());
        CHECK("sweep.spawned", orphan.tail > 1);
        CHECK("sweep.alive", orphan.tail > 1 && kill(orphan.tail, 0) == 0);
        // Record the orphan under a DEAD recorder (the reaped middle child),
        // then start a fresh tail — its start() runs the sweep.
        BeadsRecent::setEventsPid(st->runner->projectDir(),
                                  (int)orphan.tail, (int)orphan.deadRecorder);
        struct SweepCtx { AsyncState *st; pid_t orphan; int ticks = 0; BdEventsTail *tail = nullptr; };
        auto *sc = new SweepCtx{st, orphan.tail};
        sc->tail = new BdEventsTail(st->runner->bdPath(), st->runner->projectDir());
        sc->tail->onChange = [] {};
        sc->tail->onBroken = [](const std::string &) {};
        sc->tail->start();
        g_timeout_add(100, +[](gpointer u) -> gboolean {
            auto *sc = static_cast<SweepCtx *>(u);
            // Phase 2: the orphan must be gone (SIGTERM delivered by the sweep).
            if (kill(sc->orphan, 0) == 0 && ++sc->ticks < 20) return G_SOURCE_CONTINUE;
            CHECK("sweep.reaped", kill(sc->orphan, 0) != 0);
            BdEventsTail *t = sc->tail;
            g_idle_add(+[](gpointer p) -> gboolean {
                auto *t = static_cast<BdEventsTail *>(p);
                t->stop(); delete t; return G_SOURCE_REMOVE;
            }, t);
            AsyncState *state = sc->st;
            delete sc;
            asyncNext(state);
            return G_SOURCE_REMOVE;
        }, sc);
    } else if (st->step == 11) {
        st->step = 12;
        // Remote fetch e2e against an in-process SoupServer: Gitea-style
        // derived raw path, 200 → onText, unchanged body + no ETag → length
        // gate suppresses a second onText, 404 → onError.
        static const char *kBody = "{\"id\":\"r-1\",\"status\":\"open\"}\n"
                                   "{\"id\":\"r-2\",\"status\":\"closed\"}\n";
        SoupServer *srv = soup_server_new(nullptr);
        soup_server_add_handler(srv, "/own/repo/raw/branch/main/.beads/issues.jsonl",
            +[](SoupServer *, SoupServerMessage *msg, const char *, GHashTable *, gpointer) {
                soup_server_message_set_status(msg, 200, nullptr);
                soup_server_message_set_response(msg, "application/octet-stream",
                    SOUP_MEMORY_STATIC, kBody, strlen(kBody));
            }, nullptr, nullptr);
        GError *err = nullptr;
        if (!soup_server_listen_local(srv, 0, (SoupServerListenOptions)0, &err)) {
            g_printerr("FAIL remote.listen (%s)\n", err ? err->message : "?");
            g_fail = 1;
            if (err) g_error_free(err);
            asyncNext(st);
            return;
        }
        GSList *uris = soup_server_get_uris(srv);
        int port = uris ? g_uri_get_port((GUri *)uris->data) : 0;
        g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);
        char base[96];
        g_snprintf(base, sizeof base, "http://127.0.0.1:%d/own/repo", port);
        std::string display;
        std::string spec = RemoteJsonlFetcher::specForInput(base, "", &display);
        CHECK("remote.derive", spec.find("/raw/branch/main/.beads/issues.jsonl")
                               != std::string::npos);
        struct RemoteCtx { AsyncState *st; SoupServer *srv;
                           RemoteJsonlFetcher *f = nullptr, *bad = nullptr;
                           int texts = 0; bool gotErr = false; };
        auto *rc = new RemoteCtx{st, srv};
        rc->f = new RemoteJsonlFetcher(spec, "");
        rc->f->onText = [rc](const std::string &jsonl) {
            rc->texts++;
            if (rc->texts == 1) {
                CHECK("remote.body", jsonl.find("r-2") != std::string::npos);
                rc->f->kick();   // same body, no ETag → length gate, no 2nd onText
                g_timeout_add(400, +[](gpointer u) -> gboolean {
                    auto *rc = static_cast<RemoteCtx *>(u);
                    CHECK("remote.lengthgate", rc->texts == 1);
                    // 404 path: derived URL for a repo the server doesn't serve.
                    GSList *uris2 = soup_server_get_uris(rc->srv);
                    int port2 = uris2 ? g_uri_get_port((GUri *)uris2->data) : 0;
                    g_slist_free_full(uris2, (GDestroyNotify)g_uri_unref);
                    char miss[96];
                    g_snprintf(miss, sizeof miss,
                               "http://127.0.0.1:%d/no/such", port2);
                    std::string d2;
                    std::string missSpec =
                        RemoteJsonlFetcher::specForInput(miss, "", &d2);
                    rc->bad = new RemoteJsonlFetcher(missSpec, "");
                    rc->bad->onError = [rc](const std::string &m) {
                        if (rc->gotErr) return;
                        rc->gotErr = true;
                        CHECK("remote.404", m.find("404") != std::string::npos);
                        g_idle_add(+[](gpointer u2) -> gboolean {
                            auto *rc = static_cast<RemoteCtx *>(u2);
                            rc->f->stop(); delete rc->f;
                            rc->bad->stop(); delete rc->bad;
                            soup_server_disconnect(rc->srv);
                            g_object_unref(rc->srv);
                            AsyncState *state = rc->st;
                            delete rc;
                            asyncNext(state);
                            return G_SOURCE_REMOVE;
                        }, rc);
                    };
                    rc->bad->onText = [](const std::string &) {
                        g_printerr("FAIL remote.404.text\n"); g_fail = 1;
                    };
                    rc->bad->start();
                    return G_SOURCE_REMOVE;
                }, rc);
            }
        };
        rc->f->onError = [](const std::string &m) {
            g_printerr("FAIL remote.fetch (%s)\n", m.c_str()); g_fail = 1;
        };
        rc->f->start();
    } else {
        g_main_loop_quit(st->loop);
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { g_printerr("usage: %s <root> <fakeBinDir>\n", argv[0]); return 2; }
    // Isolate the per-project settings ini (BeadsRecent) — the tail's orphan
    // sweep and the key tests must never touch the real user config.
    std::string xdg = std::string(argv[1]) + "/xdgconfig";
    g_mkdir_with_parents(xdg.c_str(), 0755);
    g_setenv("XDG_CONFIG_HOME", xdg.c_str(), TRUE);
    testScanner(argv[1]);
    testScannerDoltOnly(argv[1]);
    testJsonl(argv[1]);
    testClassification();
    testSpecParsing();
    testRecentKeys();
    testRunnerAsync(argv[1], argv[2]);
    if (!g_fail) g_print("beads_selftest: PASS\n");
    return g_fail;
}
