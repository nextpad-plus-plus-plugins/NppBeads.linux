// BeadsSchemeHandler — serves the bundled viewer over a custom URI scheme
// (Linux WebKitGTK port of the macOS WKURLSchemeHandler). Maps
// nppbeads://viewer/<path> to files under viewerRoot, with a traversal guard,
// MIME typing (incl. application/wasm), and no-store + permissive-CORS headers
// so the sql.js / graph WASM modules load cleanly.
//
// Registered once per WebKitWebContext. The context is shared by every panel
// web view, so the same handler instance serves all of them.

#pragma once

#include <webkit/webkit.h>
#include <string>

class BeadsSchemeHandler {
public:
    static constexpr const char *kScheme = "nppbeads";

    explicit BeadsSchemeHandler(const std::string &viewerRoot);
    ~BeadsSchemeHandler();

    // Register this handler's scheme on `context`. Safe to call once per
    // context; the handler owns the closure lifetime (freed in dtor).
    void registerOn(WebKitWebContext *context);

    const std::string &viewerRoot() const { return viewerRoot_; }

    // Resolve a request path to an absolute on-disk file, or "" if it escapes
    // viewerRoot. Exposed for the self-test.
    std::string resolvePath(const std::string &requestPath) const;

private:
    static void handleRequest(WebKitURISchemeRequest *request, gpointer user);

    std::string viewerRoot_;      // canonical, no trailing slash
    WebKitWebContext *context_ = nullptr;
};
