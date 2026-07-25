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
#include <string>

// Serialize the project chip's GMenuModel (labels + actions, sections
// inlined) so the test can assert the switcher's contents.
static void dumpModel(GMenuModel *m, std::string &out) {
    if (!m) return;
    int n = g_menu_model_get_n_items(m);
    for (int i = 0; i < n; i++) {
        GMenuModel *sec = g_menu_model_get_item_link(m, i, G_MENU_LINK_SECTION);
        if (sec) { dumpModel(sec, out); g_object_unref(sec); continue; }
        gchar *label = nullptr, *action = nullptr;
        g_menu_model_get_item_attribute(m, i, "label", "s", &label);
        g_menu_model_get_item_attribute(m, i, "action", "s", &action);
        out += (label ? label : "?");
        out += " [";
        out += (action ? action : "-");
        out += "]\n";
        g_free(label);
        g_free(action);
    }
}

static std::string dumpChipMenu(BeadsPanel *panel) {
    GtkWidget *toolbar = gtk_widget_get_first_child(panel->widget());
    GtkWidget *chip = gtk_widget_get_first_child(toolbar);   // GtkMenuButton
    std::string out;
    dumpModel(gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(chip)), out);
    return out;
}

static gboolean quitCb(gpointer) {
    g_print("HARNESS: timeout reached, exiting 0\n");
    exit(0);
    return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
    if (argc < 3) { g_printerr("usage: %s <resourcesDir> <projectRoot> [seconds]\n", argv[0]); return 2; }
    adw_init();

    // Switcher-menu test mode: assert the chip menu's contents in the
    // empty state and after binding, then exit without running the loop.
    if (g_getenv("NPPBEADS_TEST_MENU")) {
        int fail = 0;
        auto expect = [&](const char *id, const std::string &dump, const char *needle, bool present) {
            bool has = dump.find(needle) != std::string::npos;
            if (has != present) { g_printerr("MENUFAIL %s (%s)\n", id, needle); fail = 1; }
        };
        BeadsPanel *p = new BeadsPanel(argv[1]);
        std::string d0 = dumpChipMenu(p);
        g_print("--- empty-state menu ---\n%s", d0.c_str());
        expect("empty.open",    d0, "Open .beads folder", true);
        expect("empty.nopick",  d0, "beads.pickroot", false);
        expect("empty.nounbind",d0, "Unbind", false);
        expect("empty.noreveal",d0, "Reveal", false);

        auto proj = BeadsProjectScanner::projectFromRoot(argv[2]);
        if (!proj) { g_printerr("MENUFAIL no test project\n"); return 3; }
        p->bindProject(proj);
        std::string d1 = dumpChipMenu(p);
        g_print("--- bound-state menu ---\n%s", d1.c_str());
        expect("bound.current", d1, "✓ ", true);
        expect("bound.open",    d1, "Open .beads folder", true);
        expect("bound.unbind",  d1, "Unbind current project", true);
        expect("bound.reveal",  d1, "Reveal .beads/ folder", true);

        // Recents persisted by the bind?
        std::string ini = std::string(g_get_user_config_dir()) + "/beadsviewer/recent.ini";
        gchar *txt = nullptr;
        g_file_get_contents(ini.c_str(), &txt, nullptr, nullptr);
        bool rec = txt && strstr(txt, proj->projectRoot.c_str());
        g_free(txt);
        if (!rec) { g_printerr("MENUFAIL recents-persist\n"); fail = 1; }

        // Second panel (fresh instance): the recent must now appear as a
        // pickroot item; a bogus stored root must be filtered out.
        BeadsPanel *p2 = new BeadsPanel(argv[1]);
        std::string d2 = dumpChipMenu(p2);
        g_print("--- second-panel menu (recents visible) ---\n%s", d2.c_str());
        expect("recent.pick", d2, "beads.pickroot", true);

        // Crash-probe the folder dialog: activate the action, spin the loop
        // briefly, then exit. (Selection can't be driven headless; this
        // proves the GtkFileDialog path opens cleanly.)
        {
            GtkWidget *win = gtk_window_new();
            gtk_window_set_child(GTK_WINDOW(win), p2->widget());
            gtk_window_present(GTK_WINDOW(win));
            GtkWidget *toolbar = gtk_widget_get_first_child(p2->widget());
            GtkWidget *chip = gtk_widget_get_first_child(toolbar);
            gtk_widget_activate_action(chip, "beads.openfolder", nullptr);
            for (int i = 0; i < 200; i++) g_main_context_iteration(nullptr, FALSE);
            g_print("openfolder dialog: no crash\n");
        }

        g_print(fail ? "menu test: FAIL\n" : "menu test: PASS\n");
        return fail;
    }

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
