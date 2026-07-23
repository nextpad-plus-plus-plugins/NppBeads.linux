// BeadIdIndicator — paints a link-style Scintilla indicator under bead ids
// (bd-XXX and hierarchical bd-XXX.1.2) in the active editor, and maps a caret
// byte offset back to the id under it. Ported from BeadIdIndicator.mm.
//
// Host-agnostic by construction: takes the Scintilla send function as a
// pointer (Linux: scintilla_view_send_message) and the active view as an
// opaque handle (Linux: the ScintillaView GtkWidget*).

#pragma once

#include <glib.h>
#include <string>
#include <vector>
#include <cstdint>

// Matches the Linux scintilla_view_send_message ABI.
using BeadIdSendMessageFn = intptr_t (*)(void *view, unsigned int msg,
                                         uintptr_t wParam, intptr_t lParam);

struct BeadIdMatch {
    intptr_t startByte = 0;
    intptr_t endByte = 0;
    std::string beadId;
};

class BeadIdIndicator {
public:
    explicit BeadIdIndicator(BeadIdSendMessageFn send);
    ~BeadIdIndicator();

    void setPrefix(const std::string &prefix);     // "" → "bd-"
    const std::string &prefix() const { return prefix_; }

    void setScintillaHandle(void *h);
    void *scintillaHandle() const { return handle_; }

    void scheduleRescan();     // 150 ms debounce, coalesced
    void rescanNow();
    void clearAll();

    // Query the cache for the bead id whose painted range covers byteOffset.
    std::string beadIdAtPosition(intptr_t byteOffset) const;
    const std::vector<BeadIdMatch> &currentMatches() const { return cache_; }

private:
    void installStylesIfNeeded();
    std::string escapedPrefix() const;
    intptr_t send(unsigned int msg, uintptr_t w = 0, intptr_t l = 0) const {
        return handle_ ? send_(handle_, msg, w, l) : 0;
    }

    BeadIdSendMessageFn send_;
    void *handle_ = nullptr;
    std::string prefix_ = "bd-";
    std::vector<BeadIdMatch> cache_;
    bool stylesInstalled_ = false;
    guint debounceSrc_ = 0;
};
