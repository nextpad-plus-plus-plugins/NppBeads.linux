// BdCommandRunner — g_subprocess wrapper around the `bd` CLI (Linux port of
// the macOS NSTask-based runner). Every CRUD surface routes through here.
//
// Async model: g_subprocess_communicate_async runs the child on the GLib main
// loop and calls back on the main thread when done — no manual threads, and
// completions are already on the main thread (matching the macOS contract).
//
// Faithful to the macOS design: always prepend --sandbox (disables bd's dolt
// auto-push → ~100× faster writes on repos without non-interactive git auth),
// force BEADS_ACTOR + LANG env, drain both pipes, classify errors via regex,
// short read-cache (750 ms list / 250 ms show), unwrap bd's single-object
// arrays. Linux path resolution walks ~/.local/bin, ~/go/bin, /usr/local/bin,
// /usr/bin, then PATH (no Homebrew paths).

#pragma once

#include "beads_types.h"
#include <map>

// bd error classification (mirrors macOS BdErrorKind).
enum BdErrorKind {
    BdErrNone = 0, BdErrBlockedByDeps = 1, BdErrNotFound = 2,
    BdErrAlreadyClaimed = 3, BdErrCycle = 4, BdErrLocked = 5,
    BdErrBdMissing = 6, BdErrNotInProject = 7, BdErrGeneric = 99,
};

struct BdResult {
    bool ok = false;
    int  exitCode = 0;
    JsonNode *json = nullptr;        // owned; freed in ~BdResult
    std::string errorMessage;
    std::vector<std::string> warnings;
    std::string rawStdout, rawStderr;
    double elapsed = 0;
    std::vector<std::string> argv;

    BdResult() = default;
    ~BdResult();
    BdResult(const BdResult &) = delete;
    BdResult &operator=(const BdResult &) = delete;

    BdErrorKind errorKind() const;
    std::vector<std::string> blockerIds() const;
};
using BdResultPtr = std::shared_ptr<BdResult>;
using BdCompletion = std::function<void(const BdResultPtr &res)>;

class BdCommandRunner {
public:
    explicit BdCommandRunner(const std::string &projectDir);
    ~BdCommandRunner();

    const std::string &projectDir() const { return projectDir_; }
    const std::string &bdPath() const { return bdPath_; }
    const std::string &bdVersion() const { return bdVersion_; }
    void setActor(const std::string &a) { actor_ = a; }
    void setUseSandbox(bool v) { useSandbox_ = v; }
    bool useSandbox() const { return useSandbox_; }

    // is bd installed + does this dir have a usable project? Cached; completion
    // on the main thread. (bdPresent, projectReady)
    void probe(std::function<void(bool bdPresent, bool projectReady)> done,
               bool forceRefresh = false);

    // Reads (cached).
    void listAllIssues(BdCompletion done);
    void showIssue(const std::string &id, BdCompletion done);
    void ready(BdCompletion done);

    // Writes (never cached; invalidate the read cache on success).
    void createIssue(const std::string &title, const char *type,
                     const int *priority, const char *description,
                     const std::vector<std::string> &labels, BdCompletion done);
    void updateIssue(const std::string &id, const char *title,
                     const char *description, const char *status,
                     const int *priority, const char *type, const char *assignee,
                     const std::vector<std::string> &addLabels,
                     const std::vector<std::string> &removeLabels, BdCompletion done);
    void claimIssue(const std::string &id, BdCompletion done);
    void closeIssue(const std::string &id, const char *reason, bool force, BdCompletion done);
    void reopenIssue(const std::string &id, const char *reason, BdCompletion done);
    void addDependency(const std::string &dependent, const std::string &dependency,
                       const std::string &depType, BdCompletion done);
    void removeDependency(const std::string &dependent, const std::string &dependency,
                          BdCompletion done);
    void deleteIssue(const std::string &id, BdCompletion done);
    void unassignIssue(const std::string &id, BdCompletion done);
    void addComment(const std::string &id, const std::string &body, BdCompletion done);

    void invalidateCache();

private:
    struct CacheEntry { BdResultPtr result; gint64 expiresAt; };

    // Run bd async; `expectsJson` gates JSON parsing (off for version/info).
    void execute(std::vector<std::string> args, const std::string *stdinText,
                 bool expectsJson, BdCompletion done);
    BdResultPtr cachedFor(const std::string &key, double ttlSec);
    void cacheStore(const BdResultPtr &res, const std::string &key, double ttlSec);

    std::string projectDir_;
    std::string bdPath_;
    std::string bdVersion_;
    std::string actor_;
    bool useSandbox_ = true;
    bool probed_ = false;
    bool projectReady_ = false;
    std::map<std::string, CacheEntry> cache_;
};
