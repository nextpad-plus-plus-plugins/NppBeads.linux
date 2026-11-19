// BeadsProjectScanner — locate `.beads/` by walking up from a file/dir.
// Ported from BeadsProjectScanner.mm. Pure path logic.

#pragma once

#include <string>
#include <vector>
#include <memory>

struct BeadsProject {
    std::string beadsDir;      // absolute path to .beads/
    std::string jsonlPath;     // .beads/issues.jsonl or "" if absent
    std::string dbPath;        // .beads/beads.db or "" if absent
    std::string projectRoot;   // parent of .beads/
    // Remote read-only mode (macOS nppmac-0th): the connection spec doubles
    // as beadsDir/projectRoot so every string-identity code path (dedupe,
    // per-project keys, chip tooltip) works unchanged; jsonlPath stays ""
    // — the RemoteJsonlFetcher feeds JsonlDataSource via adoptRawText.
    std::string remoteSpec;
    std::string remoteDisplay;
    bool isRemote() const { return !remoteSpec.empty(); }
};

namespace BeadsProjectScanner {

bool isUsableBeadsDir(const std::string &beadsDir);
std::shared_ptr<BeadsProject> findProjectFromPath(const std::string &filePath);
std::shared_ptr<BeadsProject> projectFromBeadsDir(const std::string &beadsDir);
std::shared_ptr<BeadsProject> projectFromRoot(const std::string &projectRoot);
std::shared_ptr<BeadsProject> remoteProject(const std::string &spec,
                                            const std::string &display);
std::vector<std::shared_ptr<BeadsProject>>
    discoverUniqueProjectsFromPaths(const std::vector<std::string> &paths, size_t max);

}  // namespace BeadsProjectScanner
