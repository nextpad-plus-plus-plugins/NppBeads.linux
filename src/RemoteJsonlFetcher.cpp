// RemoteJsonlFetcher.cpp — see header. Ported from RemoteJsonlFetcher.mm.

#include "RemoteJsonlFetcher.h"

#include <glib.h>
#include <libsoup/soup.h>
#include <libsecret/secret.h>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────
//  Spec parsing
// ─────────────────────────────────────────────────────────────────────

static std::string trimWS(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string lower(const std::string &s) {
    gchar *d = g_ascii_strdown(s.c_str(), -1);
    std::string out = d;
    g_free(d);
    return out;
}

// "owner/repo" from assorted GitHub shapes; false when not GitHub-like.
static bool parseGitHubOwnerRepo(const std::string &input,
                                 std::string *owner, std::string *repo) {
    std::string s = input;
    for (const char *prefix : {"https://", "http://"}) {
        if (g_str_has_prefix(lower(s).c_str(), prefix)) { s = s.substr(strlen(prefix)); break; }
    }
    for (const char *host : {"www.github.com/", "github.com/"}) {
        if (g_str_has_prefix(lower(s).c_str(), host)) { s = s.substr(strlen(host)); break; }
    }
    if (g_str_has_suffix(lower(s).c_str(), ".git")) s = s.substr(0, s.size() - 4);
    while (!s.empty() && s.back() == '/') s.pop_back();
    size_t slash = s.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= s.size()) return false;
    if (s.find('/', slash + 1) != std::string::npos) return false;
    std::string o = s.substr(0, slash), r = s.substr(slash + 1);
    // Owner/repo charsets are tight; anything URL-ish here means the input
    // wasn't a GitHub repo reference after all.
    if (o.find(':') != std::string::npos || o.find('@') != std::string::npos) return false;
    if (owner) *owner = o;
    if (repo)  *repo  = r;
    return true;
}

std::string RemoteJsonlFetcher::specForInput(const std::string &input,
                                             const std::string &branch,
                                             std::string *outDisplay) {
    std::string in = trimWS(input);
    if (in.empty()) return "";
    std::string br = trimWS(branch);
    if (br.empty()) br = "main";

    std::string owner, repo;
    if (parseGitHubOwnerRepo(in, &owner, &repo)) {
        if (outDisplay) *outDisplay = owner + "/" + repo + " @ " + br;
        return "github.com/" + owner + "/" + repo + "@" + br;
    }

    // Self-hosted forge or direct raw URL. http:// accepted (LAN forges).
    std::string lo = lower(in);
    if (g_str_has_prefix(lo.c_str(), "https://") || g_str_has_prefix(lo.c_str(), "http://")) {
        if (g_str_has_suffix(lo.c_str(), ".git")) in = in.substr(0, in.size() - 4);
        GUri *uri = g_uri_parse(in.c_str(), G_URI_FLAGS_NONE, nullptr);
        if (!uri) return "";
        const char *host = g_uri_get_host(uri);
        const char *path = g_uri_get_path(uri);
        const char *query = g_uri_get_query(uri);
        int port = g_uri_get_port(uri);
        if (!host || !*host || !path || strlen(path) < 2) { g_uri_unref(uri); return ""; }
        std::string hostPort = host;
        if (port > 0) hostPort += ":" + std::to_string(port);

        // Repo-page URL — exactly /owner/repo, no query, not a file —
        // derives the Gitea/Forgejo raw layout:
        //   {scheme}://{host}/{owner}/{repo}/raw/branch/{branch}/.beads/issues.jsonl
        // (GitLab's /-/raw/ layout differs — paste its raw URL directly.)
        std::string p = path;
        while (!p.empty() && p.front() == '/') p.erase(0, 1);
        while (!p.empty() && p.back() == '/') p.pop_back();
        size_t slash = p.find('/');
        bool twoParts = slash != std::string::npos && slash > 0 &&
                        slash + 1 < p.size() &&
                        p.find('/', slash + 1) == std::string::npos;
        bool looksLikeFile = g_str_has_suffix(lower(p).c_str(), ".jsonl");
        std::string scheme = g_uri_get_scheme(uri);
        g_uri_unref(uri);

        if (twoParts && (!query || !*query) && !looksLikeFile) {
            std::string o = p.substr(0, slash), r = p.substr(slash + 1);
            gchar *brEnc = g_uri_escape_string(br.c_str(), "/", FALSE);
            std::string spec = scheme + "://" + hostPort + "/" + o + "/" + r +
                               "/raw/branch/" + brEnc + "/.beads/issues.jsonl";
            g_free(brEnc);
            if (outDisplay) *outDisplay = hostPort + "/" + o + "/" + r + " @ " + br;
            return spec;
        }
        // Anything else: a direct link to the raw file, fetched as-is.
        if (outDisplay) {
            gchar *base = g_path_get_basename(p.c_str());
            *outDisplay = std::string(host) + " (" + base + ")";
            g_free(base);
        }
        return in;
    }
    return "";
}

// ─────────────────────────────────────────────────────────────────────
//  Keyring — token storage. The token is the ONLY secret; it never
//  touches recent.ini or logs.
// ─────────────────────────────────────────────────────────────────────

static const SecretSchema *tokenSchema() {
    static const SecretSchema schema = {
        "org.nextpad.NppBeads.Remote", SECRET_SCHEMA_NONE,
        {
            { "spec", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { nullptr, (SecretSchemaAttributeType)0 },
        },
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    };
    return &schema;
}

void RemoteJsonlFetcher::storeToken(const std::string &token,
                                    const std::string &spec) {
    if (spec.empty()) return;
    GError *err = nullptr;
    if (token.empty()) {
        secret_password_clear_sync(tokenSchema(), nullptr, &err,
                                   "spec", spec.c_str(), nullptr);
    } else {
        secret_password_store_sync(tokenSchema(), SECRET_COLLECTION_DEFAULT,
                                   "NppBeads Remote", token.c_str(), nullptr,
                                   &err, "spec", spec.c_str(), nullptr);
    }
    if (err) {
        g_warning("[NppBeads] keyring %s failed: %s",
                  token.empty() ? "clear" : "store", err->message);
        g_error_free(err);
    }
}

std::string RemoteJsonlFetcher::tokenForSpec(const std::string &spec) {
    if (spec.empty()) return "";
    // Test seam (same family as NPPBEADS_BD): lets headless runs — which
    // have no Secret Service — inject a token without touching a keyring.
    if (const char *t = g_getenv("NPPBEADS_REMOTE_TOKEN")) return t;
    GError *err = nullptr;
    gchar *pw = secret_password_lookup_sync(tokenSchema(), nullptr, &err,
                                            "spec", spec.c_str(), nullptr);
    if (err) { g_error_free(err); return ""; }
    std::string out = pw ? pw : "";
    if (pw) secret_password_free(pw);
    return out;
}

// ─────────────────────────────────────────────────────────────────────
//  Polling
// ─────────────────────────────────────────────────────────────────────

RemoteJsonlFetcher::RemoteJsonlFetcher(const std::string &spec,
                                       const std::string &token)
    : spec_(spec), token_(token) {
    session_ = soup_session_new_with_options("timeout", 30,
                                             "user-agent", "NppBeads",
                                             nullptr);
    buildRequestURL();
}

RemoteJsonlFetcher::~RemoteJsonlFetcher() { stop(); if (session_) g_object_unref(session_); }

bool RemoteJsonlFetcher::buildRequestURL() {
    github_ = g_str_has_prefix(spec_.c_str(), "github.com/");
    if (github_) {
        std::string rest = spec_.substr(strlen("github.com/"));
        size_t at = rest.rfind('@');
        if (at == std::string::npos) return false;
        std::string ownerRepo = rest.substr(0, at);
        std::string branch    = rest.substr(at + 1);
        if (ownerRepo.empty() || branch.empty()) return false;
        gchar *orEnc = g_uri_escape_string(ownerRepo.c_str(), "/", FALSE);
        if (!token_.empty()) {
            gchar *brQ = g_uri_escape_string(branch.c_str(), nullptr, FALSE);
            url_ = std::string("https://api.github.com/repos/") + orEnc +
                   "/contents/.beads/issues.jsonl?ref=" + brQ;
            g_free(brQ);
        } else {
            gchar *brEnc = g_uri_escape_string(branch.c_str(), "/", FALSE);
            url_ = std::string("https://raw.githubusercontent.com/") + orEnc +
                   "/" + brEnc + "/.beads/issues.jsonl";
            g_free(brEnc);
        }
        g_free(orEnc);
    } else {
        url_ = spec_;
    }
    return !url_.empty();
}

void RemoteJsonlFetcher::start() {
    if (timerId_) return;
    fetch();
    timerId_ = g_timeout_add_seconds(60, +[](gpointer u) -> gboolean {
        auto *s = static_cast<RemoteJsonlFetcher *>(u);
        if (s->paused_) { s->pendingTick_ = true; return G_SOURCE_CONTINUE; }
        s->fetch();
        return G_SOURCE_CONTINUE;
    }, this);
}

void RemoteJsonlFetcher::stop() {
    if (timerId_) { g_source_remove(timerId_); timerId_ = 0; }
    generation_++;                 // silence any in-flight response
    inFlight_ = false;
    paused_ = false;
    pendingTick_ = false;
}

void RemoteJsonlFetcher::pause() { paused_ = true; }

void RemoteJsonlFetcher::resume() {
    if (!paused_) return;
    paused_ = false;
    if (pendingTick_) { pendingTick_ = false; fetch(); }
}

void RemoteJsonlFetcher::kick() {
    if (paused_) { pendingTick_ = true; return; }
    fetch();
}

struct RemoteFetchCtx {
    std::weak_ptr<int> alive;
    RemoteJsonlFetcher *self;
    unsigned gen;
    SoupMessage *msg;
};

void RemoteJsonlFetcher::fetch() {
    if (inFlight_ || url_.empty()) return;   // one in flight at a time
    SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, url_.c_str());
    if (!msg) { fail("bad URL"); return; }
    SoupMessageHeaders *h = soup_message_get_request_headers(msg);
    if (github_ && !token_.empty()) {
        soup_message_headers_replace(h, "Accept", "application/vnd.github.raw+json");
        soup_message_headers_replace(h, "X-GitHub-Api-Version", "2022-11-28");
    }
    if (!token_.empty())
        soup_message_headers_replace(h, "Authorization",
                                     ("Bearer " + token_).c_str());
    if (!etag_.empty())
        soup_message_headers_replace(h, "If-None-Match", etag_.c_str());

    inFlight_ = true;
    auto *ctx = new RemoteFetchCtx{alive_, this, generation_, msg};
    soup_session_send_and_read_async(session_, msg, G_PRIORITY_DEFAULT, nullptr,
        +[](GObject *src, GAsyncResult *res, gpointer u) {
            auto *ctx = static_cast<RemoteFetchCtx *>(u);
            GError *err = nullptr;
            GBytes *bytes = soup_session_send_and_read_finish(
                SOUP_SESSION(src), res, &err);
            auto alive = ctx->alive.lock();
            if (alive && ctx->self->generation_ == ctx->gen) {
                RemoteJsonlFetcher *s = ctx->self;
                s->inFlight_ = false;
                if (err) {
                    s->fail(std::string("network error — ") + err->message);
                } else {
                    unsigned status = soup_message_get_status(ctx->msg);
                    const char *etag = soup_message_headers_get_one(
                        soup_message_get_response_headers(ctx->msg), "ETag");
                    gsize len = 0;
                    const char *body = bytes
                        ? (const char *)g_bytes_get_data(bytes, &len) : nullptr;
                    s->handleResponse(status, etag, body, len);
                }
            }
            if (err) g_error_free(err);
            if (bytes) g_bytes_unref(bytes);
            g_object_unref(ctx->msg);
            delete ctx;
        }, ctx);
}

void RemoteJsonlFetcher::handleResponse(unsigned status, const char *etag,
                                        const char *body, size_t len) {
    if (status == 304) { lastError_.clear(); return; }      // unchanged
    if (status == 401 || status == 403) {
        fail("authorization failed (HTTP " + std::to_string(status) +
             ") — check the access token");
        return;
    }
    if (status == 404) {
        fail("not found (HTTP 404) — wrong repo/branch, no "
             ".beads/issues.jsonl, or a private repo without a token");
        return;
    }
    if (status != 200 || !body) {
        fail("fetch failed (HTTP " + std::to_string(status) + ")");
        return;
    }
    lastError_.clear();
    etag_ = etag ? etag : "";
    std::string text(body, len);
    // Without an ETag (some forges), gate on length so an identical body
    // doesn't thrash the webview every minute.
    if (etag_.empty() && text.size() == lastLength_ && lastLength_ > 0) return;
    lastLength_ = text.size();
    if (onText) onText(text);
}

void RemoteJsonlFetcher::fail(const std::string &message) {
    lastError_ = message;
    if (onError) onError(message);
}
