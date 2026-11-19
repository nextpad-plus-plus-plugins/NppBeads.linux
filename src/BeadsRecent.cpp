// BeadsRecent.cpp — see header.

#include "BeadsRecent.h"
#include <glib.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>

namespace BeadsRecent {

static std::string iniPath() {
    return std::string(g_get_user_config_dir()) + "/beadsviewer/recent.ini";
}

std::vector<std::string> load() {
    std::vector<std::string> out;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, iniPath().c_str(), G_KEY_FILE_NONE, nullptr)) {
        gsize n = 0;
        gchar **arr = g_key_file_get_string_list(kf, "recent", "roots", &n, nullptr);
        for (gsize i = 0; arr && i < n; i++)
            if (arr[i] && arr[i][0] == '/') out.push_back(arr[i]);   // absolute only
        if (arr) g_strfreev(arr);
    }
    g_key_file_free(kf);
    return out;
}

std::vector<std::string> loadValidated() {
    std::vector<std::string> out;
    for (auto &r : load()) {
        std::string beads = r + "/.beads";
        if (g_file_test(beads.c_str(), G_FILE_TEST_IS_DIR)) out.push_back(r);
    }
    return out;
}

static void save(const std::vector<std::string> &roots) {
    GKeyFile *kf = g_key_file_new();
    // Load the existing file first — the ini also carries per-project
    // settings groups (events-seq etc.); writing a fresh keyfile here would
    // wipe them on every push().
    g_key_file_load_from_file(kf, iniPath().c_str(),
        (GKeyFileFlags)(G_KEY_FILE_KEEP_COMMENTS), nullptr);
    std::vector<const char *> ptrs;
    for (auto &r : roots) ptrs.push_back(r.c_str());
    g_key_file_set_string_list(kf, "recent", "roots", ptrs.data(), ptrs.size());
    gchar *dir = g_path_get_dirname(iniPath().c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    g_key_file_save_to_file(kf, iniPath().c_str(), nullptr);
    g_key_file_free(kf);
}

void push(const std::string &root) {
    if (root.empty()) return;
    auto roots = load();
    roots.erase(std::remove(roots.begin(), roots.end(), root), roots.end());
    roots.insert(roots.begin(), root);
    if (roots.size() > 10) roots.resize(10);
    save(roots);
}

void remove(const std::string &root) {
    auto roots = load();
    roots.erase(std::remove(roots.begin(), roots.end(), root), roots.end());
    save(roots);
}

void clear() { save({}); }

// ── per-project settings (one keyfile group per project root) ───────────────
static std::string projectGroup(const std::string &root) {
    return "project " + root;
}

static std::string getProjectKey(const std::string &root, const char *key) {
    std::string out;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, iniPath().c_str(), G_KEY_FILE_NONE, nullptr)) {
        gchar *v = g_key_file_get_string(kf, projectGroup(root).c_str(), key, nullptr);
        if (v) { out = v; g_free(v); }
    }
    g_key_file_free(kf);
    return out;
}

static void setProjectKey(const std::string &root, const char *key,
                          const std::string &value) {
    if (root.empty()) return;
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, iniPath().c_str(),
        (GKeyFileFlags)(G_KEY_FILE_KEEP_COMMENTS), nullptr);   // absent file is fine
    g_key_file_set_string(kf, projectGroup(root).c_str(), key, value.c_str());
    gchar *dir = g_path_get_dirname(iniPath().c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    g_key_file_save_to_file(kf, iniPath().c_str(), nullptr);
    g_key_file_free(kf);
}

std::string eventsAnswerFor(const std::string &root) {
    return getProjectKey(root, "events-answer");
}
void setEventsAnswer(const std::string &root, const std::string &answer) {
    setProjectKey(root, "events-answer", answer);
}

unsigned long long eventsSeqFor(const std::string &root) {
    std::string v = getProjectKey(root, "events-seq");
    return v.empty() ? 0ULL : g_ascii_strtoull(v.c_str(), nullptr, 10);
}
void setEventsSeq(const std::string &root, unsigned long long seq) {
    setProjectKey(root, "events-seq", std::to_string(seq));
}

bool jsonlPriorityFor(const std::string &root) {
    return getProjectKey(root, "jsonl-priority") == "true";
}
void setJsonlPriority(const std::string &root, bool enabled) {
    setProjectKey(root, "jsonl-priority", enabled ? "true" : "false");
}

bool autoPushFor(const std::string &root) {
    return getProjectKey(root, "auto-push") == "true";
}
void setAutoPush(const std::string &root, bool enabled) {
    setProjectKey(root, "auto-push", enabled ? "true" : "false");
}

int eventsPidFor(const std::string &root, int *recorderOut) {
    if (recorderOut) *recorderOut = 0;
    std::string v = getProjectKey(root, "events-pid");
    if (v.empty()) return 0;
    size_t colon = v.find(':');
    if (colon != std::string::npos && recorderOut)
        *recorderOut = atoi(v.c_str() + colon + 1);
    return atoi(v.c_str());
}
void setEventsPid(const std::string &root, int pid, int recorderPid) {
    setProjectKey(root, "events-pid",
                  std::to_string(pid) + ":" + std::to_string(recorderPid));
}

// ── Recent remote connections ("remotes" group; "spec\x1fdisplay" rows) ─────
std::vector<RemoteEntry> loadRemotes() {
    std::vector<RemoteEntry> out;
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, iniPath().c_str(), G_KEY_FILE_NONE, nullptr)) {
        gsize n = 0;
        gchar **arr = g_key_file_get_string_list(kf, "remotes", "entries", &n, nullptr);
        for (gsize i = 0; arr && i < n; i++) {
            const char *sep = arr[i] ? strchr(arr[i], '\x1f') : nullptr;
            if (!sep) continue;
            RemoteEntry e;
            e.spec.assign(arr[i], sep - arr[i]);
            e.display = sep + 1;
            if (!e.spec.empty()) out.push_back(std::move(e));
        }
        if (arr) g_strfreev(arr);
    }
    g_key_file_free(kf);
    return out;
}

void pushRemote(const std::string &spec, const std::string &display) {
    if (spec.empty()) return;
    auto list = loadRemotes();
    list.erase(std::remove_if(list.begin(), list.end(),
        [&](const RemoteEntry &e){ return e.spec == spec; }), list.end());
    list.insert(list.begin(), {spec, display.empty() ? spec : display});
    if (list.size() > 6) list.resize(6);

    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, iniPath().c_str(),
        (GKeyFileFlags)(G_KEY_FILE_KEEP_COMMENTS), nullptr);  // keep other groups
    std::vector<std::string> rows;
    std::vector<const char *> ptrs;
    for (auto &e : list) {
        rows.push_back(e.spec + '\x1f' + e.display);
        ptrs.push_back(rows.back().c_str());
    }
    g_key_file_set_string_list(kf, "remotes", "entries", ptrs.data(), ptrs.size());
    gchar *dir = g_path_get_dirname(iniPath().c_str());
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    g_key_file_save_to_file(kf, iniPath().c_str(), nullptr);
    g_key_file_free(kf);
}

}  // namespace BeadsRecent
