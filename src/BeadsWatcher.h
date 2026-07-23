// BeadsWatcher — GFileMonitor on issues.jsonl (Linux port of the dispatch
// VNODE watcher). Coalesces rapid writes with a 750 ms debounce; re-arms on
// the atomic write→rename pattern. onChange fires on the main loop.

#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <functional>
#include <string>

class BeadsWatcher {
public:
    BeadsWatcher();
    ~BeadsWatcher();

    std::function<void()> onChange;

    void watchPath(const std::string &path);   // "" to unbind
    void stop();
    const std::string &watchedPath() const { return watchedPath_; }

private:
    static void on_changed(GFileMonitor *, GFile *, GFile *, GFileMonitorEvent, gpointer);
    void fireDebounced();

    GFileMonitor *monitor_ = nullptr;
    std::string watchedPath_;
    guint debounceSrc_ = 0;
};
