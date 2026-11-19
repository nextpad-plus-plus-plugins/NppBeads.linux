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

// ── Per-project panel settings, same ini (shared with the standalone app;
// Linux equivalent of the macOS NSUserDefaults keys). Each project root gets
// its own keyfile group ("project <root>").

// Events-feed prompt answer: "" (never asked), "enabled" or "declined".
std::string eventsAnswerFor(const std::string &root);
void setEventsAnswer(const std::string &root, const std::string &answer);

// Events-journal checkpoint (highest seq seen; next tail resumes from it).
unsigned long long eventsSeqFor(const std::string &root);
void setEventsSeq(const std::string &root, unsigned long long seq);

// Data-source priority escape hatch: render from issues.jsonl instead of the
// Dolt DB. Absent/false = Dolt priority (default).
bool jsonlPriorityFor(const std::string &root);
void setJsonlPriority(const std::string &root, bool enabled);

// Per-project bd auto-push opt-in (macOS NppBeadsAutoPushProjects). Absent /
// false = sandbox ON (bd gets --sandbox, Dolt auto-push disabled — default).
bool autoPushFor(const std::string &root);
void setAutoPush(const std::string &root, bool enabled);

// Last spawned `bd events tail` child pid for this project + the pid of the
// host process that recorded it (macOS nppmac-8n1 orphan sweep, Linux
// shape). Deliberately NOT cleared on clean stop — the next start() reaps
// the child only when it is alive, its RECORDER is dead (orphans re-parent
// to an arbitrary subreaper on Linux, so ppid tells nothing) and its
// command line still is a bd events-tail invocation; stale values are
// harmless.
int eventsPidFor(const std::string &root, int *recorderOut);
void setEventsPid(const std::string &root, int pid, int recorderPid);

// ── Recent REMOTE connections (macOS NppBeadsRecentRemotes): {spec, display}
// pairs, MRU-first, deduped by spec, capped. Specs only — tokens NEVER land
// here (they live in the Secret Service keyring).
struct RemoteEntry { std::string spec; std::string display; };
std::vector<RemoteEntry> loadRemotes();
void pushRemote(const std::string &spec, const std::string &display);

}  // namespace BeadsRecent
