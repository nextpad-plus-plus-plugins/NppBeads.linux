// BeadsPanel.cpp — see header. GTK4 + WebKitGTK 6.0 port of BeadsPanel.mm.

#include "BeadsPanel.h"
#include <adwaita.h>
#include <gio/gio.h>
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
    // Parameterized: pick a discovered project by its beadsDir.
    GSimpleAction *pick = g_simple_action_new("pickproject", G_VARIANT_TYPE_STRING);
    g_signal_connect(pick, "activate",
        G_CALLBACK(+[](GSimpleAction *, GVariant *param, gpointer s){
            const char *beadsDir = g_variant_get_string(param, nullptr);
            auto *p = static_cast<BeadsPanel*>(s);
            auto proj = BeadsProjectScanner::projectFromBeadsDir(beadsDir);
            if (proj) p->bindProject(proj);
        }), this);
    g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(pick));
    g_object_unref(pick);
    gtk_widget_insert_action_group(root_, "beads", G_ACTION_GROUP(ag));
    g_object_unref(ag);
    rebuildProjectMenu();

    // View-mode dropdown.
    const char *modes[] = { "Dashboard", "Issues", "Insights", "Graph", "Board", "Activity", nullptr };
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

void BeadsPanel::rebuildProjectMenu() {
    GMenu *menu = g_menu_new();

    // Discovered projects section.
    auto projects = discoverProjects(12);
    if (!projects.empty()) {
        GMenu *sec = g_menu_new();
        for (auto &p : projects) {
            std::string label = pathLeaf(p->projectRoot);
            GMenuItem *item = g_menu_item_new(label.c_str(), nullptr);
            g_menu_item_set_action_and_target_value(item, "beads.pickproject",
                g_variant_new_string(p->beadsDir.c_str()));
            g_menu_append_item(sec, item);
            g_object_unref(item);
        }
        g_menu_append_section(menu, "Projects", G_MENU_MODEL(sec));
        g_object_unref(sec);
    }

    GMenu *actions = g_menu_new();
    g_menu_append(actions, "Reload issues from disk", "beads.reload");
    g_menu_append(actions, "Reload viewer",           "beads.reloadviewer");
    g_menu_append(actions, "Reveal .beads/ folder",   "beads.reveal");
    if (project_) g_menu_append(actions, "Unbind current project", "beads.unbind");
    g_menu_append_section(menu, nullptr, G_MENU_MODEL(actions));
    g_object_unref(actions);

    if (hideHandler_) {
        GMenu *hide = g_menu_new();
        g_menu_append(hide, "Hide panel", "beads.hide");
        g_menu_append_section(menu, nullptr, G_MENU_MODEL(hide));
        g_object_unref(hide);
    }

    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(projectChip_), G_MENU_MODEL(menu));
    g_object_unref(menu);
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
        case BeadsViewMode::Issues:    return "nppbeads://viewer/index.html#/issues";
        case BeadsViewMode::Insights:  return "nppbeads://viewer/index.html#/insights";
        case BeadsViewMode::Graph:     return "nppbeads://viewer/index.html#/graph";
        case BeadsViewMode::Board:     return "nppbeads://viewer/app/board.html";
        case BeadsViewMode::Activity:  return "nppbeads://viewer/app/activity.html";
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
    activeDataSource_->invalidateCache();
    ds_->reload();
    std::string fresh = ds_->rawText();
    refreshStatusBar();
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
    else if (viewMode_ == BeadsViewMode::Issues)
        runJS("if(window.__nppRichSearch)window.__nppRichSearch(" + lit + ");");
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
        refreshTitleBar();
        refreshStatusBar();
        if (projectChangedHandler_) projectChangedHandler_(project_);
        return;
    }

    if (poll_) { poll_->stop(); poll_.reset(); }
    project_ = project;
    ds_->bindToPath(project ? project->jsonlPath : "");
    watcher_->watchPath(project ? project->jsonlPath : "");
    activeDataSource_ = ds_.get();
    bdDataSource_.reset();
    bdRunner_.reset();

    selectDataSourceForProject();

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
            poll_ = std::make_unique<BeadsPoll>(bdRunner_, 2000);
            poll_->onChange = [this](const std::string &) { broadcastDataChanged(); };
            poll_->start();
            g_message("[NppBeads] bd backend active — %s", activeDataSource_->backendLabel().c_str());
        } else {
            g_message("[NppBeads] JSONL fallback (bd=%d, ready=%d)", bdPresent, projectReady);
        }
        refreshStatusBar();
    });
}

void BeadsPanel::reloadData() {
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
    if (!poll_) return;
    if (active) poll_->resume();
    else        poll_->pause();
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

    bool showSearch = (viewMode_ == BeadsViewMode::Board ||
                       viewMode_ == BeadsViewMode::Issues ||
                       viewMode_ == BeadsViewMode::Activity);
    gtk_widget_set_visible(searchEntry_, showSearch);
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
        std::string id = jscStr(msg, "id");
        if (id.empty()) return;
        setViewMode(BeadsViewMode::Dashboard, false);
        gchar *enc = g_uri_escape_string(id.c_str(), nullptr, FALSE);
        std::string url = std::string("nppbeads://viewer/index.html#/issue/") + (enc ? enc : "");
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
