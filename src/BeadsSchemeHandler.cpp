// BeadsSchemeHandler.cpp — see header. Ported from BeadsSchemeHandler.mm.

#include "BeadsSchemeHandler.h"
#include <gio/gio.h>

static const char *mimeForExtension(const char *extLower) {
    if (!extLower) return "application/octet-stream";
    auto is = [&](const char *e) { return g_strcmp0(extLower, e) == 0; };
    if (is("html") || is("htm"))  return "text/html; charset=utf-8";
    if (is("js") || is("mjs"))    return "application/javascript; charset=utf-8";
    if (is("css"))                return "text/css; charset=utf-8";
    if (is("json") || is("map"))  return "application/json; charset=utf-8";
    if (is("wasm"))               return "application/wasm";
    if (is("png"))                return "image/png";
    if (is("jpg") || is("jpeg"))  return "image/jpeg";
    if (is("gif"))                return "image/gif";
    if (is("svg"))                return "image/svg+xml";
    if (is("ico"))                return "image/x-icon";
    if (is("webp"))               return "image/webp";
    if (is("woff"))               return "font/woff";
    if (is("woff2"))              return "font/woff2";
    if (is("ttf"))                return "font/ttf";
    if (is("otf"))                return "font/otf";
    if (is("txt") || is("md"))    return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

static std::string canonical(const std::string &p) {
    gchar *c = g_canonicalize_filename(p.c_str(), nullptr);
    std::string out = c ? c : "";
    g_free(c);
    return out;
}

BeadsSchemeHandler::BeadsSchemeHandler(const std::string &viewerRoot) {
    viewerRoot_ = canonical(viewerRoot);
    // strip a trailing slash for clean prefix comparison
    while (viewerRoot_.size() > 1 && viewerRoot_.back() == '/') viewerRoot_.pop_back();
}
BeadsSchemeHandler::~BeadsSchemeHandler() {}

void BeadsSchemeHandler::registerOn(WebKitWebContext *context) {
    context_ = context;
    webkit_web_context_register_uri_scheme(context, kScheme, handleRequest, this, nullptr);
    // Treat the scheme like a first-class local origin so module scripts,
    // workers and the WASM cross-origin-isolation path all behave.
    WebKitSecurityManager *sec = webkit_web_context_get_security_manager(context);
    webkit_security_manager_register_uri_scheme_as_local(sec, kScheme);
    webkit_security_manager_register_uri_scheme_as_cors_enabled(sec, kScheme);
    webkit_security_manager_register_uri_scheme_as_secure(sec, kScheme);
}

// requestPath is the URI path component, e.g. "/bridge.js". The host part of
// nppbeads://viewer/... is dropped by WebKit before we see the path, so the
// leading segment is just the file path under viewerRoot.
std::string BeadsSchemeHandler::resolvePath(const std::string &requestPath) const {
    std::string path = requestPath;
    if (path.empty() || path == "/") path = "/index.html";
    std::string joined = viewerRoot_ + (path[0] == '/' ? "" : "/") + path;
    std::string canon = canonical(joined);
    if (canon.empty()) return "";
    std::string rootSlash = viewerRoot_ + "/";
    if (canon == viewerRoot_) return canon;
    if (canon.compare(0, rootSlash.size(), rootSlash) != 0) return "";  // traversal
    return canon;
}

void BeadsSchemeHandler::handleRequest(WebKitURISchemeRequest *request, gpointer user) {
    auto *self = static_cast<BeadsSchemeHandler *>(user);
    const char *rawPath = webkit_uri_scheme_request_get_path(request);
    const char *uri = webkit_uri_scheme_request_get_uri(request);

    std::string filePath = self->resolvePath(rawPath ? rawPath : "");
    if (filePath.empty()) {
        GError *e = g_error_new(G_FILE_ERROR, G_FILE_ERROR_ACCES,
                                "NppBeads scheme: path outside viewerRoot (%s)", uri ? uri : "?");
        webkit_uri_scheme_request_finish_error(request, e);
        g_error_free(e);
        return;
    }

    GFile *f = g_file_new_for_path(filePath.c_str());
    GError *err = nullptr;
    GFileInfo *info = g_file_query_info(f, G_FILE_ATTRIBUTE_STANDARD_SIZE,
                                        G_FILE_QUERY_INFO_NONE, nullptr, &err);
    if (!info) {
        GError *e = g_error_new(G_FILE_ERROR, G_FILE_ERROR_NOENT,
                                "not found: %s", filePath.c_str());
        webkit_uri_scheme_request_finish_error(request, e);
        g_error_free(e);
        if (err) g_error_free(err);
        g_object_unref(f);
        return;
    }
    goffset size = g_file_info_get_size(info);
    g_object_unref(info);

    GFileInputStream *stream = g_file_read(f, nullptr, &err);
    g_object_unref(f);
    if (!stream) {
        GError *e = g_error_new(G_FILE_ERROR, G_FILE_ERROR_NOENT,
                                "not readable: %s", err ? err->message : filePath.c_str());
        webkit_uri_scheme_request_finish_error(request, e);
        g_error_free(e);
        if (err) g_error_free(err);
        return;
    }

    gchar *ext = nullptr;
    {
        gchar *base = g_path_get_basename(filePath.c_str());
        const char *dot = strrchr(base, '.');
        if (dot && dot[1]) ext = g_ascii_strdown(dot + 1, -1);
        g_free(base);
    }
    const char *mime = mimeForExtension(ext);

    // Set no-store + permissive CORS via a response object (matches macOS).
    WebKitURISchemeResponse *resp =
        webkit_uri_scheme_response_new(G_INPUT_STREAM(stream), size);
    webkit_uri_scheme_response_set_status(resp, 200, nullptr);
    webkit_uri_scheme_response_set_content_type(resp, mime);
    SoupMessageHeaders *hdrs = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
    // set_http_headers replaces the whole header set, so the content-type from
    // set_content_type won't survive unless we also put it here — and
    // WebAssembly.instantiateStreaming demands an exact "application/wasm".
    soup_message_headers_append(hdrs, "Content-Type", mime);
    soup_message_headers_append(hdrs, "Cache-Control", "no-store");
    soup_message_headers_append(hdrs, "Access-Control-Allow-Origin", "*");
    // WASM threads / SharedArrayBuffer want cross-origin isolation.
    soup_message_headers_append(hdrs, "Cross-Origin-Opener-Policy", "same-origin");
    soup_message_headers_append(hdrs, "Cross-Origin-Embedder-Policy", "require-corp");
    soup_message_headers_append(hdrs, "Cross-Origin-Resource-Policy", "cross-origin");
    webkit_uri_scheme_response_set_http_headers(resp, hdrs);  // takes ownership

    webkit_uri_scheme_request_finish_with_response(request, resp);
    g_object_unref(resp);
    g_object_unref(stream);
    g_free(ext);
}
