// BeadsPanel — GTK4 + WebKitGTK panel embedding the bundled beads_viewer.
// Linux port of BeadsPanel.mm (the WKWebView panel). Reproduces the JS↔native
// bridge (single "beadsBridge" channel, flat {type,reqId,...} envelope,
// window.__nppBridge.resolve responses), the document-start JSONL/theme
// injection, the JSONL→bd data-source upgrade, the file watcher + live poll,
// and the write→broadcast data flow.

#pragma once

#include "beads_types.h"
#include "BeadsProjectScanner.h"
#include "JsonlDataSource.h"
#include "BdDataSource.h"
#include "BeadsWatcher.h"
#include "BeadsPoll.h"
#include "BdEventsTail.h"
#include "BeadsSchemeHandler.h"

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <functional>
#include <memory>
#include <set>
#include <string>

// View surfaces, in dropdown order (the Rich viewer's Issues list is retired;
// Activity hosts every issue's detail modal — macOS 30bf81d).
enum class BeadsViewMode { Dashboard = 0, Insights = 1, Activity = 2, Board = 3, Graph = 4 };
enum class BeadsThemePref { Auto = 0, Light = 1, Dark = 2 };

class BeadsPanel {
public:
    explicit BeadsPanel(const std::string &resourcesDir);
    ~BeadsPanel();

    GtkWidget *widget() const { return root_; }               // dock this
    std::shared_ptr<BeadsProject> project() const { return project_; }

    // Mirrors the macOS public API used by the plugin entry.
    void bindProject(std::shared_ptr<BeadsProject> project);
    void reloadData();
    void prepareForShow();
    void showBeadDetail(const std::string &beadId);
    void showCreateIssueWithTitle(const std::string &title);
    void noteFileActivated(const std::string &filePath);
    void openBeadsDir();

    void setHideHandler(std::function<void()> h) { hideHandler_ = std::move(h); }
    // Fired on every project change (incl. the in-app switcher) — the
    // standalone app uses it to retitle the window.
    void setProjectChangedHandler(std::function<void(std::shared_ptr<BeadsProject>)> h) {
        projectChangedHandler_ = std::move(h);
    }
    // Poll pause/resume follows host-window focus.
    void setWindowActive(bool active);

    // Discovered projects from the seen-file pool (for the switcher).
    std::vector<std::shared_ptr<BeadsProject>> discoverProjects(size_t max = 12);

    // Test/automation hook: evaluate arbitrary JS in the panel web view.
    void injectJS(const std::string &js);

private:
    // UI
    void buildUI();
    void setupWebView();
    GtkWidget *makeToolbar();
    void rebuildProjectMenu();
    void pickProjectRoot(const std::string &root);
    void openBeadsFolderDialog();
    void refreshStatusBar();
    void refreshTitleBar();

    // viewer / injection
    void installUserScripts();
    void loadViewer();
    std::string urlForViewMode(BeadsViewMode m) const;
    void setViewMode(BeadsViewMode m, bool navigate);
    void runJS(const std::string &js);
    void runOnBoardView(const std::string &js);
    void pushJsonlToBridge();
    void broadcastDataChanged();
    void pushJsonlToWebview(const std::string &fresh);
    void pushTheme();
    void pushSearchQuery(const std::string &q);
    bool isDark() const;

    // data source
    void selectDataSourceForProject();
    void setDataSourcePriorityJsonl(bool jsonlPriority);
    void exportJsonlNow();

    // live sync — events feed (bd >= 1.3, opt-in journal) with poll fallback
    void startLiveSync(BdCommandRunner *runner);
    void startPoll(BdCommandRunner *runner);
    void startTail(BdCommandRunner *runner);
    void offerEventsFeed(BdCommandRunner *runner);

    // bridge
    void onScriptMessage(JSCValue *msg);
    void resolveRequest(const std::string &reqId, bool ok, JsonNode *bead, const BeadsError *err);
    // write handlers
    void handleCreate(JSCValue *m, const std::string &reqId);
    void handleUpdate(JSCValue *m, const std::string &reqId);
    void handleClose(JSCValue *m, const std::string &reqId);
    void handleReopen(JSCValue *m, const std::string &reqId);
    void handleClaim(JSCValue *m, const std::string &reqId);
    void handleDepAdd(JSCValue *m, const std::string &reqId);
    void handleDepRemove(JSCValue *m, const std::string &reqId);
    void handleDelete(JSCValue *m, const std::string &reqId);
    void handleUnassign(JSCValue *m, const std::string &reqId);
    void handleAddComment(JSCValue *m, const std::string &reqId);
    void handleFetchBead(JSCValue *m, const std::string &reqId);

    // signal trampolines
    static void s_scriptMessage(WebKitUserContentManager *, JSCValue *v, gpointer self);
    static void s_loadChanged(WebKitWebView *, WebKitLoadEvent ev, gpointer self);
    static gboolean s_decidePolicy(WebKitWebView *, WebKitPolicyDecision *, WebKitPolicyDecisionType, gpointer self);
    static void s_refreshClicked(GtkButton *, gpointer self);
    static void s_themeClicked(GtkButton *, gpointer self);
    static void s_viewModeChanged(GObject *dropdown, GParamSpec *, gpointer self);
    static void s_searchChanged(GtkSearchEntry *, gpointer self);

    // state
    std::string resourcesDir_;
    GtkWidget *root_ = nullptr;
    GtkWidget *projectChip_ = nullptr;    // GtkMenuButton
    GMenu *projectMenu_ = nullptr;        // persistent model, repopulated in place
    std::string projectMenuSig_;          // content signature (skip no-op rebuilds)
    GtkWidget *viewModeDrop_ = nullptr;   // GtkDropDown
    GtkWidget *searchEntry_ = nullptr;
    GtkWidget *statusLabel_ = nullptr;
    GtkWidget *themeBtn_ = nullptr;
    WebKitWebView *webView_ = nullptr;
    WebKitWebContext *webContext_ = nullptr;
    WebKitUserContentManager *ucm_ = nullptr;

    std::unique_ptr<BeadsSchemeHandler> scheme_;
    std::shared_ptr<JsonlDataSource> ds_;
    std::unique_ptr<BeadsWatcher> watcher_;
    std::shared_ptr<BdCommandRunner> bdRunner_;
    std::unique_ptr<BdDataSource> bdDataSource_;
    BeadsDataSource *activeDataSource_ = nullptr;
    std::unique_ptr<BeadsPoll> poll_;
    std::unique_ptr<BdEventsTail> tail_;   // events-feed live sync (bd >= 1.3);
                                           // mutually exclusive with poll_
    GSimpleAction *datasrcAction_ = nullptr;   // owned by the action group
    GSimpleAction *exportAction_ = nullptr;

    // Data-source priority: when true the webview renders from the Dolt DB
    // (`bd export` to stdout) instead of the issues.jsonl file. Default on in
    // bd mode; per-project jsonl-priority flips it off (escape hatch).
    bool renderFromBd_ = false;
    bool bcInFlight_ = false;   // one dolt-export broadcast at a time
    bool bcPending_ = false;    // coalesce broadcasts that arrive mid-flight

    std::shared_ptr<BeadsProject> project_;
    std::set<std::string> seenFilePaths_;

    BeadsViewMode viewMode_ = BeadsViewMode::Dashboard;
    BeadsThemePref themePref_ = BeadsThemePref::Auto;
    std::string lastSearchQuery_;
    std::string pendingPostLoadJS_;
    bool viewerLoaded_ = false;
    bool settingDropdown_ = false;
    size_t jsonlBytesLastSeen_ = (size_t)-1;

    std::function<void()> hideHandler_;
    std::function<void(std::shared_ptr<BeadsProject>)> projectChangedHandler_;
};

// Exact reproduction of the macOS jsStringLiteral (incl. U+2028/U+2029).
std::string beads_jsStringLiteral(const std::string &s);
