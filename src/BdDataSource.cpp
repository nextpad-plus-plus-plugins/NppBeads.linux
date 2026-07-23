// BdDataSource.cpp — see header. Ported from BdDataSource.mm.

#include "BdDataSource.h"

static BeadsError errorFromResult(const BdResultPtr &res) {
    if (!res || res->ok) return {};
    BeadsError e;
    switch (res->errorKind()) {
        case BdErrNotFound:       e.code = BeadsErrNotFound; break;
        case BdErrBlockedByDeps:  e.code = BeadsErrBlockedByDeps; e.blockers = res->blockerIds(); break;
        case BdErrAlreadyClaimed: e.code = BeadsErrAlreadyClaimed; break;
        case BdErrCycle:          e.code = BeadsErrCycle; break;
        case BdErrLocked:         e.code = BeadsErrLocked; break;
        case BdErrBdMissing:      e.code = BeadsErrBdMissing; break;
        case BdErrNotInProject:   e.code = BeadsErrNotInProject; break;
        default:                  e.code = BeadsErrGeneric; break;
    }
    e.message = res->errorMessage.empty() ? "bd command failed" : res->errorMessage;
    return e;
}

// bd's `json` is owned by the BdResult; hand the completion a copy it can keep
// during the call (it's freed after return per the IssueCompletion contract).
static JsonNode *objOrNull(const BdResultPtr &res) {
    return (res->json && JSON_NODE_HOLDS_OBJECT(res->json)) ? res->json : nullptr;
}

void BdDataSource::listAllIssues(IssueListCompletion done) {
    runner_->listAllIssues([done](const BdResultPtr &res) {
        if (!res->ok) { if (done) done(nullptr, errorFromResult(res)); return; }
        JsonArray *arr = (res->json && JSON_NODE_HOLDS_ARRAY(res->json))
            ? json_node_get_array(res->json) : nullptr;
        if (done) done(arr, {});
    });
}
void BdDataSource::showIssue(const std::string &id, IssueCompletion done) {
    runner_->showIssue(id, [done](const BdResultPtr &res) {
        if (!res->ok) { if (done) done(nullptr, errorFromResult(res)); return; }
        if (done) done(objOrNull(res), {});
    });
}
void BdDataSource::createIssue(const std::string &title, const char *type, const int *priority,
                               const char *description, const std::vector<std::string> &labels,
                               IssueCompletion done) {
    runner_->createIssue(title, type, priority, description, labels, [done](const BdResultPtr &res) {
        if (!res->ok) { if (done) done(nullptr, errorFromResult(res)); return; }
        if (done) done(objOrNull(res), {});
    });
}
void BdDataSource::updateIssue(const std::string &id, const char *title, const char *description,
                               const char *status, const int *priority, const char *type,
                               const char *assignee, const std::vector<std::string> &addLabels,
                               const std::vector<std::string> &removeLabels, IssueCompletion done) {
    runner_->updateIssue(id, title, description, status, priority, type, assignee, addLabels,
                         removeLabels, [done](const BdResultPtr &res) {
        if (!res->ok) { if (done) done(nullptr, errorFromResult(res)); return; }
        if (done) done(objOrNull(res), {});
    });
}
void BdDataSource::claimIssue(const std::string &id, IssueCompletion done) {
    runner_->claimIssue(id, [done](const BdResultPtr &res) {
        if (!res->ok) { if (done) done(nullptr, errorFromResult(res)); return; }
        if (done) done(objOrNull(res), {});
    });
}
void BdDataSource::closeIssue(const std::string &id, const char *reason, bool force, IssueCompletion done) {
    runner_->closeIssue(id, reason, force, [done](const BdResultPtr &res) {
        if (!res->ok) { if (done) done(nullptr, errorFromResult(res)); return; }
        if (done) done(objOrNull(res), {});
    });
}
void BdDataSource::reopenIssue(const std::string &id, const char *reason, IssueCompletion done) {
    runner_->reopenIssue(id, reason, [done](const BdResultPtr &res) {
        if (!res->ok) { if (done) done(nullptr, errorFromResult(res)); return; }
        if (done) done(objOrNull(res), {});
    });
}
void BdDataSource::addDependency(const std::string &dependent, const std::string &dependency,
                                 const std::string &depType, ErrorCompletion done) {
    runner_->addDependency(dependent, dependency, depType, [done](const BdResultPtr &res) {
        if (done) done(res->ok ? BeadsError{} : errorFromResult(res));
    });
}
void BdDataSource::removeDependency(const std::string &dependent, const std::string &dependency, ErrorCompletion done) {
    runner_->removeDependency(dependent, dependency, [done](const BdResultPtr &res) {
        if (done) done(res->ok ? BeadsError{} : errorFromResult(res));
    });
}
void BdDataSource::deleteIssue(const std::string &id, ErrorCompletion done) {
    runner_->deleteIssue(id, [done](const BdResultPtr &res) {
        if (done) done(res->ok ? BeadsError{} : errorFromResult(res));
    });
}
void BdDataSource::unassignIssue(const std::string &id, IssueCompletion done) {
    runner_->unassignIssue(id, [done](const BdResultPtr &res) {
        if (!res->ok) { if (done) done(nullptr, errorFromResult(res)); return; }
        if (done) done(objOrNull(res), {});
    });
}
void BdDataSource::addComment(const std::string &id, const std::string &body, ErrorCompletion done) {
    runner_->addComment(id, body, [done](const BdResultPtr &res) {
        if (done) done(res->ok ? BeadsError{} : errorFromResult(res));
    });
}
