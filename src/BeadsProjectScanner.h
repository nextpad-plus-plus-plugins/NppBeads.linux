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
};

namespace BeadsProjectScanner {

bool isUsableBeadsDir(const std::string &beadsDir);
std::shared_ptr<BeadsProject> findProjectFromPath(const std::string &filePath);
std::shared_ptr<BeadsProject> projectFromBeadsDir(const std::string &beadsDir);
std::shared_ptr<BeadsProject> projectFromRoot(const std::string &projectRoot);
std::vector<std::shared_ptr<BeadsProject>>
    discoverUniqueProjectsFromPaths(const std::vector<std::string> &paths, size_t max);

}  // namespace BeadsProjectScanner
