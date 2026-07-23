// beads_panel_harness — hosts a BeadsPanel in a plain GTK window (no plugin
// host) so the viewer + scheme handler + JS bridge can be exercised headless.
// Usage: beads_panel_harness <resourcesDir> <projectRoot> [seconds]
//
// With NPPBEADS_DEBUG_CONSOLE=1 the WebView pipes viewer console logs to
// stdout, so a passing run shows bridge.js parsing the JSONL and building the
// sql.js DB. Exits 0 after the timeout.

#include "BeadsPanel.h"
#include "BeadsProjectScanner.h"
#include <adwaita.h>

static gboolean quitCb(gpointer) {
    g_print("HARNESS: timeout reached, exiting 0\n");
    exit(0);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    if (argc < 3) { g_printerr("usage: %s <resourcesDir> <projectRoot> [seconds]\n", argv[0]); return 2; }
    adw_init();

    BeadsPanel *panel = new BeadsPanel(argv[1]);
    GtkWidget *win = gtk_window_new();
    gtk_window_set_default_size(GTK_WINDOW(win), 760, 860);
    gtk_window_set_child(GTK_WINDOW(win), panel->widget());
    gtk_window_present(GTK_WINDOW(win));

    auto proj = BeadsProjectScanner::projectFromRoot(argv[2]);
    if (!proj) { g_printerr("HARNESS: no .beads/ under %s\n", argv[2]); return 3; }
    g_print("HARNESS: binding project %s (jsonl=%s)\n",
            proj->projectRoot.c_str(), proj->jsonlPath.c_str());
    panel->bindProject(proj);

    // Bridge round-trip probe: after the viewer settles, drive a write through
    // the JS bridge and log the resolved payload. On the JSONL backend this
    // must come back {ok:false, errorKind:1} (ReadOnly) — proving the full
    // beadsBridge → handler → data source → __nppBridge.resolve loop.
    if (g_getenv("NPPBEADS_TEST_BRIDGE")) {
        g_timeout_add_seconds(3, [](gpointer p) -> gboolean {
            static_cast<BeadsPanel *>(p)->injectJS(
                "window.__nppBridge.call('createBead',{title:'Harness probe'})"
                ".then(r=>console.log('HARNESSRESULT '+JSON.stringify(r)))"
                ".catch(e=>console.log('HARNESSRESULT ERR '+e));");
            return G_SOURCE_REMOVE;
        }, panel);
    }

    int secs = argc > 3 ? atoi(argv[3]) : 6;
    g_timeout_add_seconds(secs, quitCb, nullptr);

    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);
    return 0;
}
