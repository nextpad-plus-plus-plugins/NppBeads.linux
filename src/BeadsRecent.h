// BeadsRecent — persistent MRU of project roots, shared by the NppBeads
// plugin panel and the standalone BeadsViewer app (Linux equivalent of the
// macOS NppBeadsRecentProjectRoots defaults key, which both binaries share).
// Stored as a GKeyFile at $XDG_CONFIG_HOME/beadsviewer/recent.ini.

#pragma once

#include <string>
#include <vector>

namespace BeadsRecent {

// All stored roots, MRU-first, unvalidated.
std::vector<std::string> load();

// Stored roots whose <root>/.beads still exists (macOS
// _persistedRecentProjectRoots semantics — filters at read, doesn't rewrite).
std::vector<std::string> loadValidated();

void push(const std::string &root);     // dedupe, insert front, cap 10
void remove(const std::string &root);   // prune a stale entry
void clear();

}  // namespace BeadsRecent
