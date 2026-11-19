# NppBeads — Beads panel for Nextpad++ (Linux) · BeadsViewer standalone app

A dockable side panel that embeds the [Beads](https://github.com/gastownhall/beads)
issue tracker inside [Nextpad++ for Linux](https://github.com/nextpad-plus-plus/nextpad-plus-plus-linux)
— and, from the same codebase, **BeadsViewer**, a standalone GTK4 desktop
application hosting the identical panel.

All viewer assets are bundled and served over a custom `nppbeads://` scheme;
no external fetches for local projects, fully offline.

## What you get (v1.0.2)

- **Five surfaces** — Dashboard, Insights (PageRank / Betweenness / Critical
  Path / k-core / articulation points / cycles + Project Velocity + Graph
  Health), Activity feed with every issue's full detail modal, Kanban Board
  with Raw/Effective status modes and drag-to-update, and a **3D
  force-directed dependency graph** (2D switch, heatmap, critical path,
  what-if overlays, facet filters)
- **beads ≥ 1.3 native** — binds Dolt-only workspaces (no `issues.jsonl`
  needed), renders live from the database via `bd export`, per-project Data
  Source choice with an `issues.jsonl` escape hatch, opt-in **live events
  feed** (`bd events tail --follow`) replacing the 2 s poll
- **Remote read-only mode** — monitor any repository's
  `.beads/issues.jsonl` over HTTP(S) with no clone and no bd: GitHub repos
  (fine-grained token for private ones), self-hosted Gitea/Forgejo repo
  URLs (raw layout derived automatically), or direct raw URLs; tokens live
  in the Secret Service keyring
- **Editor integration** (plugin) — bead-id detection (`bd-XXX`) in any open
  buffer with Jump / Copy id / Create issue from selection, painted on a
  host-allocated indicator
- **More-actions (⋯) menu** — switch project, reload issues/viewer, reveal
  `issues.jsonl`, Data Source + export, per-project **bd auto-push** opt-in,
  copy diagnostics to clipboard, raw JSONL head
- Writes (create / update / close / claim / deps / comments) go through the
  `bd` CLI — install it separately; without it everything is read-only

## Install

**Plugin:** Plugins ▸ Plugins Admin inside Nextpad++ (catalog name
“Beads”), or unzip `NppBeadsv<ver>-linux-<arch>.zip` from
[Releases](https://github.com/nextpad-plus-plus-plugins/NppBeads.linux/releases)
into `~/.local/share/nextpad++/plugins/`. Then **Plugins ▸ NppBeads ▸ Show
Beads panel**.

**Standalone app:** grab the `BeadsViewerv<ver>_<arch>` package for your
distro from the same Releases page — `.deb` (amd64 / arm64 / i386 / armhf),
`.rpm`, or Arch `.pkg.tar.zst` — or from the Snap Store:

```bash
sudo snap install beads
```

Launch with `BeadsViewer` (or from the applications grid). One window per
project; the dock icon's **New Window** opens more.

## Build from source

```bash
cd NppBeads.linux
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
cmake --install build --component plugin          # plugin → ~/.local/share/nextpad++/plugins/
cmake --install build --component app --prefix ~/.local   # BeadsViewer app
```

Dependencies (Ubuntu 24.04+): `libgtk-4-dev libadwaita-1-dev
libwebkitgtk-6.0-dev libjson-glib-dev libsoup-3.0-dev libsecret-1-dev`.
Packaging recipes for every shipped format live in `tools/` and
`snap/snapcraft.yaml`.

## Architecture

```
BeadsPanel (GTK4)
 ├── Title bar + status bar      (GtkBox · project chip, view switcher, ⋯ menu)
 ├── WebKitGTK 6.0 WebView       (serves nppbeads:// via a URI scheme handler)
 │    └── bundled viewer + bridge.js  (JSONL → sql.js synthesis, 3D graph)
 └── Data layer
      ├── JsonlDataSource        (issues.jsonl reader / adopted remote text)
      ├── BdCommandRunner        (bd CLI: reads, writes, probe, export)
      ├── BdEventsTail           (bd events journal follower, crash-orphan sweep)
      ├── RemoteJsonlFetcher     (libsoup3 conditional polling, keyring tokens)
      ├── BeadsProjectScanner    (walk-up auto-detect, Dolt-only aware)
      └── BeadsWatcher / BeadsPoll  (file watcher + poll fallback)
shell-app/                        BeadsViewer application around the panel
```

Serving everything through `nppbeads://` instead of `file://` keeps
ES-module dynamic imports and `WebAssembly.instantiateStreaming` working
under WebKit's file-origin rules.

## Licenses

- NppBeads / BeadsViewer: GPL-3.0-or-later
- Bundled viewer based on
  [Dicklesworthstone/beads_viewer](https://github.com/Dicklesworthstone/beads_viewer) (GPL)
- Beads itself: [gastownhall/beads](https://github.com/gastownhall/beads)
