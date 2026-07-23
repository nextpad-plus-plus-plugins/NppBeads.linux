// BeadsPoll.cpp — see header. Ported from BeadsPoll.mm.

#include "BeadsPoll.h"

BeadsPoll::BeadsPoll(std::shared_ptr<BdCommandRunner> runner, unsigned intervalMs)
    : runner_(std::move(runner)) {
    if (intervalMs < 500)   intervalMs = 500;
    if (intervalMs > 60000) intervalMs = 60000;
    intervalMs_ = intervalMs;
}
BeadsPoll::~BeadsPoll() { stop(); }

void BeadsPoll::start() {
    if (started_) return;
    started_ = true;
    timer_ = g_timeout_add(intervalMs_, onTimer, this);
}
void BeadsPoll::stop() {
    if (!started_) return;
    started_ = false;
    paused_ = false;
    generation_++;   // invalidate any in-flight completion
    if (timer_) { g_source_remove(timer_); timer_ = 0; }
    inFlight_ = false;
    havePayload_ = false;
    lastPayload_.clear();
}
void BeadsPoll::kick() {
    if (!started_ || paused_ || inFlight_) return;
    tick();
}
gboolean BeadsPoll::onTimer(gpointer self) {
    static_cast<BeadsPoll *>(self)->tick();
    return G_SOURCE_CONTINUE;
}

void BeadsPoll::tick() {
    if (paused_) return;
    if (inFlight_) return;
    if (!runner_) return;
    inFlight_ = true;
    unsigned long gen = generation_;
    runner_->listAllIssues([this, gen](const BdResultPtr &res) {
        if (gen != generation_) return;   // stop() / re-bind raced us
        inFlight_ = false;
        if (!res->ok) {
            g_message("[NppBeads] poll: bd list failed (%s)",
                      res->errorMessage.empty() ? "?" : res->errorMessage.c_str());
            return;
        }
        std::string payload = res->rawStdout;
        if (!havePayload_) { lastPayload_ = payload; havePayload_ = true; return; }
        if (payload == lastPayload_) return;
        lastPayload_ = payload;
        if (onChange) onChange(payload);
    });
}
