// beads_types.h — shared types for the NppBeads Linux port.
//
// The plugin speaks JSON throughout (issue objects come straight from
// `bd --json` / issues.jsonl and are handed to the JS bridge as-is), so we
// carry issue data as json-glib nodes rather than a native model. Errors use
// a typed struct that mirrors the macOS BeadsDataSourceErrorCode set.

#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>
#include <functional>
#include <string>
#include <vector>
#include <memory>

// Error classification, mirroring macOS BeadsDataSourceErrorCode / BdErrorKind.
enum BeadsErrorCode {
    BeadsErrOK           = 0,
    BeadsErrReadOnly     = 1,   // write attempted on the JSONL backend
    BeadsErrNotFound     = 2,
    BeadsErrBlockedByDeps= 3,   // close refused; `blockers` populated
    BeadsErrAlreadyClaimed = 4,
    BeadsErrCycle        = 5,
    BeadsErrLocked       = 6,
    BeadsErrBdMissing    = 7,
    BeadsErrNotInProject = 8,
    BeadsErrGeneric      = 99,
};

struct BeadsError {
    int code = BeadsErrOK;
    std::string message;
    std::vector<std::string> blockers;   // only for BeadsErrBlockedByDeps
    bool ok() const { return code == BeadsErrOK && message.empty(); }
};

// Completion shapes. `issue` is a borrowed JsonNode owned by the caller of the
// completion (freed after it returns) — copy it if you need to retain it.
using IssueCompletion    = std::function<void(JsonNode *issue, const BeadsError &err)>;
using IssueListCompletion= std::function<void(JsonArray *issues, const BeadsError &err)>;
using ErrorCompletion    = std::function<void(const BeadsError &err)>;

// ── The data-source interface (was the BeadsDataSource protocol) ────────────
class BeadsDataSource {
public:
    virtual ~BeadsDataSource() = default;

    virtual std::string backendLabel() const = 0;   // status-bar tag
    virtual bool writable() const = 0;

    // Reads.
    virtual void listAllIssues(IssueListCompletion done) = 0;
    virtual void showIssue(const std::string &id, IssueCompletion done) = 0;

    // Writes (JSONL backend answers all with BeadsErrReadOnly).
    virtual void createIssue(const std::string &title, const char *type,
                             const int *priority, const char *description,
                             const std::vector<std::string> &labels,
                             IssueCompletion done) = 0;
    virtual void updateIssue(const std::string &id, const char *title,
                             const char *description, const char *status,
                             const int *priority, const char *type,
                             const char *assignee,
                             const std::vector<std::string> &addLabels,
                             const std::vector<std::string> &removeLabels,
                             IssueCompletion done) = 0;
    virtual void claimIssue(const std::string &id, IssueCompletion done) = 0;
    virtual void closeIssue(const std::string &id, const char *reason,
                            bool force, IssueCompletion done) = 0;
    virtual void reopenIssue(const std::string &id, const char *reason,
                             IssueCompletion done) = 0;
    virtual void addDependency(const std::string &dependent,
                               const std::string &dependency,
                               const std::string &depType, ErrorCompletion done) = 0;
    virtual void removeDependency(const std::string &dependent,
                                  const std::string &dependency, ErrorCompletion done) = 0;
    virtual void deleteIssue(const std::string &id, ErrorCompletion done) = 0;
    virtual void unassignIssue(const std::string &id, IssueCompletion done) = 0;
    virtual void addComment(const std::string &id, const std::string &body,
                            ErrorCompletion done) = 0;

    virtual void invalidateCache() = 0;
};

// ── small json-glib helpers (shared across modules) ─────────────────────────
namespace beads {

// Parse a JSON document; returns an owned root JsonNode* or nullptr.
inline JsonNode *parseJson(const char *data, gssize len) {
    if (!data) return nullptr;
    JsonParser *p = json_parser_new();
    JsonNode *root = nullptr;
    if (json_parser_load_from_data(p, data, len, nullptr)) {
        JsonNode *r = json_parser_get_root(p);
        if (r) root = json_node_copy(r);
    }
    g_object_unref(p);
    return root;
}

// Serialize a node to a std::string.
inline std::string jsonToString(JsonNode *node) {
    if (!node) return "";
    JsonGenerator *g = json_generator_new();
    json_generator_set_root(g, node);
    gsize len = 0;
    gchar *s = json_generator_to_data(g, &len);
    std::string out(s ? s : "", s ? len : 0);
    g_free(s);
    g_object_unref(g);
    return out;
}

// Trim ASCII whitespace.
inline std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

}  // namespace beads
