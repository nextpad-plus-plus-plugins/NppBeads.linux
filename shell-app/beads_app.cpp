// beads_app — standalone BeadsViewer (GTK4/libadwaita). Hosts the same
// BeadsPanel the Nextpad++ plugin uses, in a regular window with a menu bar.
// Linux port of the macOS shell-app (BeadsAppDelegate / MainWindowController /
// MenuBuilder). Same source tree, same viewer assets, no host dependency.
//
// Multi-window: one window per project. Resumes the most-recent project on
// launch; File ▸ Open Project Folder / recent list / drag-a-folder open more.

#include "BeadsPanel.h"
#include "BeadsProjectScanner.h"
#include <adwaita.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <unistd.h>

#include "BeadsRecent.h"   // shared MRU (also used by the plugin panel)

// ── app state ────────────────────────────────────────────────────────────────
struct App {
    AdwApplication *app = nullptr;
    std::vector<GtkWindow *> windows;
};
static App g_app;

static std::string resolveResourcesDir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        buf[n] = '\0';
        gchar *dir = g_path_get_dirname(buf);
        std::string res = std::string(dir) + "/resources";
        g_free(dir);
        if (g_file_test((res + "/viewer/index.html").c_str(), G_FILE_TEST_EXISTS)) return res;
        // Installed layout fallback: <prefix>/share/beadsviewer/resources
        std::string share = "/usr/share/beadsviewer/resources";
        if (g_file_test((share + "/viewer/index.html").c_str(), G_FILE_TEST_EXISTS)) return share;
        return res;
    }
    return "";
}

static void rebuildMenuBar();

static std::string titleForProject(const std::shared_ptr<BeadsProject> &p) {
    if (!p || p->projectRoot.empty()) return "BeadsViewer";
    gchar *b = g_path_get_basename(p->projectRoot.c_str());
    std::string t = std::string("BeadsViewer — ") + (b ? b : "");
    g_free(b);
    return t;
}

// Focus an existing window bound to `root`, or nullptr if none.
static GtkWindow *findWindowForRoot(const std::string &root) {
    for (GtkWindow *w : g_app.windows) {
        auto *panel = static_cast<BeadsPanel *>(g_object_get_data(G_OBJECT(w), "beads-panel"));
        if (!panel) continue;
        auto proj = panel->project();
        if (proj && proj->projectRoot == root) return w;
    }
    return nullptr;
}

static void openWindowForProject(std::shared_ptr<BeadsProject> proj) {
    if (proj && !proj->projectRoot.empty()) {
        if (GtkWindow *existing = findWindowForRoot(proj->projectRoot)) {
            gtk_window_present(existing);
            return;
        }
    }

    GtkWidget *win = gtk_application_window_new(GTK_APPLICATION(g_app.app));
    gtk_window_set_default_size(GTK_WINDOW(win), 980, 720);
    gtk_widget_set_size_request(win, 520, 400);
    gtk_application_window_set_show_menubar(GTK_APPLICATION_WINDOW(win), TRUE);

    BeadsPanel *panel = new BeadsPanel(resolveResourcesDir());
    // Retitle on any project change (incl. the in-app switcher).
    GtkWindow *winPtr = GTK_WINDOW(win);
    panel->setProjectChangedHandler([winPtr](std::shared_ptr<BeadsProject> p) {
        gtk_window_set_title(winPtr, titleForProject(p).c_str());
        if (p && !p->projectRoot.empty()) { BeadsRecent::push(p->projectRoot); rebuildMenuBar(); }
    });
    gtk_window_set_child(GTK_WINDOW(win), panel->widget());
    gtk_window_set_title(GTK_WINDOW(win), titleForProject(proj).c_str());

    g_object_set_data(G_OBJECT(win), "beads-panel", panel);
    g_app.windows.push_back(GTK_WINDOW(win));

    // Track host-window focus for the live poll.
    g_signal_connect(win, "notify::is-active", G_CALLBACK(+[](GObject *w, GParamSpec *, gpointer) {
        auto *p = static_cast<BeadsPanel *>(g_object_get_data(w, "beads-panel"));
        if (p) p->setWindowActive(gtk_window_is_active(GTK_WINDOW(w)));
    }), nullptr);

    g_signal_connect(win, "destroy", G_CALLBACK(+[](GtkWidget *w, gpointer) {
        auto &v = g_app.windows;
        v.erase(std::remove(v.begin(), v.end(), GTK_WINDOW(w)), v.end());
        auto *p = static_cast<BeadsPanel *>(g_object_get_data(G_OBJECT(w), "beads-panel"));
        delete p;   // stops poll/watcher; widget tree already torn down by GTK
    }), nullptr);

    gtk_window_present(GTK_WINDOW(win));

    if (proj) {
        panel->bindProject(proj);
        BeadsRecent::push(proj->projectRoot);
        rebuildMenuBar();
    }
}

static GtkWindow *activeWindow() {
    GtkWindow *w = gtk_application_get_active_window(GTK_APPLICATION(g_app.app));
    if (w) return w;
    return g_app.windows.empty() ? nullptr : g_app.windows.front();
}
static BeadsPanel *activePanel() {
    GtkWindow *w = activeWindow();
    return w ? static_cast<BeadsPanel *>(g_object_get_data(G_OBJECT(w), "beads-panel")) : nullptr;
}

// ── open-folder dialog ───────────────────────────────────────────────────────
static void onFolderChosen(GObject *src, GAsyncResult *res, gpointer) {
    GFile *dir = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, nullptr);
    if (!dir) return;
    gchar *path = g_file_get_path(dir);
    g_object_unref(dir);
    if (!path) return;
    auto proj = BeadsProjectScanner::findProjectFromPath(path);
    if (proj) openWindowForProject(proj);
    else {
        GtkAlertDialog *a = gtk_alert_dialog_new("No .beads/ folder found");
        gchar *msg = g_strdup_printf("No Beads project (.beads/ directory) at or above:\n%s", path);
        gtk_alert_dialog_set_detail(a, msg);
        gtk_alert_dialog_show(a, activeWindow());
        g_free(msg);
        g_object_unref(a);
    }
    g_free(path);
}
static void openProjectFolder() {
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Open Project Folder");
    gtk_file_dialog_select_folder(d, activeWindow(), nullptr, onFolderChosen, nullptr);
    g_object_unref(d);
}

// ── actions ──────────────────────────────────────────────────────────────────
static void act_open(GSimpleAction *, GVariant *, gpointer)      { openProjectFolder(); }
static void act_newwindow(GSimpleAction *, GVariant *, gpointer) { openWindowForProject(nullptr); }
static void act_reload(GSimpleAction *, GVariant *, gpointer)    { if (auto *p = activePanel()) p->reloadData(); }
static void act_reveal(GSimpleAction *, GVariant *, gpointer)    { if (auto *p = activePanel()) p->openBeadsDir(); }
static void act_close(GSimpleAction *, GVariant *, gpointer)     { if (auto *w = activeWindow()) gtk_window_close(w); }
static void act_quit(GSimpleAction *, GVariant *, gpointer)      { g_application_quit(G_APPLICATION(g_app.app)); }
static void act_github(GSimpleAction *, GVariant *, gpointer) {
    g_app_info_launch_default_for_uri("https://github.com/gastownhall/beads", nullptr, nullptr);
}
static void act_about(GSimpleAction *, GVariant *, gpointer) {
    GtkAlertDialog *a = gtk_alert_dialog_new("BeadsViewer");
    gtk_alert_dialog_set_detail(a,
        "Standalone viewer for the Beads (bd) issue tracker.\n"
        "Same panel + viewer as the Nextpad++ NppBeads plugin.\n\n"
        "Beads: https://github.com/gastownhall/beads");
    gtk_alert_dialog_show(a, activeWindow());
    g_object_unref(a);
}
static void act_openrecent(GSimpleAction *, GVariant *param, gpointer) {
    const char *root = g_variant_get_string(param, nullptr);
    auto proj = BeadsProjectScanner::projectFromRoot(root);
    if (proj) openWindowForProject(proj);
    else {
        // stale entry — drop it
        BeadsRecent::remove(root);
        rebuildMenuBar();
    }
}
static void act_clearrecent(GSimpleAction *, GVariant *, gpointer) {
    BeadsRecent::clear();
    rebuildMenuBar();
}

// ── menu bar ─────────────────────────────────────────────────────────────────
static void rebuildMenuBar() {
    GMenu *bar = g_menu_new();

    // File
    GMenu *file = g_menu_new();
    g_menu_append(file, "Open Project Folder…", "app.open");
    // Open Recent submenu
    GMenu *recent = g_menu_new();
    auto roots = BeadsRecent::load();
    if (roots.empty()) {
        GMenuItem *none = g_menu_item_new("(no recent projects)", nullptr);
        g_menu_append_item(recent, none);
        g_object_unref(none);
    } else {
        for (auto &r : roots) {
            gchar *leaf = g_path_get_basename(r.c_str());
            GMenuItem *it = g_menu_item_new(leaf ? leaf : r.c_str(), nullptr);
            g_menu_item_set_action_and_target_value(it, "app.openrecent", g_variant_new_string(r.c_str()));
            g_menu_append_item(recent, it);
            g_object_unref(it);
            g_free(leaf);
        }
        GMenu *clear = g_menu_new();
        g_menu_append(clear, "Clear Menu", "app.clearrecent");
        g_menu_append_section(recent, nullptr, G_MENU_MODEL(clear));
        g_object_unref(clear);
    }
    g_menu_append_submenu(file, "Open Recent", G_MENU_MODEL(recent));
    g_object_unref(recent);
    g_menu_append(file, "New Window", "app.newwindow");
    GMenu *fileOps = g_menu_new();
    g_menu_append(fileOps, "Reload Data", "app.reload");
    g_menu_append(fileOps, "Reveal .beads Folder", "app.reveal");
    g_menu_append_section(file, nullptr, G_MENU_MODEL(fileOps));
    g_object_unref(fileOps);
    GMenu *fileClose = g_menu_new();
    g_menu_append(fileClose, "Close Window", "app.close");
    g_menu_append(fileClose, "Quit", "app.quit");
    g_menu_append_section(file, nullptr, G_MENU_MODEL(fileClose));
    g_object_unref(fileClose);
    g_menu_append_submenu(bar, "File", G_MENU_MODEL(file));
    g_object_unref(file);

    // Help
    GMenu *help = g_menu_new();
    g_menu_append(help, "Beads on GitHub", "app.github");
    g_menu_append(help, "About BeadsViewer", "app.about");
    g_menu_append_submenu(bar, "Help", G_MENU_MODEL(help));
    g_object_unref(help);

    gtk_application_set_menubar(GTK_APPLICATION(g_app.app), G_MENU_MODEL(bar));
    g_object_unref(bar);
}

static void installActions() {
    struct { const char *name; GCallback cb; const GVariantType *param; } acts[] = {
        { "open",        G_CALLBACK(act_open),        nullptr },
        { "newwindow",   G_CALLBACK(act_newwindow),   nullptr },
        { "reload",      G_CALLBACK(act_reload),      nullptr },
        { "reveal",      G_CALLBACK(act_reveal),      nullptr },
        { "close",       G_CALLBACK(act_close),       nullptr },
        { "quit",        G_CALLBACK(act_quit),        nullptr },
        { "github",      G_CALLBACK(act_github),      nullptr },
        { "about",       G_CALLBACK(act_about),       nullptr },
        { "clearrecent", G_CALLBACK(act_clearrecent), nullptr },
        { "openrecent",  G_CALLBACK(act_openrecent),  G_VARIANT_TYPE_STRING },
    };
    for (auto &a : acts) {
        GSimpleAction *act = g_simple_action_new(a.name, a.param);
        g_signal_connect(act, "activate", a.cb, nullptr);
        g_action_map_add_action(G_ACTION_MAP(g_app.app), G_ACTION(act));
        g_object_unref(act);
    }
    // Keyboard accelerators.
    const char *openA[]  = { "<Control>o", nullptr };
    const char *newA[]   = { "<Control>n", nullptr };
    const char *closeA[] = { "<Control>w", nullptr };
    const char *quitA[]  = { "<Control>q", nullptr };
    const char *relA[]   = { "<Control>r", nullptr };
    gtk_application_set_accels_for_action(GTK_APPLICATION(g_app.app), "app.open", openA);
    gtk_application_set_accels_for_action(GTK_APPLICATION(g_app.app), "app.newwindow", newA);
    gtk_application_set_accels_for_action(GTK_APPLICATION(g_app.app), "app.close", closeA);
    gtk_application_set_accels_for_action(GTK_APPLICATION(g_app.app), "app.quit", quitA);
    gtk_application_set_accels_for_action(GTK_APPLICATION(g_app.app), "app.reload", relA);
}

// ── lifecycle ────────────────────────────────────────────────────────────────
static void on_startup(GtkApplication *, gpointer) {
    installActions();
    rebuildMenuBar();
}
static void on_activate(GtkApplication *, gpointer) {
    // Resume the most-recent resolvable project; else open an empty window
    // (or the folder dialog on a truly empty first run).
    for (auto &root : BeadsRecent::load()) {
        auto proj = BeadsProjectScanner::projectFromRoot(root);
        if (proj) { openWindowForProject(proj); return; }
    }
    openWindowForProject(nullptr);
    openProjectFolder();
}
static void on_open(GApplication *, gpointer files, gint n, const char *, gpointer) {
    GFile **arr = (GFile **)files;
    bool any = false;
    for (gint i = 0; i < n; i++) {
        gchar *path = g_file_get_path(arr[i]);
        if (!path) continue;
        auto proj = BeadsProjectScanner::findProjectFromPath(path);
        if (proj) { openWindowForProject(proj); any = true; }
        g_free(path);
    }
    if (!any) openWindowForProject(nullptr);
}

int main(int argc, char **argv) {
    g_app.app = ADW_APPLICATION(adw_application_new(
        "org.nextpadplusplus.BeadsViewer", G_APPLICATION_HANDLES_OPEN));
    g_signal_connect(g_app.app, "startup",  G_CALLBACK(on_startup), nullptr);
    g_signal_connect(g_app.app, "activate", G_CALLBACK(on_activate), nullptr);
    g_signal_connect(g_app.app, "open",     G_CALLBACK(on_open), nullptr);
    int rc = g_application_run(G_APPLICATION(g_app.app), argc, argv);
    g_object_unref(g_app.app);
    return rc;
}
