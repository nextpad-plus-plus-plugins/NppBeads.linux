// BeadsProjectScanner.cpp — see header. Ported from BeadsProjectScanner.mm.

#include "BeadsProjectScanner.h"
#include <glib.h>
#include <set>

namespace BeadsProjectScanner {

bool isUsableBeadsDir(const std::string &beadsDir) {
    if (beadsDir.empty()) return false;
    if (!g_file_test(beadsDir.c_str(), G_FILE_TEST_IS_DIR)) return false;
    std::string jsonl = beadsDir + "/issues.jsonl";
    std::string db    = beadsDir + "/beads.db";
    return g_file_test(jsonl.c_str(), G_FILE_TEST_EXISTS) ||
           g_file_test(db.c_str(), G_FILE_TEST_EXISTS);
}

static std::shared_ptr<BeadsProject> makeProject(const std::string &beadsDir,
                                                 const std::string &root) {
    auto p = std::make_shared<BeadsProject>();
    p->beadsDir = beadsDir;
    p->projectRoot = root;
    std::string jsonl = beadsDir + "/issues.jsonl";
    std::string db    = beadsDir + "/beads.db";
    if (g_file_test(jsonl.c_str(), G_FILE_TEST_EXISTS)) p->jsonlPath = jsonl;
    if (g_file_test(db.c_str(), G_FILE_TEST_EXISTS))    p->dbPath    = db;
    return p;
}

std::shared_ptr<BeadsProject> findProjectFromPath(const std::string &filePath) {
    if (filePath.empty()) return nullptr;
    std::string dir = filePath;
    if (!g_file_test(dir.c_str(), G_FILE_TEST_EXISTS)) return nullptr;
    if (!g_file_test(dir.c_str(), G_FILE_TEST_IS_DIR)) {
        gchar *d = g_path_get_dirname(dir.c_str());
        dir = d; g_free(d);
    }
    const char *home = g_get_home_dir();
    std::string homeStr = home ? home : "";
    const size_t kMaxDepth = 40;
    size_t depth = 0;
    while (dir.length() > 1 && depth++ < kMaxDepth) {
        std::string candidate = dir + "/.beads";
        if (isUsableBeadsDir(candidate)) return makeProject(candidate, dir);
        if (dir == homeStr) return nullptr;       // stop at $HOME
        gchar *parent = g_path_get_dirname(dir.c_str());
        std::string p = parent; g_free(parent);
        if (p == dir) return nullptr;              // hit root
        dir = p;
    }
    return nullptr;
}

std::shared_ptr<BeadsProject> projectFromBeadsDir(const std::string &beadsDir) {
    if (beadsDir.empty() || !isUsableBeadsDir(beadsDir)) return nullptr;
    gchar *root = g_path_get_dirname(beadsDir.c_str());
    auto p = makeProject(beadsDir, root);
    g_free(root);
    return p;
}

std::shared_ptr<BeadsProject> projectFromRoot(const std::string &projectRoot) {
    if (projectRoot.empty()) return nullptr;
    return projectFromBeadsDir(projectRoot + "/.beads");
}

std::vector<std::shared_ptr<BeadsProject>>
discoverUniqueProjectsFromPaths(const std::vector<std::string> &paths, size_t max) {
    std::vector<std::shared_ptr<BeadsProject>> out;
    if (paths.empty() || max == 0) return out;
    std::set<std::string> seen;
    for (const auto &p : paths) {
        if (out.size() >= max) break;
        auto proj = findProjectFromPath(p);
        if (!proj || proj->beadsDir.empty()) continue;
        if (seen.count(proj->beadsDir)) continue;
        seen.insert(proj->beadsDir);
        out.push_back(proj);
    }
    return out;
}

}  // namespace BeadsProjectScanner
