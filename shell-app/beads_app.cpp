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
#include <glib-unix.h>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <unistd.h>

#include "BeadsRecent.h"   // shared MRU (also used by the plugin panel)
#include "RemoteJsonlFetcher.h"

#include <fcntl.h>
#include <sys/wait.h>
#include <sched.h>
#include <cstdio>

// ── WebKit sandbox probe (Ubuntu 24.04 AppArmor userns restriction) ─────────
// Same probe as the plugin entry (NppBeads.cpp): without it, WebKit's bwrap
// launcher aborts the whole process on hosts that restrict unprivileged user
// namespaces. Must run before ANY WebKit initialization.
static bool userNamespacesUsable() {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (unshare(CLONE_NEWUSER) != 0) _exit(1);
        int fd = open("/proc/self/setgroups", O_WRONLY);
        if (fd >= 0) { (void)!write(fd, "deny", 4); close(fd); }
        fd = open("/proc/self/uid_map", O_WRONLY);
        if (fd < 0) _exit(1);
        char map[64];
        int n = snprintf(map, sizeof map, "0 %d 1", (int)getuid());
        bool ok = (n > 0 && write(fd, map, (size_t)n) == n);
        close(fd);
        _exit(ok ? 0 : 1);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
static void ensureWebKitCanLaunch() {
    if (g_getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS")) return;
    if (userNamespacesUsable()) return;
    g_message("[BeadsViewer] user namespaces unavailable (AppArmor userns "
              "restriction?) — disabling the WebKit sandbox so the app can run");
    g_setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", TRUE);
}

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
        // Dev-build layout: binary in build/, resources beside the source
        // tree. (Built BEFORE g_free(dir) — the first version of this
        // fallback read `dir` after freeing it and probed a garbage path.)
        std::string dev = std::string(dir) + "/../resources";
        // Prefix-relative share path: covers /usr, /usr/local, ~/.local AND
        // the snap mount (/snap/<name>/current/usr/bin/../share/…) in one
        // candidate.
        std::string rel = std::string(dir) + "/../share/beadsviewer/resources";
        g_free(dir);
        if (g_file_test((res + "/viewer/index.html").c_str(), G_FILE_TEST_EXISTS)) return res;
        if (g_file_test((dev + "/viewer/index.html").c_str(), G_FILE_TEST_EXISTS)) return dev;
        // Installed layouts: <prefix>/share/beadsviewer/resources for the
        // prefixes we ship to (XDG user dir, /usr/local, /usr).
        const std::string shares[] = {
            rel,
            std::string(g_get_user_data_dir()) + "/beadsviewer/resources",
            "/usr/local/share/beadsviewer/resources",
            "/usr/share/beadsviewer/resources",
        };
        for (const auto &share : shares)
            if (g_file_test((share + "/viewer/index.html").c_str(), G_FILE_TEST_EXISTS))
                return share;
        return res;
    }
    return "";
}

static void rebuildMenuBar();

static std::string titleForProject(const std::shared_ptr<BeadsProject> &p) {
    if (!p || p->projectRoot.empty()) return "BeadsViewer";
    if (p->isRemote()) return "BeadsViewer — " + p->remoteDisplay;
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

    gtk_window_set_icon_name(GTK_WINDOW(win), "org.nextpadplusplus.BeadsViewer");
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
static void act_quit(GSimpleAction *, GVariant *, gpointer) {
    // Close every window FIRST so each panel destructor runs and stops its
    // live-sync children (events tail / remote fetcher) — the macOS shell's
    // applicationWillTerminate parity; a bare quit leaks the bd child.
    auto wins = g_app.windows;              // destroy mutates the vector
    for (GtkWindow *w : wins) gtk_window_destroy(w);
    g_application_quit(G_APPLICATION(g_app.app));
}
static void act_github(GSimpleAction *, GVariant *, gpointer) {
    g_app_info_launch_default_for_uri("https://github.com/gastownhall/beads", nullptr, nullptr);
}
static void act_about(GSimpleAction *, GVariant *, gpointer) {
    GtkAlertDialog *a = gtk_alert_dialog_new("BeadsViewer " BEADSVIEWER_VERSION);
    gtk_alert_dialog_set_detail(a,
        "Standalone viewer for the Beads (bd) issue tracker:\n"
        "local projects (bd \u2265 1.3, Dolt or issues.jsonl) and read-only\n"
        "remote repositories over HTTP(S).\n\n"
        "Beads: https://github.com/gastownhall/beads");
    gtk_alert_dialog_show(a, activeWindow());
    g_object_unref(a);
}
static void act_openremote(GSimpleAction *, GVariant *param, gpointer) {
    const char *v = g_variant_get_string(param, nullptr);
    const char *sep = v ? strchr(v, '\x1f') : nullptr;
    if (!sep) return;
    auto proj = BeadsProjectScanner::remoteProject(std::string(v, sep - v), sep + 1);
    if (proj) openWindowForProject(proj);
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
    // Recent remote connections (\U0001f310 section, parity with the panel's
    // switcher): the manager persona lives in the shell too.
    {
        auto remotes = BeadsRecent::loadRemotes();
        if (!remotes.empty()) {
            GMenu *sec = g_menu_new();
            for (auto &e : remotes) {
                GMenuItem *it = g_menu_item_new(("\U0001f310 " + e.display).c_str(), nullptr);
                std::string packed = e.spec + '\x1f' + e.display;
                g_menu_item_set_action_and_target_value(it, "app.openremote",
                    g_variant_new_string(packed.c_str()));
                g_menu_append_item(sec, it);
                g_object_unref(it);
            }
            g_menu_append_section(recent, nullptr, G_MENU_MODEL(sec));
            g_object_unref(sec);
        }
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
        { "openremote",  G_CALLBACK(act_openremote),  G_VARIANT_TYPE_STRING },
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
    // Resume the most-recent resolvable project; with no resolvable LOCAL
    // project, fall back to the most recent REMOTE connection (the manager
    // persona — macOS nppmac-0th launch-restore parity); else an empty
    // window + the folder dialog on a truly empty first run.
    for (auto &root : BeadsRecent::load()) {
        auto proj = BeadsProjectScanner::projectFromRoot(root);
        if (proj) { openWindowForProject(proj); return; }
    }
    auto remotes = BeadsRecent::loadRemotes();
    if (!remotes.empty()) {
        auto proj = BeadsProjectScanner::remoteProject(remotes.front().spec,
                                                       remotes.front().display);
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

// Safety net (macOS NSApplicationWillTerminateNotification parity): if the
// loop ends with windows still alive (SIGTERM, g_application_quit from
// elsewhere), stop each panel's live-sync children before the process exits —
// a leaked `bd events tail` polls the workspace forever.
static void on_shutdown(GApplication *, gpointer) {
    for (GtkWindow *w : g_app.windows) {
        auto *p = static_cast<BeadsPanel *>(g_object_get_data(G_OBJECT(w), "beads-panel"));
        delete p;   // stops tail/poll/remote; process is exiting anyway
        g_object_set_data(G_OBJECT(w), "beads-panel", nullptr);
    }
    g_app.windows.clear();
}

static gboolean on_unix_signal(gpointer) {
    // SIGTERM/SIGINT/SIGHUP → the same clean path as File ▸ Quit: close the
    // windows (panel dtors stop the bd children), then leave the loop. The
    // host app gained the identical discipline with its crash-recovery work.
    act_quit(nullptr, nullptr, nullptr);
    return G_SOURCE_REMOVE;
}

// --new-window: the dock icon's "New Window" desktop action (and a plain CLI
// convenience). On a RUNNING app the second process forwards the app-level
// "newwindow" action to the primary over D-Bus (GApplication exports its
// GActionGroup) and exits; on a cold start it falls through to the normal
// activate (MRU resume) — the GNOME Terminal convention.
static gint on_handle_local_options(GApplication *app, GVariantDict *opts, gpointer) {
    if (!g_variant_dict_contains(opts, "new-window")) return -1;  // continue
    GError *err = nullptr;
    if (!g_application_register(app, nullptr, &err)) {
        if (err) { g_warning("register: %s", err->message); g_error_free(err); }
        return 1;
    }
    if (g_application_get_is_remote(app)) {
        g_action_group_activate_action(G_ACTION_GROUP(app), "newwindow", nullptr);
        return 0;   // remote done — primary opened the window
    }
    return -1;      // we ARE the primary: proceed to normal activate
}

int main(int argc, char **argv) {
    ensureWebKitCanLaunch();   // before ANY WebKit init
    g_unix_signal_add(SIGTERM, on_unix_signal, nullptr);
    g_unix_signal_add(SIGINT,  on_unix_signal, nullptr);
    g_unix_signal_add(SIGHUP,  on_unix_signal, nullptr);
    g_app.app = ADW_APPLICATION(adw_application_new(
        "org.nextpadplusplus.BeadsViewer", G_APPLICATION_HANDLES_OPEN));
    g_application_add_main_option(G_APPLICATION(g_app.app), "new-window", 'n',
        G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
        "Open a new window (focuses the running instance's app)", nullptr);
    g_signal_connect(g_app.app, "handle-local-options",
                     G_CALLBACK(on_handle_local_options), nullptr);
    g_signal_connect(g_app.app, "startup",  G_CALLBACK(on_startup), nullptr);
    g_signal_connect(g_app.app, "shutdown", G_CALLBACK(on_shutdown), nullptr);
    g_signal_connect(g_app.app, "activate", G_CALLBACK(on_activate), nullptr);
    g_signal_connect(g_app.app, "open",     G_CALLBACK(on_open), nullptr);
    int rc = g_application_run(G_APPLICATION(g_app.app), argc, argv);
    g_object_unref(g_app.app);
    return rc;
}
