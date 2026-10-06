// NppBeads — Nextpad++ (GTK4 Linux) plugin embedding the bundled beads_viewer
// in a docked WebKitGTK panel. Linux port of NppBeads.mm.
//
// Auto-detects `.beads/` above the active file, reads issues.jsonl (and drives
// the `bd` CLI when present), and paints link-style indicators under bead ids
// in the editor. Phase-5 editor integration: jump/copy/create-from-selection.

#include <gtk/gtk.h>
#include <dlfcn.h>
#include <string>

#include "Scintilla.h"   // SCNotification, SCI_*, SCN_* codes

extern "C" {
#include "plugin.h"     // host contract: FuncItem, NppData, NPPM_/NPPN_
}

#include "BeadsPanel.h"
#include "BeadIdIndicator.h"
#include "BeadsProjectScanner.h"

#define NPP_EXPORT __attribute__((visibility("default")))

extern "C" intptr_t scintilla_view_send_message(void *view, unsigned int msg,
                                                uintptr_t wParam, intptr_t lParam);

// ── plugin identity + menu slots ─────────────────────────────────────────────
static const char *PLUGIN_NAME = "NppBeads";

enum CmdIdx {
    kCmdShowPanel = 0, kCmdReload, kCmdOpenDir, kCmdSep1,
    kCmdJumpToBead, kCmdCopyBeadId, kCmdCreateFromSel, kCmdSep2,
    kCmdAbout, kCmdCount
};

static FuncItem sFuncItem[kCmdCount];
static NppData  sNpp;

// ── state ────────────────────────────────────────────────────────────────────
static BeadsPanel  *sPanel = nullptr;
static long         g_panelHandle = 0;
static bool         sPanelVisible = false;
static std::string  sResourcesDir;
static std::shared_ptr<BeadsProject> sCurrentProject;
static BeadIdIndicator *sIndicator = nullptr;

static void cmdTogglePanel();

static inline long npp(unsigned int msg, unsigned long w = 0, long l = 0) {
    return sNpp.hostMsg(msg, w, l);
}

// ── WebKit sandbox probe (Ubuntu 24.04 AppArmor userns restriction) ─────────
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sched.h>
#include <cstdio>

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
    g_message("[NppBeads] user namespaces unavailable (AppArmor userns "
              "restriction?) — disabling the WebKit sandbox so the panel can run");
    g_setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", TRUE);
}

// ── helpers ──────────────────────────────────────────────────────────────────
static std::string resolveResourcesDir() {
    Dl_info info;
    if (dladdr((const void *)&resolveResourcesDir, &info) && info.dli_fname) {
        std::string p = info.dli_fname;
        size_t slash = p.rfind('/');
        if (slash != std::string::npos) return p.substr(0, slash) + "/resources";
    }
    return "";
}

static std::string currentFullPath() {
    char buf[2048] = {0};
    npp(NPPM_GETFULLCURRENTPATH, (unsigned long)(sizeof(buf) - 1), (long)(intptr_t)buf);
    return std::string(buf);
}

static void *currentScintilla() {
    int which = 0;
    return (void *)(intptr_t)npp(NPPM_GETCURRENTSCINTILLA, 0, (long)(intptr_t)&which);
}

// ── panel hosting ────────────────────────────────────────────────────────────
static void ensurePanel() {
    if (sPanel) return;
    ensureWebKitCanLaunch();   // must run before the first WebKit object
    sPanel = new BeadsPanel(sResourcesDir);
    sPanel->setHideHandler([]() { if (sPanelVisible) cmdTogglePanel(); });
}

static bool panelIsShown() {
    return sPanel && sPanelVisible;
}

// ── project detection ────────────────────────────────────────────────────────
static void notePathActivated() {
    if (!sPanel) return;
    std::string path = currentFullPath();
    if (!path.empty()) sPanel->noteFileActivated(path);
}

static void rescanProjectFromCurrentBuffer() {
    if (!sPanel) return;
    std::string path = currentFullPath();
    // Never auto-rebind on a .beads/ internal file (see macOS note: races the
    // SCN storm on large issues.jsonl). The switcher covers that intent.
    if (!path.empty() && path.find("/.beads/") != std::string::npos) return;

    auto proj = path.empty() ? nullptr : BeadsProjectScanner::findProjectFromPath(path);
    if (!proj) return;   // leave current binding alone
    auto cur = sPanel->project();
    if (cur && !cur->beadsDir.empty() && cur->beadsDir == proj->beadsDir) return;
    sCurrentProject = proj;
    sPanel->bindProject(proj);
}

// ── editor-integration helpers ───────────────────────────────────────────────
static bool shouldRunIndicatorOnCurrentBuffer() {
    std::string path = currentFullPath();
    if (path.empty()) return true;
    if (path.find("/.beads/") != std::string::npos) return false;
    if (sCurrentProject && !sCurrentProject->jsonlPath.empty() &&
        path == sCurrentProject->jsonlPath) return false;
    return true;
}

static std::string beadIdAtCurrentCaret() {
    if (!sIndicator) return "";
    void *h = currentScintilla();
    if (!h) return "";
    intptr_t caret = scintilla_view_send_message(h, SCI_GETCURRENTPOS, 0, 0);
    std::string id = sIndicator->beadIdAtPosition(caret);
    if (!id.empty()) return id;

    // Fallback: scan a ±64-byte window around the caret directly.
    intptr_t start = caret - 64; if (start < 0) start = 0;
    intptr_t len = scintilla_view_send_message(h, SCI_GETLENGTH, 0, 0);
    intptr_t end = caret + 64; if (end > len) end = len;
    if (end <= start) return "";
    std::string window;
    window.reserve((size_t)(end - start));
    for (intptr_t i = start; i < end; i++)
        window.push_back((char)scintilla_view_send_message(h, SCI_GETCHARAT, (uintptr_t)i, 0));

    // Match bd-id tokens; pick the one covering the caret.
    std::string prefix = sIndicator->prefix();
    GError *err = nullptr;
    gchar *esc = g_regex_escape_string(prefix.c_str(), -1);
    std::string pat = std::string("\\b") + (esc ? esc : "bd-") + "[a-z0-9]+(\\.\\d+)*\\b";
    g_free(esc);
    GRegex *rx = g_regex_new(pat.c_str(), (GRegexCompileFlags)0, (GRegexMatchFlags)0, &err);
    if (!rx) { if (err) g_error_free(err); return ""; }
    GMatchInfo *mi = nullptr;
    std::string best;
    intptr_t caretOff = caret - start;
    if (g_regex_match(rx, window.c_str(), (GRegexMatchFlags)0, &mi)) {
        while (g_match_info_matches(mi)) {
            gint s = 0, e = 0;
            if (g_match_info_fetch_pos(mi, 0, &s, &e) && caretOff >= s && caretOff <= e) {
                gchar *m = g_match_info_fetch(mi, 0);
                if (m) best = m; g_free(m);
                break;
            }
            g_match_info_next(mi, nullptr);
        }
    }
    g_match_info_free(mi);
    g_regex_unref(rx);
    return best;
}

static std::string currentSelectionText() {
    void *h = currentScintilla();
    if (!h) return "";
    intptr_t s = scintilla_view_send_message(h, SCI_GETSELECTIONSTART, 0, 0);
    intptr_t e = scintilla_view_send_message(h, SCI_GETSELECTIONEND, 0, 0);
    if (e <= s) return "";
    intptr_t len = e - s; if (len > 4096) len = 4096;
    std::string buf;
    buf.reserve((size_t)len);
    for (intptr_t i = 0; i < len; i++)
        buf.push_back((char)scintilla_view_send_message(h, SCI_GETCHARAT, (uintptr_t)(s + i), 0));
    // Trim + collapse internal whitespace to single spaces (one-line title).
    gchar *stripped = g_strstrip(g_strdup(buf.c_str()));
    std::string out;
    bool inWs = false;
    for (char *c = stripped; *c; c++) {
        if (g_ascii_isspace(*c)) { if (!inWs && !out.empty()) out.push_back(' '); inWs = true; }
        else { out.push_back(*c); inWs = false; }
    }
    g_free(stripped);
    // rstrip trailing space
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// ── menu commands ────────────────────────────────────────────────────────────
static void cmdTogglePanel() {
    ensurePanel();

    if (g_panelHandle == 0) {
        // Linux ABI: wParam = title, lParam = widget (reversed vs macOS).
        g_panelHandle = npp(NPPM_DMM_REGISTERPANEL,
                            (unsigned long)(uintptr_t)"NppBeads",
                            (long)(intptr_t)sPanel->widget());
        if (g_panelHandle == 0) { g_warning("[NppBeads] panel registration failed"); return; }
        // Declare the reopen command so the host restores the panel after a
        // restart (GH linux#18): module = getName() ("NppBeads"), cmdIndex 0
        // = "Show Beads panel". Hosts < 1.1.0 return 0 — ignored.
        NppPanelInfo info;
        info.moduleName = PLUGIN_NAME;
        info.cmdIndex   = 0;
        npp(NPPM_DMM_SETPANELINFO, (unsigned long)(uintptr_t)g_panelHandle,
            (long)(intptr_t)&info);
    }

    bool target = !panelIsShown();
    sPanelVisible = target;
    npp(NPPM_SETMENUITEMCHECK, (unsigned long)sFuncItem[kCmdShowPanel].cmdID, target ? 1 : 0);

    if (target) {
        sPanel->prepareForShow();
        npp(NPPM_DMM_SHOWPANEL, (unsigned long)g_panelHandle, 0);
        rescanProjectFromCurrentBuffer();
    } else {
        npp(NPPM_DMM_HIDEPANEL, (unsigned long)g_panelHandle, 0);
    }
}

static void cmdReload() {
    if (!sPanel) { cmdTogglePanel(); return; }
    sPanel->reloadData();
}
static void cmdOpenDir() { if (sPanel) sPanel->openBeadsDir(); }

static void cmdJumpToBead() {
    std::string bid = beadIdAtCurrentCaret();
    if (bid.empty()) { gtk_widget_error_bell(GTK_WIDGET(sNpp.nppHandle)); return; }
    ensurePanel();
    if (!sPanelVisible) cmdTogglePanel();
    sPanel->showBeadDetail(bid);
}
static void cmdCopyBeadId() {
    std::string bid = beadIdAtCurrentCaret();
    if (bid.empty()) { gtk_widget_error_bell(GTK_WIDGET(sNpp.nppHandle)); return; }
    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(sNpp.nppHandle));
    gdk_clipboard_set_text(cb, bid.c_str());
}
static void cmdCreateFromSelection() {
    std::string sel = currentSelectionText();
    ensurePanel();
    if (!sPanelVisible) cmdTogglePanel();
    sPanel->showCreateIssueWithTitle(sel);
}

static void cmdAbout() {
    GtkAlertDialog *a = gtk_alert_dialog_new("NppBeads — Beads viewer panel");
    gtk_alert_dialog_set_detail(a,
        "Embeds the beads_viewer inside a dockable Nextpad++ side panel.\n"
        "Auto-detects .beads/ above the active file and reads issues.jsonl.\n"
        "Drives the `bd` CLI when present; fully offline otherwise.\n\n"
        "Beads: https://github.com/gastownhall/beads\n"
        "Viewer: https://github.com/dicklesworthstone/beads_viewer");
    gtk_alert_dialog_show(a, GTK_WINDOW(sNpp.nppHandle));
    g_object_unref(a);
}

// ── exports ──────────────────────────────────────────────────────────────────
extern "C" NPP_EXPORT void setInfo(NppData data) {
    sNpp = data;
    sResourcesDir = resolveResourcesDir();

    sIndicator = new BeadIdIndicator(scintilla_view_send_message);

    auto setItem = [&](int idx, const char *name, void (*fn)(void)) {
        g_strlcpy(sFuncItem[idx].itemName, name, sizeof sFuncItem[idx].itemName);
        sFuncItem[idx].pFunc = fn;
        sFuncItem[idx].init2Check = 0;
    };
    setItem(kCmdShowPanel,     "Show Beads panel",             cmdTogglePanel);
    setItem(kCmdReload,        "Reload issues",                cmdReload);
    setItem(kCmdOpenDir,       "Reveal .beads/ folder",        cmdOpenDir);
    setItem(kCmdSep1,          "-",                            nullptr);
    setItem(kCmdJumpToBead,    "Jump to bead under caret",     cmdJumpToBead);
    setItem(kCmdCopyBeadId,    "Copy bead id under caret",     cmdCopyBeadId);
    setItem(kCmdCreateFromSel, "Create issue from selection",  cmdCreateFromSelection);
    setItem(kCmdSep2,          "-",                            nullptr);
    setItem(kCmdAbout,         "About NppBeads",               cmdAbout);
}

extern "C" NPP_EXPORT const char *getName() { return PLUGIN_NAME; }

extern "C" NPP_EXPORT FuncItem *getFuncsArray(int *nbF) {
    *nbF = kCmdCount;
    return sFuncItem;
}

extern "C" NPP_EXPORT void beNotified(SCNotification *n) {
    if (!n) return;
    switch (n->nmhdr.code) {
        case NPPN_TBMODIFICATION:
        case NPPN_READY: {
            // Toolbar icon (lParam = full PNG path). TBMODIFICATION fires right
            // after READY on Linux; READY alone is enough, guard makes it safe.
            static bool iconDone = false;
            if (!iconDone) {
                iconDone = true;
                static std::string icon = sResourcesDir + "/toolbar.png";
                npp(NPPM_ADDTOOLBARICON_FORDARKMODE,
                    (unsigned long)sFuncItem[kCmdShowPanel].cmdID, (long)(intptr_t)icon.c_str());
            }
            break;
        }
        case NPPN_BUFFERACTIVATED:
        case NPPN_FILEOPENED:
            notePathActivated();
            if (sPanelVisible) rescanProjectFromCurrentBuffer();
            if (sIndicator) {
                sIndicator->setScintillaHandle(currentScintilla());
                if (shouldRunIndicatorOnCurrentBuffer()) sIndicator->rescanNow();
            }
            break;
        case SCN_MODIFIED:
        case SCN_UPDATEUI:
        case SCN_PAINTED:
            if (sIndicator && sIndicator->scintillaHandle() &&
                shouldRunIndicatorOnCurrentBuffer())
                sIndicator->scheduleRescan();
            break;
        case NPPN_SHUTDOWN:
            if (sIndicator) { sIndicator->clearAll(); delete sIndicator; sIndicator = nullptr; }
            if (g_panelHandle > 0) {
                npp(NPPM_DMM_UNREGISTERPANEL, (unsigned long)g_panelHandle, 0);
                g_panelHandle = 0;
            }
            if (sPanel) { delete sPanel; sPanel = nullptr; }
            sCurrentProject.reset();
            break;
        default: break;
    }
}

extern "C" NPP_EXPORT int isUnicode() { return 1; }

extern "C" NPP_EXPORT intptr_t messageProc(unsigned int, uintptr_t, intptr_t) { return 1; }
