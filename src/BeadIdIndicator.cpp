// BeadIdIndicator.cpp — see header. Ported from BeadIdIndicator.mm.

#include "BeadIdIndicator.h"
#include <algorithm>
#include <cstring>

// Minimal Scintilla constants (duplicated to avoid pulling in Scintilla.h —
// the plugin resolves scintilla_view_send_message at dlopen, it doesn't link
// the Scintilla API for its own types).
#define SCI_INDICSETSTYLE       2080
#define SCI_INDICSETFORE        2082
#define SCI_SETINDICATORCURRENT 2500
#define SCI_INDICATORFILLRANGE  2504
#define SCI_INDICATORCLEARRANGE 2505
#define SCI_GETLENGTH           2006
#define SCI_GETFIRSTVISIBLELINE 2152
#define SCI_LINESONSCREEN       2370
#define SCI_GETLINECOUNT        2154
#define SCI_POSITIONFROMLINE    2167
#define SCI_SETSEARCHFLAGS      2198
#define SCI_SETTARGETRANGE      2643
#define SCI_GETTARGETSTART      2644
#define SCI_GETTARGETEND        2645
#define SCI_SEARCHINTARGET      2197
#define SCI_DOCLINEFROMVISIBLE  2221
#define SCI_GETCHARAT           2007

#define INDIC_TEXTFORE          17
#define SCFIND_REGEXP           0x00200000
#define SCFIND_CXX11REGEX       0x00400000

// Indicator slot. Host uses 9-13 (Mark styles) and 28 (inc-search); 25 is
// clear. Plugin convention is 20-30.
static const int kBeadIndicator = 25;
// Link color. Scintilla color is 0x00BBGGRR. #2563eb → BGR 0xEB6325.
static const int kBeadLinkColor = 0xEB6325;

BeadIdIndicator::BeadIdIndicator(BeadIdSendMessageFn send) : send_(send) {}
BeadIdIndicator::~BeadIdIndicator() {
    if (debounceSrc_) g_source_remove(debounceSrc_);
}

void BeadIdIndicator::setPrefix(const std::string &prefix) {
    std::string p = prefix;
    // trim
    size_t a = p.find_first_not_of(" \t\r\n");
    size_t b = p.find_last_not_of(" \t\r\n");
    p = (a == std::string::npos) ? "" : p.substr(a, b - a + 1);
    if (p.empty()) p = "bd-";
    prefix_ = p;
    cache_.clear();
    scheduleRescan();
}

void BeadIdIndicator::setScintillaHandle(void *h) {
    if (handle_ == h) return;
    handle_ = h;
    stylesInstalled_ = false;   // re-install against the new view
    cache_.clear();
}

void BeadIdIndicator::scheduleRescan() {
    if (!handle_) return;
    if (debounceSrc_) return;   // already pending — coalesce
    debounceSrc_ = g_timeout_add(150, [](gpointer p) -> gboolean {
        auto *self = static_cast<BeadIdIndicator *>(p);
        self->debounceSrc_ = 0;
        self->rescanNow();
        return G_SOURCE_REMOVE;
    }, this);
}

void BeadIdIndicator::installStylesIfNeeded() {
    if (stylesInstalled_ || !handle_) return;
    send(SCI_INDICSETSTYLE, (uintptr_t)kBeadIndicator, INDIC_TEXTFORE);
    send(SCI_INDICSETFORE,  (uintptr_t)kBeadIndicator, (intptr_t)kBeadLinkColor);
    stylesInstalled_ = true;
}

std::string BeadIdIndicator::escapedPrefix() const {
    static const char *special = "\\.^$|?*+()[]{}";
    std::string out;
    for (char c : prefix_) {
        if (strchr(special, c)) out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

void BeadIdIndicator::rescanNow() {
    if (!handle_) return;
    installStylesIfNeeded();

    intptr_t firstVisible = send(SCI_GETFIRSTVISIBLELINE);
    intptr_t onScreen     = send(SCI_LINESONSCREEN);
    intptr_t totalLines   = send(SCI_GETLINECOUNT);

    intptr_t docFirst = send(SCI_DOCLINEFROMVISIBLE, (uintptr_t)std::max((intptr_t)0, firstVisible - 50));
    intptr_t docLast  = send(SCI_DOCLINEFROMVISIBLE, (uintptr_t)(firstVisible + onScreen + 50));
    if (docLast < docFirst) docLast = docFirst;
    if (docLast >= totalLines) docLast = totalLines;

    intptr_t startByte = send(SCI_POSITIONFROMLINE, (uintptr_t)docFirst);
    intptr_t endByte   = (docLast >= totalLines) ? send(SCI_GETLENGTH)
                                                 : send(SCI_POSITIONFROMLINE, (uintptr_t)docLast);
    if (startByte < 0) startByte = 0;
    if (endByte < startByte) endByte = startByte;

    // Clear only the scan window (persistent-across-scroll, no flicker).
    send(SCI_SETINDICATORCURRENT, (uintptr_t)kBeadIndicator);
    send(SCI_INDICATORCLEARRANGE, (uintptr_t)startByte, (intptr_t)(endByte - startByte));

    std::vector<BeadIdMatch> matches;
    std::string pattern = "\\b" + escapedPrefix() + "[a-z0-9]+(\\.\\d+)*\\b";
    const char *patUtf8 = pattern.c_str();
    intptr_t patLen = (intptr_t)pattern.size();

    send(SCI_SETSEARCHFLAGS, (uintptr_t)(SCFIND_REGEXP | SCFIND_CXX11REGEX));

    intptr_t cursor = startByte;
    const intptr_t kMaxMatches = 4096;
    intptr_t safety = 0;
    while (cursor < endByte && safety++ < kMaxMatches) {
        send(SCI_SETTARGETRANGE, (uintptr_t)cursor, (intptr_t)endByte);
        intptr_t found = send(SCI_SEARCHINTARGET, (uintptr_t)patLen, (intptr_t)patUtf8);
        if (found < 0) break;   // -1 not found, -2 invalid regex
        intptr_t mStart = send(SCI_GETTARGETSTART);
        intptr_t mEnd   = send(SCI_GETTARGETEND);
        if (mEnd <= mStart) break;

        send(SCI_INDICATORFILLRANGE, (uintptr_t)mStart, (intptr_t)(mEnd - mStart));

        std::string id_;
        id_.reserve((size_t)(mEnd - mStart));
        for (intptr_t i = 0; i < mEnd - mStart; i++)
            id_.push_back((char)send(SCI_GETCHARAT, (uintptr_t)(mStart + i)));
        if (!id_.empty()) matches.push_back({ mStart, mEnd, id_ });

        cursor = (mEnd == cursor) ? mEnd + 1 : mEnd;   // zero-length guard
    }

    cache_ = std::move(matches);
}

void BeadIdIndicator::clearAll() {
    if (!handle_) return;
    installStylesIfNeeded();
    intptr_t len = send(SCI_GETLENGTH);
    send(SCI_SETINDICATORCURRENT, (uintptr_t)kBeadIndicator);
    send(SCI_INDICATORCLEARRANGE, 0, len);
    cache_.clear();
}

std::string BeadIdIndicator::beadIdAtPosition(intptr_t byteOffset) const {
    long lo = 0, hi = (long)cache_.size() - 1;
    while (lo <= hi) {
        long mid = (lo + hi) >> 1;
        const BeadIdMatch &m = cache_[mid];
        if (byteOffset < m.startByte)     hi = mid - 1;
        else if (byteOffset >= m.endByte) lo = mid + 1;
        else                              return m.beadId;
    }
    return "";
}
