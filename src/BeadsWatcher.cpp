// BeadsWatcher.cpp — see header. GFileMonitor replaces the dispatch VNODE
// source; GFileMonitor already handles atomic write→rename (it re-stats the
// path), so we get the CHANGED/CREATED/DELETED events and debounce them.

#include "BeadsWatcher.h"

BeadsWatcher::BeadsWatcher() {}
BeadsWatcher::~BeadsWatcher() { stop(); }

void BeadsWatcher::stop() {
    if (debounceSrc_) { g_source_remove(debounceSrc_); debounceSrc_ = 0; }
    if (monitor_) {
        g_file_monitor_cancel(monitor_);
        g_object_unref(monitor_);
        monitor_ = nullptr;
    }
    watchedPath_.clear();
}

void BeadsWatcher::watchPath(const std::string &path) {
    if (path == watchedPath_ && monitor_) return;
    stop();
    watchedPath_ = path;
    if (path.empty()) return;

    GFile *f = g_file_new_for_path(path.c_str());
    GError *err = nullptr;
    // WATCH_MOVES catches the write→rename atomic-save pattern as one event set.
    monitor_ = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, nullptr, &err);
    g_object_unref(f);
    if (!monitor_) {
        g_message("[NppBeads] watch failed: %s -> %s", path.c_str(), err ? err->message : "?");
        if (err) g_error_free(err);
        return;
    }
    // ~200ms rate limit at the monitor level; our debounce coalesces further.
    g_file_monitor_set_rate_limit(monitor_, 200);
    g_signal_connect(monitor_, "changed", G_CALLBACK(on_changed), this);
}

void BeadsWatcher::on_changed(GFileMonitor *, GFile *, GFile *,
                              GFileMonitorEvent ev, gpointer user) {
    // Ignore attribute-only churn (ATTRIBUTE_CHANGED); react to content moves.
    switch (ev) {
        case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
        case G_FILE_MONITOR_EVENT_CREATED:
        case G_FILE_MONITOR_EVENT_DELETED:
        case G_FILE_MONITOR_EVENT_MOVED_IN:
        case G_FILE_MONITOR_EVENT_RENAMED:
        case G_FILE_MONITOR_EVENT_CHANGED:
            static_cast<BeadsWatcher *>(user)->fireDebounced();
            break;
        default: break;
    }
}

void BeadsWatcher::fireDebounced() {
    if (!onChange) return;
    if (debounceSrc_) g_source_remove(debounceSrc_);
    // 750 ms so agent-driven rapid writes coalesce into a single reload.
    debounceSrc_ = g_timeout_add(750, [](gpointer p) -> gboolean {
        auto *self = static_cast<BeadsWatcher *>(p);
        self->debounceSrc_ = 0;
        if (self->onChange) self->onChange();
        return G_SOURCE_REMOVE;
    }, this);
}
