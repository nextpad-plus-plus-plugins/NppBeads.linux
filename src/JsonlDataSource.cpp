// JsonlDataSource.cpp — see header. Ported from JsonlDataSource.mm.

#include "JsonlDataSource.h"
#include <glib/gstdio.h>

static BeadsError readOnlyError() {
    return { BeadsErrReadOnly, "Read-only JSONL backend. Install `bd` to enable editing.", {} };
}
// Completions must fire on the main loop (protocol contract) — defer via idle.
template <typename Fn>
static void onMain(Fn fn) {
    auto *heap = new Fn(std::move(fn));
    g_idle_add([](gpointer p) -> gboolean {
        auto *f = static_cast<Fn *>(p);
        (*f)(); delete f; return G_SOURCE_REMOVE;
    }, heap);
}

JsonlDataSource::JsonlDataSource() { cachedIssues_ = json_array_new(); }
JsonlDataSource::~JsonlDataSource() { if (cachedIssues_) json_array_unref(cachedIssues_); }

void JsonlDataSource::bindToPath(const std::string &path) {
    if (path == jsonlPath_) return;
    jsonlPath_ = path;
    reload();
}

void JsonlDataSource::reload() {
    loaded_ = false;
    cachedText_.clear();
    if (cachedIssues_) json_array_unref(cachedIssues_);
    cachedIssues_ = json_array_new();
    cntOpen_ = cntBlocked_ = cntClosed_ = 0;
}

void JsonlDataSource::loadIfNeeded() {
    if (loaded_) return;
    loaded_ = true;   // set first so a parse failure doesn't re-loop
    if (jsonlPath_.empty()) return;

    gchar *data = nullptr; gsize len = 0;
    GError *err = nullptr;
    if (!g_file_get_contents(jsonlPath_.c_str(), &data, &len, &err)) {
        g_message("[NppBeads] JSONL read failed: %s -> %s", jsonlPath_.c_str(),
                  err ? err->message : "?");
        if (err) g_error_free(err);
        return;
    }
    // Store raw text; if not valid UTF-8, keep bytes as-is (viewer tolerates).
    cachedText_.assign(data, len);

    gchar **lines = g_strsplit(cachedText_.c_str(), "\n", -1);
    for (int i = 0; lines[i]; i++) {
        std::string t = beads::trim(lines[i]);
        if (t.empty()) continue;
        JsonNode *node = beads::parseJson(t.c_str(), t.size());
        if (!node) continue;
        if (!JSON_NODE_HOLDS_OBJECT(node)) { json_node_free(node); continue; }
        json_array_add_element(cachedIssues_, node);   // takes ownership
    }
    g_strfreev(lines);
    g_free(data);

    guint n = json_array_get_length(cachedIssues_);
    for (guint i = 0; i < n; i++) {
        JsonObject *o = json_array_get_object_element(cachedIssues_, i);
        if (!o || !json_object_has_member(o, "status")) { cntOpen_++; continue; }
        const char *s = json_object_get_string_member(o, "status");
        if (!s) { cntOpen_++; continue; }
        if (!strcmp(s, "closed")) cntClosed_++;
        else if (!strcmp(s, "blocked")) cntBlocked_++;
        else cntOpen_++;
    }
}

std::string JsonlDataSource::rawText() { loadIfNeeded(); return cachedText_; }
JsonArray *JsonlDataSource::issues() { loadIfNeeded(); return cachedIssues_; }
size_t JsonlDataSource::issueCount() { loadIfNeeded(); return json_array_get_length(cachedIssues_); }
size_t JsonlDataSource::openIssueCount() { loadIfNeeded(); return cntOpen_; }
size_t JsonlDataSource::blockedIssueCount() { loadIfNeeded(); return cntBlocked_; }
size_t JsonlDataSource::closedIssueCount() { loadIfNeeded(); return cntClosed_; }

void JsonlDataSource::listAllIssues(IssueListCompletion done) {
    JsonArray *arr = issues();
    JsonArray *copy = json_array_ref(arr);
    onMain([done, copy] {
        if (done) done(copy, {});
        json_array_unref(copy);
    });
}

void JsonlDataSource::showIssue(const std::string &id, IssueCompletion done) {
    JsonArray *arr = issues();
    JsonNode *hit = nullptr;
    guint n = json_array_get_length(arr);
    for (guint i = 0; i < n; i++) {
        JsonObject *o = json_array_get_object_element(arr, i);
        if (o && json_object_has_member(o, "id")) {
            const char *v = json_object_get_string_member(o, "id");
            if (v && id == v) { hit = json_node_copy(json_array_get_element(arr, i)); break; }
        }
    }
    if (!hit) {
        BeadsError e { BeadsErrNotFound, "Issue " + id + " not in JSONL", {} };
        onMain([done, e] { if (done) done(nullptr, e); });
        return;
    }
    onMain([done, hit] { if (done) done(hit, {}); json_node_free(hit); });
}

// Every write → ReadOnly.
#define RO_ISSUE(name, sig) void JsonlDataSource::name sig { BeadsError e = readOnlyError(); onMain([done, e]{ if (done) done(nullptr, e); }); }
#define RO_ERR(name, sig)   void JsonlDataSource::name sig { BeadsError e = readOnlyError(); onMain([done, e]{ if (done) done(e); }); }

void JsonlDataSource::createIssue(const std::string &, const char *, const int *, const char *,
                                  const std::vector<std::string> &, IssueCompletion done) {
    BeadsError e = readOnlyError(); onMain([done, e]{ if (done) done(nullptr, e); });
}
void JsonlDataSource::updateIssue(const std::string &, const char *, const char *, const char *,
                                  const int *, const char *, const char *, const std::vector<std::string> &,
                                  const std::vector<std::string> &, IssueCompletion done) {
    BeadsError e = readOnlyError(); onMain([done, e]{ if (done) done(nullptr, e); });
}
void JsonlDataSource::claimIssue(const std::string &, IssueCompletion done) {
    BeadsError e = readOnlyError(); onMain([done, e]{ if (done) done(nullptr, e); });
}
void JsonlDataSource::closeIssue(const std::string &, const char *, bool, IssueCompletion done) {
    BeadsError e = readOnlyError(); onMain([done, e]{ if (done) done(nullptr, e); });
}
void JsonlDataSource::reopenIssue(const std::string &, const char *, IssueCompletion done) {
    BeadsError e = readOnlyError(); onMain([done, e]{ if (done) done(nullptr, e); });
}
void JsonlDataSource::addDependency(const std::string &, const std::string &, const std::string &, ErrorCompletion done) {
    BeadsError e = readOnlyError(); onMain([done, e]{ if (done) done(e); });
}
void JsonlDataSource::removeDependency(const std::string &, const std::string &, ErrorCompletion done) {
    BeadsError e = readOnlyError(); onMain([done, e]{ if (done) done(e); });
}
void JsonlDataSource::deleteIssue(const std::string &, ErrorCompletion done) {
    BeadsError e = readOnlyError(); onMain([done, e]{ if (done) done(e); });
}
void JsonlDataSource::unassignIssue(const std::string &, IssueCompletion done) {
    BeadsError e = readOnlyError(); onMain([done, e]{ if (done) done(nullptr, e); });
}
void JsonlDataSource::addComment(const std::string &, const std::string &, ErrorCompletion done) {
    BeadsError e = readOnlyError(); onMain([done, e]{ if (done) done(e); });
}
