// RemoteJsonlFetcher — read-only live sync for REMOTE beads projects.
// Linux port of RemoteJsonlFetcher.mm (macOS nppmac-0th + nppmac-iik).
// Periodically GETs a repo's `.beads/issues.jsonl` over HTTP(S) (libsoup3,
// ETag/If-None-Match conditional polling, ~60 s) and hands fresh text to the
// panel, which pours it into JsonlDataSource via adoptRawText/fedExternally —
// the same pipeline dolt-priority render uses. No bd, no git, no local clone,
// no child processes.
//
// GitHub URL strategy (identical to macOS):
//   no token → https://raw.githubusercontent.com/{o}/{r}/{branch}/.beads/issues.jsonl
//   token    → https://api.github.com/repos/{o}/{r}/contents/.beads/issues.jsonl?ref={branch}
//              with `Accept: application/vnd.github.raw+json` and
//              `Authorization: Bearer <token>` (fine-grained PAT with
//              Contents: Read-only reaches private repos; authenticated
//              conditional GETs that 304 don't count against the rate limit).
// Self-hosted forges: a repo-page URL shaped like {scheme}://{host}[:port]/
// {owner}/{repo} auto-derives the Gitea/Forgejo raw layout
// ({repo}/raw/branch/{branch}/.beads/issues.jsonl); any other http(s) URL is
// fetched as-is (GitLab: paste its raw URL directly). A token, when present,
// is sent as Authorization: Bearer — Gitea accepts that form. http:// is
// allowed: LAN git servers are exactly where plain http lives (macOS relies
// on ATS to police public-hostname http; Linux has no ATS, the user's URL is
// the user's choice).
//
// Tokens live ONLY in the Secret Service keyring (libsecret; schema
// "org.nextpad.NppBeads.Remote", attribute spec). Never in the recent.ini,
// never logged.
//
// All callbacks fire on the GLib main loop. pause/resume follow the same
// window-key contract as BeadsPoll/BdEventsTail.

#pragma once

#include <functional>
#include <memory>
#include <string>

typedef struct _SoupSession SoupSession;

class RemoteJsonlFetcher {
public:
    // ── Spec parsing ─────────────────────────────────────────────────────
    // Turn user input — "owner/repo", "github.com/owner/repo",
    // "https://github.com/owner/repo(.git)", a self-hosted forge repo URL
    // ("http(s)://host[:port]/owner/repo"), or a direct http(s) URL to a raw
    // issues.jsonl — plus an optional branch (default "main") into the
    // canonical connection spec: "github.com/{o}/{r}@{branch}" for GitHub,
    // the (possibly derived) absolute raw URL string otherwise. The spec is
    // the stable identity: keyring account, MRU dedupe key, and
    // BeadsProject.remoteSpec. Returns "" when the input can't be
    // understood. outDisplay receives a short human label.
    static std::string specForInput(const std::string &input,
                                    const std::string &branch,
                                    std::string *outDisplay);

    // ── Keyring (schema org.nextpad.NppBeads.Remote, attr spec) ──────────
    static void storeToken(const std::string &token, const std::string &spec); // "" deletes
    static std::string tokenForSpec(const std::string &spec);

    // ── Polling ──────────────────────────────────────────────────────────
    RemoteJsonlFetcher(const std::string &spec, const std::string &token);
    ~RemoteJsonlFetcher();

    // Fires on every 200 whose body differs from the last delivery (ETag +
    // length gate). The string is the raw JSONL text.
    std::function<void(const std::string &jsonl)> onText;
    // Fires on auth/network/HTTP failures. Polling continues regardless.
    std::function<void(const std::string &message)> onError;
    // Sticky human-readable failure; "" again after the next good fetch.
    const std::string &lastError() const { return lastError_; }

    void start();    // immediate fetch, then every 60 s
    void stop();     // idempotent; cancels the timer + in-flight request
    void pause();    // window not key — ticks are skipped (latched)
    void resume();   // catch-up fetch now if a tick was skipped while paused
    void kick();     // user hit Refresh — fetch immediately

private:
    void fetch();
    void handleResponse(unsigned status, const char *etag,
                        const char *body, size_t len);
    void fail(const std::string &message);
    bool buildRequestURL();

    std::string spec_, token_;
    std::string url_;                 // resolved fetch URL
    bool github_ = false;             // GitHub spec (API headers when token)
    std::string etag_;
    size_t lastLength_ = 0;
    std::string lastError_;
    SoupSession *session_ = nullptr;
    unsigned timerId_ = 0;
    bool paused_ = false;
    bool pendingTick_ = false;
    bool inFlight_ = false;
    unsigned generation_ = 0;         // bumped on stop(); silences callbacks
    std::shared_ptr<int> alive_ = std::make_shared<int>(1);
};
