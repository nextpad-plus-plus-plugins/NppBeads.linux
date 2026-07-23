// beads_selftest.cpp — headless verification of the NppBeads native layer
// (no WebKit/host). Exits 0 on success, non-zero with the failing check id.

#include "BeadsProjectScanner.h"
#include "JsonlDataSource.h"
#include "BdCommandRunner.h"
#include "BdDataSource.h"
#include <glib.h>
#include <cstdio>

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
    CHECK("jsonl.count", ds.issueCount() == 3);
    CHECK("jsonl.open", ds.openIssueCount() == 1);
    CHECK("jsonl.blocked", ds.blockedIssueCount() == 1);
    CHECK("jsonl.closed", ds.closedIssueCount() == 1);
    CHECK("jsonl.rawnonempty", !ds.rawText().empty());
    CHECK("jsonl.readonly", !ds.writable());
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

// ── async runner test against a fake `bd` on PATH ───────────────────────────
struct AsyncState { GMainLoop *loop; std::shared_ptr<BdCommandRunner> runner; int step = 0; };

static void asyncNext(AsyncState *st);

static void testRunnerAsync(const char *root, const char *fakeBinDir) {
    // Put the fake bd first on PATH so resolveBdBinary picks it up.
    std::string path = std::string(fakeBinDir) + ":" + (g_getenv("PATH") ? g_getenv("PATH") : "");
    g_setenv("PATH", path.c_str(), TRUE);

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
    } else {
        // Also exercise BdDataSource error mapping over the same runner.
        BdDataSource *bd = new BdDataSource(st->runner);
        bd->closeIssue("bd-x", nullptr, false, [st, bd](JsonNode *, const BeadsError &err) {
            CHECK("bdds.close.blocked", err.code == BeadsErrBlockedByDeps);
            CHECK("bdds.close.blockers", err.blockers.size() == 2);
            delete bd;
            g_main_loop_quit(st->loop);
        });
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { g_printerr("usage: %s <root> <fakeBinDir>\n", argv[0]); return 2; }
    testScanner(argv[1]);
    testJsonl(argv[1]);
    testClassification();
    testRunnerAsync(argv[1], argv[2]);
    if (!g_fail) g_print("beads_selftest: PASS\n");
    return g_fail;
}
