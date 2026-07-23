// BeadsPoll — timer-based `bd list` poll with hash-compare change detection.
// Companion to BeadsWatcher for change flows the file watch misses. Ported
// from BeadsPoll.mm. GLib g_timeout replaces the dispatch timer source.

#pragma once

#include "BdCommandRunner.h"
#include <functional>

class BeadsPoll {
public:
    BeadsPoll(std::shared_ptr<BdCommandRunner> runner, unsigned intervalMs);
    ~BeadsPoll();

    std::function<void(const std::string &newListJsonText)> onChange;
    bool isPaused() const { return paused_; }

    void start();
    void stop();
    void pause() { paused_ = true; }
    void resume() { paused_ = false; }
    void kick();

private:
    void tick();
    static gboolean onTimer(gpointer self);

    std::shared_ptr<BdCommandRunner> runner_;
    unsigned intervalMs_;
    guint timer_ = 0;
    bool started_ = false, paused_ = false, inFlight_ = false;
    unsigned long generation_ = 0;
    std::string lastPayload_;
    bool havePayload_ = false;
};
