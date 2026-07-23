// BdDataSource — BeadsDataSource over BdCommandRunner. Pass-through that maps
// BdErrorKind → BeadsErrorCode. Ported from BdDataSource.mm.

#pragma once

#include "beads_types.h"
#include "BdCommandRunner.h"

class BdDataSource : public BeadsDataSource {
public:
    explicit BdDataSource(std::shared_ptr<BdCommandRunner> runner) : runner_(std::move(runner)) {}
    BdCommandRunner *runner() const { return runner_.get(); }

    std::string backendLabel() const override {
        std::string v = runner_->bdVersion().empty() ? "?" : runner_->bdVersion();
        return "bd v" + v;
    }
    bool writable() const override { return !runner_->bdPath().empty(); }
    void invalidateCache() override { runner_->invalidateCache(); }

    void listAllIssues(IssueListCompletion done) override;
    void showIssue(const std::string &id, IssueCompletion done) override;
    void createIssue(const std::string &title, const char *type, const int *priority,
                     const char *description, const std::vector<std::string> &labels,
                     IssueCompletion done) override;
    void updateIssue(const std::string &id, const char *title, const char *description,
                     const char *status, const int *priority, const char *type,
                     const char *assignee, const std::vector<std::string> &addLabels,
                     const std::vector<std::string> &removeLabels, IssueCompletion done) override;
    void claimIssue(const std::string &id, IssueCompletion done) override;
    void closeIssue(const std::string &id, const char *reason, bool force, IssueCompletion done) override;
    void reopenIssue(const std::string &id, const char *reason, IssueCompletion done) override;
    void addDependency(const std::string &dependent, const std::string &dependency,
                       const std::string &depType, ErrorCompletion done) override;
    void removeDependency(const std::string &dependent, const std::string &dependency, ErrorCompletion done) override;
    void deleteIssue(const std::string &id, ErrorCompletion done) override;
    void unassignIssue(const std::string &id, IssueCompletion done) override;
    void addComment(const std::string &id, const std::string &body, ErrorCompletion done) override;

private:
    std::shared_ptr<BdCommandRunner> runner_;
};
