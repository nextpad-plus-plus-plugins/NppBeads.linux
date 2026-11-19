// BdEventsTail — live-sync via `bd events tail --since N --follow`
// (bd >= 1.3 with the per-workspace events journal enabled). Linux port of
// BdEventsTail.mm (NSTask → GSubprocess, dispatch → GLib main loop).
//
// Replaces BeadsPoll's 2-second `bd list` fork-and-diff with ONE long-lived
// bd process that emits a JSON Lines record for every committed mutation. We
// use the records as change TRIGGERS (the panel re-broadcasts through its
// existing reload path). Delivery is ~1s after a write (bd's --follow polls
// its journal internally) — same ballpark as the old poll's worst case, at a
// tiny fraction of the cost.
//
// Lifecycle mirrors BeadsPoll (start/stop/pause/resume) so BeadsPanel can
// treat either as its live-sync source:
//   - pause() doesn't kill the process; records keep updating lastSeq and set
//     a pending flag, flushed as one onChange on resume().
//   - stop() terminates the child and silences all callbacks.
//   - If the child dies on its own it is restarted (backoff, 2 tries,
//     resuming from lastSeq). After that — or when bd reports the journal
//     disabled — onBroken fires once so the owner can fall back to polling.
//
// Checkpointing is the OWNER's job: set sinceSeq before start() (0 = full
// replay, bounded by bd's retention) and persist lastSeq wherever you like.
// A sinceSeq below the retention floor makes bd exit non-zero ("truncated");
// the restart path retries from the highest seq seen, and onBroken is the
// final fallback.

#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <functional>
#include <memory>
#include <string>
#include <cstdint>

class BdEventsTail {
public:
    BdEventsTail(const std::string &bdPath, const std::string &projectDir);
    ~BdEventsTail();

    // Fired on the main loop, coalesced (~250 ms) over bursts of records.
    std::function<void()> onChange;
    // Fired on the main loop, at most once per start() cycle, when the feed
    // cannot continue (process kept dying, or bd says the journal is
    // disabled). The owner should fall back to BeadsPoll.
    std::function<void(const std::string &reason)> onBroken;

    // Checkpoint to resume from; set BEFORE start(). 0 replays the whole
    // retained journal.
    void setSinceSeq(uint64_t s) { sinceSeq_ = s; }
    uint64_t sinceSeq() const { return sinceSeq_; }
    // Highest seq seen so far (persist this as the next sinceSeq).
    uint64_t lastSeq() const { return lastSeq_; }

    // Diagnostics. Main loop.
    unsigned recordCount() const { return recordCount_; }
    bool isPaused() const { return paused_; }
    bool isRunning() const { return proc_ != nullptr; }

    void start();
    void stop();
    void pause() { paused_ = true; }
    void resume();

private:
    void spawn();
    void readNextLine(GDataInputStream *stream, unsigned gen);
    bool consumeRecordLine(const char *line);
    void scheduleChange();
    void childExited(int status);
    void breakWithReason(const std::string &reason);

    std::string bdPath_, projectDir_;
    GSubprocess *proc_ = nullptr;          // current child
    GDataInputStream *outStream_ = nullptr;
    GDataInputStream *errStream_ = nullptr;
    unsigned spawnGen_ = 0;                // identity-gates stale async reads
    std::string stderrTail_;               // last stderr text for diagnostics
    bool stopping_ = false;
    bool paused_ = false;
    bool pendingChange_ = false;
    bool changeScheduled_ = false;
    bool brokenFired_ = false;
    unsigned restartCount_ = 0;
    uint64_t sinceSeq_ = 0;
    uint64_t lastSeq_ = 0;
    unsigned recordCount_ = 0;
    // Liveness token for timeouts/async callbacks that outlive the object.
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};
