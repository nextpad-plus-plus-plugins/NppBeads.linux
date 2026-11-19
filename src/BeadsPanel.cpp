// BeadsPanel.cpp — see header. GTK4 + WebKitGTK 6.0 port of BeadsPanel.mm.

#include "BeadsPanel.h"
#include "BeadsRecent.h"
#include <adwaita.h>
#include <gio/gio.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

// ── jsStringLiteral (exact reproduction of the macOS escaper) ───────────────
std::string beads_jsStringLiteral(const std::string &s) {
    std::string out = "\"";
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        // U+2028 / U+2029 are line terminators inside JS string literals.
        if (c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 &&
            ((unsigned char)s[i + 2] == 0xA8 || (unsigned char)s[i + 2] == 0xA9)) {
            out += ((unsigned char)s[i + 2] == 0xA8) ? "\\u2028" : "\\u2029";
            i += 3; continue;
        }
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; g_snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += (char)c;
        }
        i++;
    }
    out += "\"";
    return out;
}

// ── JSCValue extraction helpers ─────────────────────────────────────────────
static bool jscHas(JSCValue *o, const char *p) { return jsc_value_object_has_property(o, p); }
static std::string jscStr(JSCValue *o, const char *p) {
    if (!jsc_value_object_has_property(o, p)) return "";
    JSCValue *v = jsc_value_object_get_property(o, p);
    std::string out;
    if (jsc_value_is_string(v)) { char *s = jsc_value_to_string(v); out = s ? s : ""; g_free(s); }
    g_object_unref(v);
    return out;
}
static bool jscBool(JSCValue *o, const char *p) {
    if (!jsc_value_object_has_property(o, p)) return false;
    JSCValue *v = jsc_value_object_get_property(o, p);
    bool b = jsc_value_is_boolean(v) ? jsc_value_to_boolean(v)
           : (jsc_value_is_number(v) ? jsc_value_to_int32(v) != 0 : false);
    g_object_unref(v);
    return b;
}
static bool jscHasNumber(JSCValue *o, const char *p, int *out) {
    if (!jsc_value_object_has_property(o, p)) return false;
    JSCValue *v = jsc_value_object_get_property(o, p);
    bool ok = jsc_value_is_number(v);
    if (ok && out) *out = jsc_value_to_int32(v);
    g_object_unref(v);
    return ok;
}
static std::vector<std::string> jscStrArray(JSCValue *o, const char *p) {
    std::vector<std::string> out;
    if (!jsc_value_object_has_property(o, p)) return out;
    JSCValue *arr = jsc_value_object_get_property(o, p);
    if (jsc_value_is_array(arr)) {
        JSCValue *lenV = jsc_value_object_get_property(arr, "length");
        int len = jsc_value_to_int32(lenV);
        g_object_unref(lenV);
        for (int i = 0; i < len; i++) {
            JSCValue *el = jsc_value_object_get_property_at_index(arr, i);
            if (jsc_value_is_string(el)) { char *s = jsc_value_to_string(el); if (s) out.push_back(s); g_free(s); }
            g_object_unref(el);
        }
    }
    g_object_unref(arr);
    return out;
}

static std::string pathLeaf(const std::string &p) {
    gchar *b = g_path_get_basename(p.c_str());
    std::string out = b ? b : p;
    g_free(b);
    return out;
}

// ── construction ─────────────────────────────────────────────────────────────
BeadsPanel::BeadsPanel(const std::string &resourcesDir) : resourcesDir_(resourcesDir) {
    ds_ = std::make_shared<JsonlDataSource>();
    activeDataSource_ = ds_.get();
    watcher_ = std::make_unique<BeadsWatcher>();
    watcher_->onChange = [this]() { reloadData(); };
    buildUI();
    loadViewer();
    refreshStatusBar();
}

BeadsPanel::~BeadsPanel() {
    if (poll_) poll_->stop();
    if (tail_) tail_->stop();
    watcher_->stop();
    // root_ is owned by the host once docked; do not destroy it here.
}

// ── UI ───────────────────────────────────────────────────────────────────────
void BeadsPanel::buildUI() {
    root_ = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(root_, "npp-panel-content");

    gtk_box_append(GTK_BOX(root_), makeToolbar());

    setupWebView();
    gtk_widget_set_hexpand(GTK_WIDGET(webView_), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(webView_), TRUE);
    gtk_box_append(GTK_BOX(root_), GTK_WIDGET(webView_));

    statusLabel_ = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(statusLabel_), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(statusLabel_), PANGO_ELLIPSIZE_END);
    gtk_widget_set_margin_start(statusLabel_, 8);
    gtk_widget_set_margin_end(statusLabel_, 8);
    gtk_widget_set_margin_top(statusLabel_, 2);
    gtk_widget_set_margin_bottom(statusLabel_, 2);
    gtk_widget_add_css_class(statusLabel_, "dim-label");
    gtk_box_append(GTK_BOX(root_), statusLabel_);
}

GtkWidget *BeadsPanel::makeToolbar() {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(row, 4);
    gtk_widget_set_margin_end(row, 4);
    gtk_widget_set_margin_top(row, 4);
    gtk_widget_set_margin_bottom(row, 4);

    // Project chip = menu button carrying the switcher + panel actions.
    projectChip_ = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(projectChip_), "(no project) ▾");
    gtk_widget_set_tooltip_text(projectChip_, "Switch Beads project");
    gtk_box_append(GTK_BOX(row), projectChip_);

    // Action group for the menu.
    GSimpleActionGroup *ag = g_simple_action_group_new();
    auto addAct = [&](const char *name, GCallback cb) {
        GSimpleAction *a = g_simple_action_new(name, nullptr);
        g_signal_connect(a, "activate", cb, this);
        g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(a));
        g_object_unref(a);
    };
    addAct("reload",       G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer s){ static_cast<BeadsPanel*>(s)->reloadData(); }));
    addAct("reloadviewer", G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer s){ auto *p=static_cast<BeadsPanel*>(s); p->viewerLoaded_=false; p->loadViewer(); }));
    addAct("reveal",       G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer s){ static_cast<BeadsPanel*>(s)->openBeadsDir(); }));
    addAct("unbind",       G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer s){ static_cast<BeadsPanel*>(s)->bindProject(nullptr); }));
    addAct("hide",         G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer s){ auto *p=static_cast<BeadsPanel*>(s); if (p->hideHandler_) p->hideHandler_(); }));
    // "Open .beads folder…" — the folder-picker path (macOS _didPickOpenBeadsDir).
    addAct("openfolder",   G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer s){ static_cast<BeadsPanel*>(s)->openBeadsFolderDialog(); }));
    // Parameterized: pick a recent/discovered project by its ROOT (macOS
    // _didPickRecentProject — validates and prunes stale entries).
    GSimpleAction *pick = g_simple_action_new("pickroot", G_VARIANT_TYPE_STRING);
    g_signal_connect(pick, "activate",
        G_CALLBACK(+[](GSimpleAction *, GVariant *param, gpointer s){
            static_cast<BeadsPanel*>(s)->pickProjectRoot(
                g_variant_get_string(param, nullptr));
        }), this);
    g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(pick));
    g_object_unref(pick);
    // Data-source priority — stateful radio pair ("dolt" | "jsonl"),
    // persisted per project (macOS 20c5103 Data Source submenu).
    datasrcAction_ = g_simple_action_new_stateful("datasrc", G_VARIANT_TYPE_STRING,
                                                  g_variant_new_string("dolt"));
    g_simple_action_set_enabled(datasrcAction_, FALSE);
    g_signal_connect(datasrcAction_, "activate",
        G_CALLBACK(+[](GSimpleAction *a, GVariant *param, gpointer s){
            const char *v = g_variant_get_string(param, nullptr);
            g_simple_action_set_state(a, g_variant_new_string(v));
            static_cast<BeadsPanel*>(s)->setDataSourcePriorityJsonl(
                g_strcmp0(v, "jsonl") == 0);
        }), this);
    g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(datasrcAction_));
    g_object_unref(datasrcAction_);   // group holds the ref
    exportAction_ = g_simple_action_new("exportnow", nullptr);
    g_simple_action_set_enabled(exportAction_, FALSE);
    g_signal_connect(exportAction_, "activate",
        G_CALLBACK(+[](GSimpleAction *, GVariant *, gpointer s){
            static_cast<BeadsPanel*>(s)->exportJsonlNow();
        }), this);
    g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(exportAction_));
    g_object_unref(exportAction_);
    gtk_widget_insert_action_group(root_, "beads", G_ACTION_GROUP(ag));
    g_object_unref(ag);

    // Persistent model, repopulated in place; a popover-map hook rebuilds it
    // right before every popup so recency/discovery reflect the moment of the
    // click (macOS builds the switcher menu on each chip tap).
    projectMenu_ = g_menu_new();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(projectChip_), G_MENU_MODEL(projectMenu_));
    rebuildProjectMenu();
    if (GtkPopover *pop = gtk_menu_button_get_popover(GTK_MENU_BUTTON(projectChip_))) {
        g_signal_connect(pop, "map", G_CALLBACK(+[](GtkWidget *, gpointer s){
            static_cast<BeadsPanel*>(s)->rebuildProjectMenu();
        }), this);
    }

    // View-mode dropdown (Issues retired; Activity replaces it — macOS order).
    const char *modes[] = { "Dashboard", "Insights", "Activity", "Board", "Graph", nullptr };
    viewModeDrop_ = gtk_drop_down_new_from_strings(modes);
    g_signal_connect(viewModeDrop_, "notify::selected", G_CALLBACK(s_viewModeChanged), this);
    gtk_box_append(GTK_BOX(row), viewModeDrop_);

    // Search.
    searchEntry_ = gtk_search_entry_new();
    gtk_widget_set_hexpand(searchEntry_, TRUE);
    gtk_widget_set_visible(searchEntry_, FALSE);
    g_signal_connect(searchEntry_, "search-changed", G_CALLBACK(s_searchChanged), this);
    gtk_box_append(GTK_BOX(row), searchEntry_);

    // Refresh.
    GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, "Reload issues from disk");
    g_signal_connect(refresh, "clicked", G_CALLBACK(s_refreshClicked), this);
    gtk_box_append(GTK_BOX(row), refresh);

    // Theme cycle.
    themeBtn_ = gtk_button_new_from_icon_name("weather-clear-symbolic");
    gtk_widget_set_tooltip_text(themeBtn_, "Theme: Auto");
    g_signal_connect(themeBtn_, "clicked", G_CALLBACK(s_themeClicked), this);
    gtk_box_append(GTK_BOX(row), themeBtn_);

    return row;
}

// Mirrors macOS _buildProjectSwitcherMenu: current project (checked label) →
// recents (persisted, validated) + session-discovered → "Open .beads folder…"
// → "Unbind current project" → panel utilities. Candidates are gathered fresh
// on every call; a content signature skips no-op rebuilds (this runs on every
// popover map).
void BeadsPanel::rebuildProjectMenu() {
    // Candidate roots, MRU-ordered: persisted recents first, then projects
    // discovered from session-seen file paths. Dedupe; skip the current one.
    std::vector<std::string> roots = BeadsRecent::loadValidated();
    for (auto &p : discoverProjects(12)) {
        if (std::find(roots.begin(), roots.end(), p->projectRoot) == roots.end())
            roots.push_back(p->projectRoot);
    }
    std::string cur = project_ ? project_->projectRoot : "";
    roots.erase(std::remove(roots.begin(), roots.end(), cur), roots.end());

    // Signature: everything that affects menu content.
    bool bdActive = bdDataSource_ && activeDataSource_ == bdDataSource_.get();
    std::string sig = cur + "\x1e";
    for (auto &r : roots) { sig += r; sig += "\x1f"; }
    sig += project_ ? "P" : "-";
    sig += hideHandler_ ? "H" : "-";
    sig += bdActive ? "B" : "-";
    if (sig == projectMenuSig_) return;
    projectMenuSig_ = sig;

    g_menu_remove_all(projectMenu_);

    // Current project row — checked, non-actionable (unregistered action
    // renders it insensitive, like the macOS disabled state-on row).
    if (project_) {
        GMenu *sec = g_menu_new();
        std::string label = "✓ " + pathLeaf(project_->projectRoot);
        g_menu_append(sec, label.c_str(), "beads.__current");
        g_menu_append_section(projectMenu_, nullptr, G_MENU_MODEL(sec));
        g_object_unref(sec);
    }

    if (!roots.empty()) {
        GMenu *sec = g_menu_new();
        for (auto &r : roots) {
            GMenuItem *item = g_menu_item_new(pathLeaf(r).c_str(), nullptr);
            g_menu_item_set_action_and_target_value(item, "beads.pickroot",
                g_variant_new_string(r.c_str()));
            g_menu_append_item(sec, item);
            g_object_unref(item);
        }
        g_menu_append_section(projectMenu_, "Projects", G_MENU_MODEL(sec));
        g_object_unref(sec);
    }

    // Always present — with no recents and no discovery this is the menu's
    // one real option, giving first-run users a way in (macOS parity).
    {
        GMenu *sec = g_menu_new();
        g_menu_append(sec, "Open .beads folder…", "beads.openfolder");
        if (project_) g_menu_append(sec, "Unbind current project", "beads.unbind");
        g_menu_append_section(projectMenu_, nullptr, G_MENU_MODEL(sec));
        g_object_unref(sec);
    }

    // Panel utilities (macOS keeps these on the right-click context menu; the
    // Linux chip menu is the single home for both). Project-scoped entries
    // only appear when a project is bound.
    {
        GMenu *sec = g_menu_new();
        if (project_) {
            g_menu_append(sec, "Reload issues from disk", "beads.reload");
            g_menu_append(sec, "Reveal .beads/ folder",   "beads.reveal");
        }
        g_menu_append(sec, "Reload viewer", "beads.reloadviewer");
        g_menu_append_section(projectMenu_, nullptr, G_MENU_MODEL(sec));
        g_object_unref(sec);
    }

    // Data-source priority (Dolt-live default · issues.jsonl escape hatch) +
    // manual export — bd backend only (macOS keeps these on the context menu;
    // the chip menu is the Linux home for both).
    if (bdActive) {
        GMenu *sec = g_menu_new();
        GMenu *srcMenu = g_menu_new();
        GMenuItem *dolt = g_menu_item_new("Prefer local Dolt DB (live via bd)", nullptr);
        g_menu_item_set_action_and_target_value(dolt, "beads.datasrc",
            g_variant_new_string("dolt"));
        g_menu_append_item(srcMenu, dolt);
        g_object_unref(dolt);
        GMenuItem *jsonl = g_menu_item_new("Prefer issues.jsonl (file on disk)", nullptr);
        g_menu_item_set_action_and_target_value(jsonl, "beads.datasrc",
            g_variant_new_string("jsonl"));
        g_menu_append_item(srcMenu, jsonl);
        g_object_unref(jsonl);
        g_menu_append_submenu(sec, "Data Source", G_MENU_MODEL(srcMenu));
        g_object_unref(srcMenu);
        g_menu_append(sec, "Export issues.jsonl now (from Dolt)", "beads.exportnow");
        g_menu_append_section(projectMenu_, nullptr, G_MENU_MODEL(sec));
        g_object_unref(sec);
    }

    if (hideHandler_) {
        GMenu *sec = g_menu_new();
        g_menu_append(sec, "Hide panel", "beads.hide");
        g_menu_append_section(projectMenu_, nullptr, G_MENU_MODEL(sec));
        g_object_unref(sec);
    }
}

// macOS _didPickRecentProject: validate, bind; if .beads/ vanished since the
// menu was built — friendly alert + prune the stale MRU entry.
void BeadsPanel::pickProjectRoot(const std::string &root) {
    if (root.empty()) return;
    auto proj = BeadsProjectScanner::projectFromRoot(root);
    if (!proj) {
        BeadsRecent::remove(root);
        projectMenuSig_.clear();   // force rebuild next popup
        GtkAlertDialog *a = gtk_alert_dialog_new("Project not found");
        std::string msg = "The .beads/ directory for this project no longer exists:\n\n" + root;
        gtk_alert_dialog_set_detail(a, msg.c_str());
        GtkRoot *win = gtk_widget_get_root(root_);
        gtk_alert_dialog_show(a, GTK_IS_WINDOW(win) ? GTK_WINDOW(win) : nullptr);
        g_object_unref(a);
        return;
    }
    bindProject(proj);
}

// macOS _didPickOpenBeadsDir: folder picker; accepts either the .beads/
// directory itself or its parent (project root); validates and binds.
void BeadsPanel::openBeadsFolderDialog() {
    GtkFileDialog *d = gtk_file_dialog_new();
    gtk_file_dialog_set_title(d, "Open .beads folder");
    // Start at the current project's root so the user doesn't start from
    // scratch; fall back to $HOME. (.beads is hidden — picking the project
    // folder itself is the natural flow; Ctrl+H shows hidden dirs.)
    std::string start = project_ ? project_->projectRoot : g_get_home_dir();
    GFile *init = g_file_new_for_path(start.c_str());
    gtk_file_dialog_set_initial_folder(d, init);
    g_object_unref(init);

    GtkRoot *win = gtk_widget_get_root(root_);
    gtk_file_dialog_select_folder(d, GTK_IS_WINDOW(win) ? GTK_WINDOW(win) : nullptr, nullptr,
        +[](GObject *src, GAsyncResult *res, gpointer s) {
            auto *self = static_cast<BeadsPanel *>(s);
            GFile *dir = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(src), res, nullptr);
            if (!dir) return;   // cancelled
            gchar *path = g_file_get_path(dir);
            g_object_unref(dir);
            if (!path) return;
            std::string chosen = path;
            g_free(path);
            // Canonicalize to the .beads/ dir: accept it directly or append.
            std::string beadsDir = chosen;
            gchar *base = g_path_get_basename(chosen.c_str());
            if (g_strcmp0(base, ".beads") != 0) beadsDir = chosen + "/.beads";
            g_free(base);
            auto proj = BeadsProjectScanner::projectFromBeadsDir(beadsDir);
            if (!proj) {
                GtkAlertDialog *a = gtk_alert_dialog_new("Not a beads project");
                gtk_alert_dialog_set_detail(a,
                    "The chosen directory doesn't contain a usable .beads/ "
                    "folder (expected issues.jsonl, beads.db, or an "
                    "embeddeddolt/ database inside).");
                GtkRoot *w = gtk_widget_get_root(self->root_);
                gtk_alert_dialog_show(a, GTK_IS_WINDOW(w) ? GTK_WINDOW(w) : nullptr);
                g_object_unref(a);
                return;
            }
            self->bindProject(proj);
        }, this);
    g_object_unref(d);
}

// ── WebView ──────────────────────────────────────────────────────────────────
void BeadsPanel::setupWebView() {
    webContext_ = webkit_web_context_new();
    std::string viewerDir = resourcesDir_ + "/viewer";
    scheme_ = std::make_unique<BeadsSchemeHandler>(viewerDir);
    scheme_->registerOn(webContext_);

    ucm_ = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(ucm_, "beadsBridge", nullptr);
    g_signal_connect(ucm_, "script-message-received::beadsBridge",
                     G_CALLBACK(s_scriptMessage), this);

    webView_ = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "web-context", webContext_,
        "user-content-manager", ucm_,
        nullptr));

    WebKitSettings *ws = webkit_web_view_get_settings(webView_);
    webkit_settings_set_enable_javascript(ws, TRUE);
    webkit_settings_set_enable_developer_extras(ws, TRUE);
    webkit_settings_set_javascript_can_access_clipboard(ws, TRUE);
    webkit_settings_set_enable_write_console_messages_to_stdout(
        ws, g_getenv("NPPBEADS_DEBUG_CONSOLE") != nullptr);

    webkit_web_view_set_zoom_level(webView_, 0.80);   // kBeadsZoomDefault

    g_signal_connect(webView_, "load-changed", G_CALLBACK(s_loadChanged), this);
    g_signal_connect(webView_, "decide-policy", G_CALLBACK(s_decidePolicy), this);
}

// ── viewer load + injection ──────────────────────────────────────────────────
std::string BeadsPanel::urlForViewMode(BeadsViewMode m) const {
    switch (m) {
        case BeadsViewMode::Dashboard: return "nppbeads://viewer/index.html#/";
        case BeadsViewMode::Insights:  return "nppbeads://viewer/index.html#/insights";
        case BeadsViewMode::Activity:  return "nppbeads://viewer/app/activity.html";
        case BeadsViewMode::Board:     return "nppbeads://viewer/app/board.html";
        case BeadsViewMode::Graph:     return "nppbeads://viewer/index.html#/graph";
    }
    return "nppbeads://viewer/index.html#/";
}

void BeadsPanel::installUserScripts() {
    webkit_user_content_manager_remove_all_scripts(ucm_);

    std::string raw = ds_->rawText();
    std::string projPath = project_ ? project_->projectRoot : "";

    // (1) JSONL + project-path preload at document-start.
    std::string s1 =
        "window.__nppBeadsPreloadedJsonl = " + beads_jsStringLiteral(raw) + ";"
        "window.__nppBeadsProjectPath = " + beads_jsStringLiteral(projPath) + ";";
    WebKitUserScript *us1 = webkit_user_script_new(
        s1.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm_, us1);
    webkit_user_script_unref(us1);

    // (2) Theme bootstrap (before page scripts, avoids flash).
    bool dark = isDark();
    std::string s2 = dark
        ? "document.documentElement.classList.add('dark');"
          "document.documentElement.dataset.theme='dark';"
          "try{localStorage.setItem('darkMode','true');}catch(e){}"
        : "document.documentElement.classList.remove('dark');"
          "document.documentElement.dataset.theme='light';"
          "try{localStorage.setItem('darkMode','false');}catch(e){}";
    WebKitUserScript *us2 = webkit_user_script_new(
        s2.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(ucm_, us2);
    webkit_user_script_unref(us2);
    // WASM bytes are served by the nppbeads:// scheme handler with the correct
    // application/wasm MIME; the viewer's locateFile resolves them there, so no
    // heavy base64 byte-map injection is needed on Linux.
}

void BeadsPanel::loadViewer() {
    if (resourcesDir_.empty()) return;
    std::string idx = resourcesDir_ + "/viewer/index.html";
    if (!g_file_test(idx.c_str(), G_FILE_TEST_EXISTS)) {
        std::string html = "<html><body style='font-family:sans-serif;padding:2em'>"
            "<h3>NppBeads</h3><p>viewer missing at " + idx + "</p></body></html>";
        webkit_web_view_load_html(webView_, html.c_str(), "nppbeads://viewer/");
        return;
    }
    installUserScripts();
    viewerLoaded_ = false;
    webkit_web_view_load_uri(webView_, urlForViewMode(viewMode_).c_str());
}

void BeadsPanel::runJS(const std::string &js) {
    if (!webView_) return;
    webkit_web_view_evaluate_javascript(webView_, js.c_str(), -1, nullptr, nullptr,
                                        nullptr, nullptr, nullptr);
}

void BeadsPanel::injectJS(const std::string &js) { runJS(js); }

void BeadsPanel::runOnBoardView(const std::string &js) {
    if (viewMode_ == BeadsViewMode::Board && viewerLoaded_ && webView_) {
        runJS(js);
        return;
    }
    pendingPostLoadJS_ = js;
    setViewMode(BeadsViewMode::Board, true);
}

void BeadsPanel::pushJsonlToBridge() {
    std::string raw = ds_->rawText();
    std::string js = "if (window.__nppBeads) { window.__nppBeads.receiveJsonl(" +
                     beads_jsStringLiteral(raw) + "); }";
    runJS(js);
}

void BeadsPanel::broadcastDataChanged() {
    // Dolt-priority render: pull canonical JSONL text straight from the
    // database (`bd export --all` to stdout — same schema as the file, incl.
    // dependencies), adopt it into ds_ so counts/preload stay on one code
    // path, then push. The jsonl FILE is not consulted, so bd 1.3's export
    // throttling can't make the view stale.
    if (renderFromBd_ && bdRunner_ &&
        bdDataSource_ && activeDataSource_ == bdDataSource_.get()) {
        if (bcInFlight_) { bcPending_ = true; return; }
        bcInFlight_ = true;
        activeDataSource_->invalidateCache();
        BdCommandRunner *runner = bdRunner_.get();
        runner->exportAllJsonl([this, runner](const BdResultPtr &res) {
            bcInFlight_ = false;
            if (bdRunner_.get() != runner) return;   // superseded by later bind
            std::string fresh;
            if (res->ok) {
                fresh = res->rawStdout;
                ds_->adoptRawText(fresh);
            } else {
                g_message("[NppBeads] dolt render refresh failed (%s) — keeping last data",
                          res->errorMessage.empty() ? "?" : res->errorMessage.c_str());
                fresh = ds_->rawText();
            }
            refreshStatusBar();
            pushJsonlToWebview(fresh);
            if (bcPending_) { bcPending_ = false; broadcastDataChanged(); }
        });
        return;
    }

    // jsonl-priority (or no bd backend): the file on disk is the truth.
    activeDataSource_->invalidateCache();
    ds_->reload();
    std::string fresh = ds_->rawText();
    refreshStatusBar();
    pushJsonlToWebview(fresh);
}

void BeadsPanel::pushJsonlToWebview(const std::string &fresh) {
    jsonlBytesLastSeen_ = fresh.size();   // keep reloadData's change gate honest
    std::string jsLit = beads_jsStringLiteral(fresh);
    std::string js =
        "window.__nppBeadsPreloadedJsonl = " + jsLit + ";"
        "if (window.__nppApp && typeof window.__nppApp.reload === 'function') {"
        "  window.__nppApp.reload(window.__nppBeadsPreloadedJsonl);"
        "} else if (window.__nppBeads && typeof window.__nppBeads.receiveJsonl === 'function') {"
        "  window.__nppBeads.receiveJsonl(window.__nppBeadsPreloadedJsonl);"
        "}";
    runJS(js);
}

// ── theme ────────────────────────────────────────────────────────────────────
bool BeadsPanel::isDark() const {
    if (themePref_ == BeadsThemePref::Light) return false;
    if (themePref_ == BeadsThemePref::Dark)  return true;
    // Auto → follow libadwaita / GTK dark preference.
    AdwStyleManager *sm = adw_style_manager_get_default();
    return adw_style_manager_get_dark(sm);
}

void BeadsPanel::pushTheme() {
    bool dark = isDark();
    std::string js = dark
        ? "document.documentElement.classList.add('dark');"
          "document.documentElement.dataset.theme='dark';"
          "try{localStorage.setItem('darkMode','true');}catch(e){}"
          "if(window.__nppApp)window.__nppApp.setTheme('dark');"
          "if(window.__nppGraph&&window.__nppGraph.applyTheme)window.__nppGraph.applyTheme(true);"
        : "document.documentElement.classList.remove('dark');"
          "document.documentElement.dataset.theme='light';"
          "try{localStorage.setItem('darkMode','false');}catch(e){}"
          "if(window.__nppApp)window.__nppApp.setTheme('light');"
          "if(window.__nppGraph&&window.__nppGraph.applyTheme)window.__nppGraph.applyTheme(false);";
    runJS(js);
}

void BeadsPanel::pushSearchQuery(const std::string &q) {
    if (!viewerLoaded_) { lastSearchQuery_ = q; return; }
    lastSearchQuery_ = q;
    std::string lit = beads_jsStringLiteral(q);
    if (viewMode_ == BeadsViewMode::Board || viewMode_ == BeadsViewMode::Activity)
        runJS("if(window.__nppApp&&window.__nppApp.setFilter)window.__nppApp.setFilter({query:" + lit + "});");
    else if (viewMode_ == BeadsViewMode::Graph)
        // Find-node (isolate matches + neighbours); bridge.js parks the
        // query until the graph loads.
        runJS("if(window.__nppGraphSearch)window.__nppGraphSearch(" + lit + ");");
}

// ── view mode ────────────────────────────────────────────────────────────────
void BeadsPanel::setViewMode(BeadsViewMode m, bool navigate) {
    viewMode_ = m;
    settingDropdown_ = true;
    gtk_drop_down_set_selected(GTK_DROP_DOWN(viewModeDrop_), (guint)m);
    settingDropdown_ = false;
    refreshTitleBar();
    if (navigate) {
        viewerLoaded_ = false;
        installUserScripts();
        webkit_web_view_load_uri(webView_, urlForViewMode(m).c_str());
    }
}

// ── public API ───────────────────────────────────────────────────────────────
void BeadsPanel::bindProject(std::shared_ptr<BeadsProject> project) {
    // Same-project rebind → refresh only, no reload.
    if (project && project_ && !project->beadsDir.empty() &&
        project->beadsDir == project_->beadsDir) {
        project_ = project;
        ds_->bindToPath(project->jsonlPath);
        watcher_->watchPath(project->jsonlPath);
        if (!project->projectRoot.empty()) BeadsRecent::push(project->projectRoot);
        refreshTitleBar();
        refreshStatusBar();
        if (projectChangedHandler_) projectChangedHandler_(project_);
        return;
    }

    // Tear down any previous-project live-sync BEFORE changing state.
    // BeadsPoll's stop bumps its generation counter so any in-flight bd
    // completion for the old project becomes a no-op; the events tail
    // terminates its child process.
    if (poll_) { poll_->stop(); poll_.reset(); }
    if (tail_) { tail_->stop(); tail_.reset(); }
    renderFromBd_ = false;      // until the new project's probe says otherwise
    bcInFlight_ = false;
    bcPending_  = false;
    if (datasrcAction_) g_simple_action_set_enabled(datasrcAction_, FALSE);
    if (exportAction_)  g_simple_action_set_enabled(exportAction_, FALSE);
    project_ = project;
    ds_->bindToPath(project ? project->jsonlPath : "");
    watcher_->watchPath(project ? project->jsonlPath : "");
    activeDataSource_ = ds_.get();
    bdDataSource_.reset();
    bdRunner_.reset();

    selectDataSourceForProject();

    // Cross-session MRU feeding the switcher dropdown (shared with the
    // standalone BeadsViewer — macOS shares the same defaults key).
    if (project_ && !project_->projectRoot.empty())
        BeadsRecent::push(project_->projectRoot);

    refreshTitleBar();
    refreshStatusBar();
    rebuildProjectMenu();
    installUserScripts();

    // Reload only if the page is already settled (mirrors the macOS
    // crash-avoidance rule; a mid-load bind relies on the reinstalled script).
    if (viewerLoaded_) {
        viewerLoaded_ = false;
        webkit_web_view_reload(webView_);
    }
    if (projectChangedHandler_) projectChangedHandler_(project_);
}

void BeadsPanel::selectDataSourceForProject() {
    if (!project_ || project_->projectRoot.empty()) return;
    bdRunner_ = std::make_shared<BdCommandRunner>(project_->projectRoot);
    BdCommandRunner *probed = bdRunner_.get();
    bdRunner_->probe([this, probed](bool bdPresent, bool projectReady) {
        if (bdRunner_.get() != probed) return;   // superseded by a later bind
        if (bdPresent && projectReady) {
            bdDataSource_ = std::make_unique<BdDataSource>(bdRunner_);
            activeDataSource_ = bdDataSource_.get();
            // Data-source priority: default is render-from-Dolt; the
            // per-project jsonl escape hatch flips the view back to the file
            // (and reconciles it after every write).
            bool jsonlPri = BeadsRecent::jsonlPriorityFor(project_->projectRoot);
            renderFromBd_ = !jsonlPri;
            ds_->setFedExternally(renderFromBd_);
            probed->setReconcileJsonlAfterWrite(jsonlPri);
            if (datasrcAction_) {
                g_simple_action_set_enabled(datasrcAction_, TRUE);
                g_simple_action_set_state(datasrcAction_,
                    g_variant_new_string(jsonlPri ? "jsonl" : "dolt"));
            }
            if (exportAction_) g_simple_action_set_enabled(exportAction_, TRUE);
            projectMenuSig_.clear();   // menu gains the Data Source section
            g_message("[NppBeads] bd backend active — %s (render: %s)",
                      activeDataSource_->backendLabel().c_str(),
                      renderFromBd_ ? "dolt" : "jsonl");
            // Live sync: events feed on bd >= 1.3 (with a one-time
            // per-project opt-in prompt for the journal), else the 2s poll.
            startLiveSync(probed);
            // Upgrade the view from the bind-time file snapshot to the live
            // database (or re-read the file in jsonl mode).
            broadcastDataChanged();
        } else {
            renderFromBd_ = false;
            ds_->setFedExternally(false);
            g_message("[NppBeads] JSONL fallback (bd=%d, ready=%d)", bdPresent, projectReady);
        }
        refreshStatusBar();
    });
}

// ── live sync — events feed (bd >= 1.3, opt-in journal) with poll fallback ──
void BeadsPanel::startLiveSync(BdCommandRunner *runner) {
    if (!runner->supportsEventsFeed() || runner->bdPath().empty()) {
        startPoll(runner);
        return;
    }
    runner->fetchEventsJournalEnabled([this, runner](bool enabled) {
        if (bdRunner_.get() != runner) return;   // superseded by later bind
        if (enabled) startTail(runner);
        else         offerEventsFeed(runner);
    });
}

void BeadsPanel::startPoll(BdCommandRunner *runner) {
    if (poll_) poll_->stop();
    poll_ = std::make_unique<BeadsPoll>(bdRunner_, 2000);
    poll_->onChange = [this](const std::string &) { broadcastDataChanged(); };
    poll_->start();
    g_message("[NppBeads] live sync: 2s poll (bd %s)",
              runner->bdVersion().empty() ? "?" : runner->bdVersion().c_str());
}

void BeadsPanel::startTail(BdCommandRunner *runner) {
    if (tail_) { tail_->stop(); tail_.reset(); }
    std::string root = runner->projectDir();
    auto tail = std::make_unique<BdEventsTail>(runner->bdPath(), root);
    tail->setSinceSeq(BeadsRecent::eventsSeqFor(root));
    tail->onChange = [this, runner, root]() {
        if (bdRunner_.get() != runner || !tail_) return;
        BeadsRecent::setEventsSeq(root, tail_->lastSeq());
        g_message("[NppBeads] events feed change (records=%u seq=%llu) — rebroadcasting",
                  tail_->recordCount(), (unsigned long long)tail_->lastSeq());
        broadcastDataChanged();
    };
    tail->onBroken = [this, runner](const std::string &reason) {
        if (bdRunner_.get() != runner) return;
        g_message("[NppBeads] events feed broken (%s) — falling back to 2s poll",
                  reason.c_str());
        tail_.reset();
        startPoll(runner);
        broadcastDataChanged();      // resync once; we may have missed changes
    };
    tail_ = std::move(tail);
    tail_->start();
    g_message("[NppBeads] live sync: events feed (bd %s, since=%llu)",
              runner->bdVersion().c_str(), (unsigned long long)tail_->sinceSeq());
}

// One-time per-project prompt before flipping the journal on, because
// `bd config set events-journal true` writes .beads/config.yaml, which is
// often tracked in git. Any remembered answer suppresses the prompt forever;
// a user who later runs the config command themselves is picked up by the
// fetchEventsJournalEnabled check on the next bind.
void BeadsPanel::offerEventsFeed(BdCommandRunner *runner) {
    std::string root = runner->projectDir();
    if (!BeadsRecent::eventsAnswerFor(root).empty()) {
        // Already asked (or previously enabled and since turned off outside
        // the panel) — don't nag; poll.
        startPoll(runner);
        return;
    }
    GtkRoot *win = gtk_widget_get_root(root_);
    if (!GTK_IS_WINDOW(win)) {
        // No UI to ask with right now. Poll for this session WITHOUT
        // recording an answer, so the prompt can happen on a later bind.
        startPoll(runner);
        return;
    }
    GtkAlertDialog *a = gtk_alert_dialog_new("Enable live updates for this project?");
    std::string detail =
        "Beads " + (runner->bdVersion().empty() ? std::string("1.3") : runner->bdVersion()) +
        " can stream changes to this panel as they happen (bd events journal) "
        "instead of polling every 2 seconds.\n\n"
        "Enabling writes \"events-journal: true\" into this project's "
        ".beads/config.yaml — note that file may be tracked in git.\n\n"
        "You can also enable it later yourself:\n"
        "bd config set events-journal true";
    gtk_alert_dialog_set_detail(a, detail.c_str());
    const char *buttons[] = { "Enable Live Updates", "Not Now", nullptr };
    gtk_alert_dialog_set_buttons(a, buttons);
    gtk_alert_dialog_set_default_button(a, 0);
    gtk_alert_dialog_set_cancel_button(a, 1);
    struct OfferCtx { BeadsPanel *self; BdCommandRunner *runner; std::string root; };
    auto *ctx = new OfferCtx{ this, runner, root };
    gtk_alert_dialog_choose(a, GTK_WINDOW(win), nullptr,
        +[](GObject *src, GAsyncResult *res, gpointer user) {
            auto *ctx = static_cast<OfferCtx *>(user);
            BeadsPanel *self = ctx->self;
            BdCommandRunner *runner = ctx->runner;
            std::string root = ctx->root;
            delete ctx;
            int btn = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, nullptr);
            if (self->bdRunner_.get() != runner) return;   // rebound meanwhile
            if (btn == 0) {
                BeadsRecent::setEventsAnswer(root, "enabled");
                runner->enableEventsJournal([self, runner](const BdResultPtr &r) {
                    if (self->bdRunner_.get() != runner) return;
                    if (r->ok) {
                        self->startTail(runner);
                    } else {
                        g_message("[NppBeads] enabling events journal failed: %s — polling",
                                  r->errorMessage.c_str());
                        self->startPoll(runner);
                    }
                });
            } else {
                BeadsRecent::setEventsAnswer(root, "declined");
                self->startPoll(runner);
            }
        }, ctx);
    g_object_unref(a);
}

// ── Data Source priority (Dolt-live default · issues.jsonl escape hatch) ───
void BeadsPanel::setDataSourcePriorityJsonl(bool jsonlPriority) {
    if (!project_ || project_->projectRoot.empty()) { gtk_widget_error_bell(root_); return; }
    BeadsRecent::setJsonlPriority(project_->projectRoot, jsonlPriority);
    bool bdActive = bdDataSource_ && activeDataSource_ == bdDataSource_.get();
    renderFromBd_ = bdActive && !jsonlPriority;
    ds_->setFedExternally(renderFromBd_);
    if (bdRunner_) bdRunner_->setReconcileJsonlAfterWrite(bdActive && jsonlPriority);
    if (!renderFromBd_) ds_->reload();    // back to the file as the truth
    g_message("[NppBeads] data source priority → %s for %s (bd %s)",
              jsonlPriority ? "jsonl" : "dolt", project_->projectRoot.c_str(),
              bdActive ? "active" : "inactive");
    broadcastDataChanged();
}

void BeadsPanel::exportJsonlNow() {
    if (!bdRunner_ || !(bdDataSource_ && activeDataSource_ == bdDataSource_.get())) {
        gtk_widget_error_bell(root_);
        return;
    }
    BdCommandRunner *runner = bdRunner_.get();
    runner->exportJsonlToProjectFile([this, runner](const BdResultPtr &res) {
        if (bdRunner_.get() != runner) return;
        if (res->ok) {
            g_message("[NppBeads] exported issues.jsonl from Dolt (manual)");
            // The file may have just been created (Dolt-only workspace) —
            // (re)arm the watcher, then refresh the view if it's file-driven.
            if (project_ && !project_->jsonlPath.empty())
                watcher_->watchPath(project_->jsonlPath);
            if (!renderFromBd_) broadcastDataChanged();
        } else {
            GtkAlertDialog *a = gtk_alert_dialog_new("Export failed");
            gtk_alert_dialog_set_detail(a, res->errorMessage.empty()
                ? "bd export returned an error." : res->errorMessage.c_str());
            GtkRoot *w = gtk_widget_get_root(root_);
            gtk_alert_dialog_show(a, GTK_IS_WINDOW(w) ? GTK_WINDOW(w) : nullptr);
            g_object_unref(a);
        }
    });
}

void BeadsPanel::reloadData() {
    // Dolt-priority: "reload" means a fresh pull from the database, not a
    // file re-read. broadcastDataChanged owns that path (incl. coalescing).
    if (renderFromBd_ && bdRunner_) { broadcastDataChanged(); return; }
    ds_->reload();
    refreshStatusBar();
    std::string fresh = ds_->rawText();
    size_t len = fresh.size();
    if (len == jsonlBytesLastSeen_ && viewerLoaded_) return;   // unchanged
    jsonlBytesLastSeen_ = len;
    installUserScripts();
    if (viewerLoaded_) broadcastDataChanged();
}

void BeadsPanel::prepareForShow() {
    setViewMode(BeadsViewMode::Dashboard, false);
    gtk_editable_set_text(GTK_EDITABLE(searchEntry_), "");
    lastSearchQuery_ = "";
    refreshTitleBar();
    if (viewerLoaded_) {
        runJS("if (typeof window.__nppClearTransientState === 'function') { window.__nppClearTransientState(); }"
              "if (location.hash !== '#/') location.hash = '/';");
        return;
    }
    installUserScripts();
    viewerLoaded_ = false;
    webkit_web_view_load_uri(webView_, urlForViewMode(BeadsViewMode::Dashboard).c_str());
}

void BeadsPanel::showBeadDetail(const std::string &beadId) {
    if (beadId.empty()) { gtk_widget_error_bell(root_); return; }
    std::string lit = beads_jsStringLiteral(beadId);
    std::string js =
        "if (window.__nppApp && typeof window.__nppApp.openBeadModalById === 'function') {"
        "  window.__nppApp.openBeadModalById(" + lit + ");"
        "} else { window.__nppBeadsPendingModalId = " + lit + "; }";
    runOnBoardView(js);
}

void BeadsPanel::showCreateIssueWithTitle(const std::string &title) {
    // Title must be one line.
    std::string t = title;
    for (char &c : t) if (c == '\n' || c == '\r') c = ' ';
    std::string lit = beads_jsStringLiteral(t);
    std::string js =
        "if (window.__nppApp && typeof window.__nppApp.openNewIssueWithTitle === 'function') {"
        "  window.__nppApp.openNewIssueWithTitle(" + lit + ");"
        "} else { window.__nppBeadsPendingCreateTitle = " + lit + "; }";
    runOnBoardView(js);
}

void BeadsPanel::noteFileActivated(const std::string &filePath) {
    if (filePath.empty()) return;
    seenFilePaths_.insert(filePath);
}

void BeadsPanel::openBeadsDir() {
    if (!project_ || project_->beadsDir.empty()) return;
    std::string uri = "file://" + project_->beadsDir;
    GError *err = nullptr;
    g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &err);
    if (err) { g_message("[NppBeads] openBeadsDir: %s", err->message); g_error_free(err); }
}

void BeadsPanel::setWindowActive(bool active) {
    if (active) {
        if (poll_) poll_->resume();
        if (tail_) tail_->resume();   // flushes any change that arrived while blurred
    } else {
        if (poll_) poll_->pause();
        if (tail_) tail_->pause();
    }
}

std::vector<std::shared_ptr<BeadsProject>> BeadsPanel::discoverProjects(size_t max) {
    std::vector<std::string> paths(seenFilePaths_.begin(), seenFilePaths_.end());
    return BeadsProjectScanner::discoverUniqueProjectsFromPaths(paths, max);
}

// ── status / title ───────────────────────────────────────────────────────────
void BeadsPanel::refreshTitleBar() {
    std::string label = project_ ? (pathLeaf(project_->projectRoot) + " ▾") : "(no project) ▾";
    gtk_menu_button_set_label(GTK_MENU_BUTTON(projectChip_), label.c_str());
    if (project_) gtk_widget_set_tooltip_text(projectChip_, project_->projectRoot.c_str());

    // Board + Activity filter their lists; on Graph it is Find-node.
    // Dashboard / Insights are aggregates.
    bool showSearch = (viewMode_ == BeadsViewMode::Board ||
                       viewMode_ == BeadsViewMode::Activity ||
                       viewMode_ == BeadsViewMode::Graph);
    gtk_widget_set_visible(searchEntry_, showSearch);
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(searchEntry_),
        viewMode_ == BeadsViewMode::Graph ? "Find node" : "Search beads");
    if (!showSearch && !lastSearchQuery_.empty()) {
        gtk_editable_set_text(GTK_EDITABLE(searchEntry_), "");
        lastSearchQuery_ = "";
    }
}

void BeadsPanel::refreshStatusBar() {
    std::string text;
    if (!project_) {
        text = "no project · click the project name ▾ to pick one";
    } else if (project_->jsonlPath.empty()) {
        text = pathLeaf(project_->projectRoot) + " · no issues.jsonl yet";
    } else {
        size_t total = ds_->issueCount(), open = ds_->openIssueCount(),
               blk = ds_->blockedIssueCount(), cls = ds_->closedIssueCount();
        std::string backend = activeDataSource_ ? activeDataSource_->backendLabel() : "read-only (JSONL)";
        if (bdDataSource_ && activeDataSource_ == bdDataSource_.get())
            // Surface which source feeds the view so the mode is never invisible.
            backend += renderFromBd_ ? " · dolt" : " · jsonl";
        char buf[512];
        g_snprintf(buf, sizeof buf,
            "%s · %zu issues (%zu open · %zu blocked · %zu closed) · %s",
            pathLeaf(project_->projectRoot).c_str(), total, open, blk, cls, backend.c_str());
        text = buf;
    }
    gtk_label_set_text(GTK_LABEL(statusLabel_), text.c_str());
}

// ── bridge dispatch ──────────────────────────────────────────────────────────
void BeadsPanel::onScriptMessage(JSCValue *msg) {
    if (!msg || !jsc_value_is_object(msg)) return;
    std::string type = jscStr(msg, "type");
    std::string reqId = jscStr(msg, "reqId");
    if (type.empty()) return;

    if (type == "getJsonl") { pushJsonlToBridge(); return; }
    if (type == "openExternal") {
        std::string url = jscStr(msg, "url");
        if (!url.empty()) g_app_info_launch_default_for_uri(url.c_str(), nullptr, nullptr);
        return;
    }
    if (type == "openBeadDetails") {
        // Issue details live in the Activity view's modal
        // (activity.html?open=<id>) — the Issues view is retired.
        std::string id = jscStr(msg, "id");
        if (id.empty()) return;
        setViewMode(BeadsViewMode::Activity, false);
        gchar *enc = g_uri_escape_string(id.c_str(), nullptr, FALSE);
        std::string url = urlForViewMode(BeadsViewMode::Activity) +
                          "?open=" + (enc ? enc : "");
        g_free(enc);
        viewerLoaded_ = false;
        installUserScripts();
        webkit_web_view_load_uri(webView_, url.c_str());
        return;
    }
    if (type == "openBeadModal") { showBeadDetail(jscStr(msg, "id")); return; }
    if (type == "zoomIn")   { double z = webkit_web_view_get_zoom_level(webView_) + 0.10; if (z > 2.0) z = 2.0; webkit_web_view_set_zoom_level(webView_, z); return; }
    if (type == "zoomOut")  { double z = webkit_web_view_get_zoom_level(webView_) - 0.10; if (z < 0.5) z = 0.5; webkit_web_view_set_zoom_level(webView_, z); return; }
    if (type == "zoomReset"){ webkit_web_view_set_zoom_level(webView_, 0.80); return; }

    if (type == "createBead")  { handleCreate(msg, reqId); return; }
    if (type == "updateBead")  { handleUpdate(msg, reqId); return; }
    if (type == "closeBead")   { handleClose(msg, reqId); return; }
    if (type == "reopenBead")  { handleReopen(msg, reqId); return; }
    if (type == "claimBead")   { handleClaim(msg, reqId); return; }
    if (type == "depAdd")      { handleDepAdd(msg, reqId); return; }
    if (type == "depRemove")   { handleDepRemove(msg, reqId); return; }
    if (type == "deleteBead")  { handleDelete(msg, reqId); return; }
    if (type == "unassignBead"){ handleUnassign(msg, reqId); return; }
    if (type == "addComment")  { handleAddComment(msg, reqId); return; }
    if (type == "fetchBead")   { handleFetchBead(msg, reqId); return; }
    // unknown → ignore (forward-compat)
}

void BeadsPanel::resolveRequest(const std::string &reqId, bool ok, JsonNode *bead, const BeadsError *err) {
    if (reqId.empty()) return;
    JsonObject *payload = json_object_new();
    json_object_set_boolean_member(payload, "ok", ok);
    if (bead && JSON_NODE_HOLDS_OBJECT(bead))
        json_object_set_member(payload, "bead", json_node_copy(bead));
    if (err && err->code != BeadsErrOK) {
        json_object_set_string_member(payload, "error",
            err->message.empty() ? "unknown" : err->message.c_str());
        json_object_set_int_member(payload, "errorKind", err->code);
        if (!err->blockers.empty()) {
            JsonArray *arr = json_array_new();
            for (auto &b : err->blockers) json_array_add_string_element(arr, b.c_str());
            json_object_set_array_member(payload, "blockers", arr);
        }
    }
    JsonNode *root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, payload);
    std::string jstr = beads::jsonToString(root);
    json_node_free(root);

    // reqId is a bridge-generated token (rNN_xxx); escape defensively anyway.
    std::string reqLit = beads_jsStringLiteral(reqId);
    std::string js =
        "if (window.__nppBridge && typeof window.__nppBridge.resolve === 'function') {"
        "  window.__nppBridge.resolve(" + reqLit + ", " + jstr + ");"
        "}";
    runJS(js);
}

// ── write handlers ───────────────────────────────────────────────────────────
void BeadsPanel::handleCreate(JSCValue *m, const std::string &reqId) {
    std::string title = jscStr(m, "title");
    std::string type = jscStr(m, "issueType");     // NOT "type" (envelope collision)
    std::string desc = jscStr(m, "description");
    int prio = 0; bool hasPrio = jscHasNumber(m, "priority", &prio);
    auto labels = jscStrArray(m, "labels");
    activeDataSource_->createIssue(title,
        type.empty() ? nullptr : type.c_str(),
        hasPrio ? &prio : nullptr,
        jscHas(m, "description") ? desc.c_str() : nullptr,
        labels,
        [this, reqId](JsonNode *bead, const BeadsError &err) {
            resolveRequest(reqId, err.ok(), bead, err.ok() ? nullptr : &err);
            if (err.ok()) broadcastDataChanged();
        });
}

void BeadsPanel::handleUpdate(JSCValue *m, const std::string &reqId) {
    std::string id = jscStr(m, "id");
    if (id.empty()) { BeadsError e{BeadsErrGeneric, "missing id", {}}; resolveRequest(reqId, false, nullptr, &e); return; }
    std::string title = jscStr(m, "title"), desc = jscStr(m, "description"),
                status = jscStr(m, "status"), type = jscStr(m, "issueType"),
                assignee = jscStr(m, "assignee");
    int prio = 0; bool hasPrio = jscHasNumber(m, "priority", &prio);
    auto add = jscStrArray(m, "addLabels"), rem = jscStrArray(m, "removeLabels");
    activeDataSource_->updateIssue(id,
        jscHas(m, "title") ? title.c_str() : nullptr,
        jscHas(m, "description") ? desc.c_str() : nullptr,
        jscHas(m, "status") ? status.c_str() : nullptr,
        hasPrio ? &prio : nullptr,
        jscHas(m, "issueType") ? type.c_str() : nullptr,
        jscHas(m, "assignee") ? assignee.c_str() : nullptr,
        add, rem,
        [this, reqId](JsonNode *bead, const BeadsError &err) {
            resolveRequest(reqId, err.ok(), bead, err.ok() ? nullptr : &err);
            if (err.ok()) broadcastDataChanged();
        });
}

void BeadsPanel::handleClose(JSCValue *m, const std::string &reqId) {
    std::string id = jscStr(m, "id"), reason = jscStr(m, "reason");
    bool force = jscBool(m, "force");
    activeDataSource_->closeIssue(id, jscHas(m, "reason") ? reason.c_str() : nullptr, force,
        [this, reqId](JsonNode *bead, const BeadsError &err) {
            resolveRequest(reqId, err.ok(), bead, err.ok() ? nullptr : &err);
            if (err.ok()) broadcastDataChanged();
        });
}

void BeadsPanel::handleReopen(JSCValue *m, const std::string &reqId) {
    std::string id = jscStr(m, "id"), reason = jscStr(m, "reason");
    activeDataSource_->reopenIssue(id, jscHas(m, "reason") ? reason.c_str() : nullptr,
        [this, reqId](JsonNode *bead, const BeadsError &err) {
            resolveRequest(reqId, err.ok(), bead, err.ok() ? nullptr : &err);
            if (err.ok()) broadcastDataChanged();
        });
}

void BeadsPanel::handleClaim(JSCValue *m, const std::string &reqId) {
    activeDataSource_->claimIssue(jscStr(m, "id"),
        [this, reqId](JsonNode *bead, const BeadsError &err) {
            resolveRequest(reqId, err.ok(), bead, err.ok() ? nullptr : &err);
            if (err.ok()) broadcastDataChanged();
        });
}

void BeadsPanel::handleDepAdd(JSCValue *m, const std::string &reqId) {
    std::string dep = jscStr(m, "dependent"), dpd = jscStr(m, "dependency"),
                kind = jscStr(m, "depType");     // NOT "type"
    activeDataSource_->addDependency(dep, dpd, kind.empty() ? "blocks" : kind,
        [this, reqId](const BeadsError &err) {
            resolveRequest(reqId, err.ok(), nullptr, err.ok() ? nullptr : &err);
            if (err.ok()) broadcastDataChanged();
        });
}

void BeadsPanel::handleDepRemove(JSCValue *m, const std::string &reqId) {
    std::string dep = jscStr(m, "dependent"), dpd = jscStr(m, "dependency");
    activeDataSource_->removeDependency(dep, dpd,
        [this, reqId](const BeadsError &err) {
            resolveRequest(reqId, err.ok(), nullptr, err.ok() ? nullptr : &err);
            if (err.ok()) broadcastDataChanged();
        });
}

void BeadsPanel::handleDelete(JSCValue *m, const std::string &reqId) {
    std::string id = jscStr(m, "id");
    if (id.empty()) { BeadsError e{BeadsErrGeneric, "missing id", {}}; resolveRequest(reqId, false, nullptr, &e); return; }
    activeDataSource_->deleteIssue(id,
        [this, reqId](const BeadsError &err) {
            resolveRequest(reqId, err.ok(), nullptr, err.ok() ? nullptr : &err);
            if (err.ok()) broadcastDataChanged();
        });
}

void BeadsPanel::handleUnassign(JSCValue *m, const std::string &reqId) {
    activeDataSource_->unassignIssue(jscStr(m, "id"),
        [this, reqId](JsonNode *bead, const BeadsError &err) {
            resolveRequest(reqId, err.ok(), bead, err.ok() ? nullptr : &err);
            if (err.ok()) broadcastDataChanged();
        });
}

void BeadsPanel::handleAddComment(JSCValue *m, const std::string &reqId) {
    std::string id = jscStr(m, "id"), body = jscStr(m, "body");
    if (id.empty()) { BeadsError e{BeadsErrGeneric, "missing id", {}}; resolveRequest(reqId, false, nullptr, &e); return; }
    if (body.empty()) { BeadsError e{BeadsErrGeneric, "empty comment body", {}}; resolveRequest(reqId, false, nullptr, &e); return; }
    activeDataSource_->addComment(id, body,
        [this, reqId](const BeadsError &err) {
            resolveRequest(reqId, err.ok(), nullptr, err.ok() ? nullptr : &err);
            if (err.ok()) broadcastDataChanged();
        });
}

void BeadsPanel::handleFetchBead(JSCValue *m, const std::string &reqId) {
    activeDataSource_->showIssue(jscStr(m, "id"),
        [this, reqId](JsonNode *bead, const BeadsError &err) {
            resolveRequest(reqId, err.ok(), bead, err.ok() ? nullptr : &err);
            // read-only: no broadcast
        });
}

// ── signal trampolines ───────────────────────────────────────────────────────
void BeadsPanel::s_scriptMessage(WebKitUserContentManager *, JSCValue *v, gpointer self) {
    static_cast<BeadsPanel *>(self)->onScriptMessage(v);
}

void BeadsPanel::s_loadChanged(WebKitWebView *, WebKitLoadEvent ev, gpointer self) {
    auto *p = static_cast<BeadsPanel *>(self);
    if (ev != WEBKIT_LOAD_FINISHED) return;
    p->viewerLoaded_ = true;
    if (p->project_) p->pushJsonlToBridge();
    p->pushTheme();
    if (!p->lastSearchQuery_.empty()) p->pushSearchQuery(p->lastSearchQuery_);
    if (!p->pendingPostLoadJS_.empty()) {
        // Board/Activity define __nppApp; flush queued modal/create JS there.
        const char *uri = webkit_web_view_get_uri(p->webView_);
        if (uri && (strstr(uri, "/app/board.html") || strstr(uri, "/app/activity.html"))) {
            p->runJS(p->pendingPostLoadJS_);
            p->pendingPostLoadJS_.clear();
        }
    }
}

gboolean BeadsPanel::s_decidePolicy(WebKitWebView *, WebKitPolicyDecision *dec,
                                    WebKitPolicyDecisionType type, gpointer) {
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return FALSE;
    WebKitNavigationPolicyDecision *nav = WEBKIT_NAVIGATION_POLICY_DECISION(dec);
    WebKitNavigationAction *act = webkit_navigation_policy_decision_get_navigation_action(nav);
    if (webkit_navigation_action_get_navigation_type(act) != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED) {
        webkit_policy_decision_use(dec);
        return TRUE;
    }
    WebKitURIRequest *req = webkit_navigation_action_get_request(act);
    const char *uri = req ? webkit_uri_request_get_uri(req) : nullptr;
    if (uri && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))) {
        g_app_info_launch_default_for_uri(uri, nullptr, nullptr);
        webkit_policy_decision_ignore(dec);
        return TRUE;
    }
    webkit_policy_decision_use(dec);
    return TRUE;
}

void BeadsPanel::s_refreshClicked(GtkButton *, gpointer self) {
    static_cast<BeadsPanel *>(self)->reloadData();
}

void BeadsPanel::s_themeClicked(GtkButton *, gpointer self) {
    auto *p = static_cast<BeadsPanel *>(self);
    // Auto → Light → Dark → Auto
    const char *name;
    switch (p->themePref_) {
        case BeadsThemePref::Auto:  p->themePref_ = BeadsThemePref::Light; name = "Theme: Light"; break;
        case BeadsThemePref::Light: p->themePref_ = BeadsThemePref::Dark;  name = "Theme: Dark";  break;
        default:                    p->themePref_ = BeadsThemePref::Auto;  name = "Theme: Auto";  break;
    }
    gtk_widget_set_tooltip_text(p->themeBtn_, name);
    p->pushTheme();
}

void BeadsPanel::s_viewModeChanged(GObject *dropdown, GParamSpec *, gpointer self) {
    auto *p = static_cast<BeadsPanel *>(self);
    if (p->settingDropdown_) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dropdown));
    p->setViewMode((BeadsViewMode)sel, true);
}

void BeadsPanel::s_searchChanged(GtkSearchEntry *entry, gpointer self) {
    const char *t = gtk_editable_get_text(GTK_EDITABLE(entry));
    static_cast<BeadsPanel *>(self)->pushSearchQuery(t ? t : "");
}
