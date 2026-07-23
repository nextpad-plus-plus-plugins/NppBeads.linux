// JsonlDataSource — reads `.beads/issues.jsonl` from disk. Always-available
// read-only backend (writes return BeadsErrReadOnly). Robust against missing/
// unreadable/non-UTF-8/malformed lines. Ported from JsonlDataSource.mm.

#pragma once

#include "beads_types.h"

class JsonlDataSource : public BeadsDataSource {
public:
    JsonlDataSource();
    ~JsonlDataSource() override;

    void bindToPath(const std::string &path);   // "" to unbind
    const std::string &jsonlPath() const { return jsonlPath_; }

    std::string rawText();                        // raw JSONL (cached)
    JsonArray *issues();                          // parsed rows (borrowed; owned by this)
    void reload();                                // force reread next accessor

    size_t issueCount();
    size_t openIssueCount();
    size_t blockedIssueCount();
    size_t closedIssueCount();

    // BeadsDataSource
    std::string backendLabel() const override { return "read-only (JSONL)"; }
    bool writable() const override { return false; }
    void listAllIssues(IssueListCompletion done) override;
    void showIssue(const std::string &id, IssueCompletion done) override;
    void createIssue(const std::string &, const char *, const int *, const char *,
                     const std::vector<std::string> &, IssueCompletion done) override;
    void updateIssue(const std::string &, const char *, const char *, const char *,
                     const int *, const char *, const char *, const std::vector<std::string> &,
                     const std::vector<std::string> &, IssueCompletion done) override;
    void claimIssue(const std::string &, IssueCompletion done) override;
    void closeIssue(const std::string &, const char *, bool, IssueCompletion done) override;
    void reopenIssue(const std::string &, const char *, IssueCompletion done) override;
    void addDependency(const std::string &, const std::string &, const std::string &, ErrorCompletion done) override;
    void removeDependency(const std::string &, const std::string &, ErrorCompletion done) override;
    void deleteIssue(const std::string &, ErrorCompletion done) override;
    void unassignIssue(const std::string &, IssueCompletion done) override;
    void addComment(const std::string &, const std::string &, ErrorCompletion done) override;
    void invalidateCache() override { reload(); }

private:
    void loadIfNeeded();

    std::string jsonlPath_;
    std::string cachedText_;
    JsonArray  *cachedIssues_ = nullptr;   // owned
    bool loaded_ = false;
    size_t cntOpen_ = 0, cntBlocked_ = 0, cntClosed_ = 0;
};
