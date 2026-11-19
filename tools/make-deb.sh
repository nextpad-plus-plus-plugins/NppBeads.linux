#!/bin/bash
# Build the installable standalone BeadsViewer .deb — the same recipe shape
# as the Nextpad++ app's tools/make-deb.sh: fresh Release configure with the
# FINAL install prefix baked in (the .desktop Exec and the resources lookup
# both derive from CMAKE_INSTALL_PREFIX), DESTDIR staging, hand-rolled
# DEBIAN metadata, dpkg-deb.
#
# Output: dist/BeadsViewerv<VERSION>_<arch>.deb
set -euo pipefail
cd "$(dirname "$0")/.."

VER=$(sed -n 's/^project(NppBeads VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$VER" ] || { echo "VERSION not found in CMakeLists.txt" >&2; exit 1; }
ARCH=$(dpkg --print-architecture)

# Memory-aware job cap (same rationale as the app script: small build VMs).
if [ -n "${NPP_JOBS:-}" ]; then
    JOBS=$NPP_JOBS
else
    MEM_MB=$(awk '/MemTotal/{print int($2/1024)}' /proc/meminfo)
    JOBS=$(nproc); CAP=$(( MEM_MB / 500 )); [ "$CAP" -lt 1 ] && CAP=1
    [ "$JOBS" -gt "$CAP" ] && JOBS=$CAP
fi

STAGE=$(mktemp -d /tmp/beadsviewer-pkg-XXXXXX)
trap 'rm -rf "$STAGE"' EXIT

echo "── Building BeadsViewer $VER ($ARCH, -j$JOBS)"
cmake -B build-pkg -S . \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-pkg -j"$JOBS" --target BeadsViewer
DESTDIR="$STAGE" cmake --install build-pkg --component app > /dev/null
# The install rules also carry the plugin's toolbar.png into
# share/beadsviewer/resources — harmless, and the panel code paths expect
# the same resources tree the plugin ships.

mkdir -p "$STAGE/DEBIAN"

# Runtime links (ldd-checked): gtk4, libadwaita, webkitgtk 6.0, json-glib,
# libsoup3 (remote read-only mode), libsecret (remote tokens in the Secret
# Service keyring). The beads `bd` CLI is optional — the viewer is read-only
# without it — and not packaged by distros, so it is only mentioned in the
# description.
cat > "$STAGE/DEBIAN/control" <<EOF
Package: beadsviewer
Version: $VER
Section: devel
Priority: optional
Architecture: $ARCH
Maintainer: Andrew Letov <aletik@gmail.com>
Depends: libgtk-4-1, libadwaita-1-0, libwebkitgtk-6.0-4, libjson-glib-1.0-0, libsoup-3.0-0, libsecret-1-0, libglib2.0-0t64 | libglib2.0-0, libstdc++6, libc6
Description: Viewer for the Beads (bd) issue tracker
 Standalone desktop viewer for Beads (https://github.com/gastownhall/beads)
 projects. Dashboard, Insights, Activity, Kanban board and a 3D dependency
 graph; binds local projects (beads >= 1.3 Dolt workspaces or issues.jsonl,
 live via the bd CLI when installed) and remote repositories read-only over
 HTTP(S) (GitHub, self-hosted Gitea/Forgejo, raw URLs) with access tokens
 kept in the Secret Service keyring. One window per project; editing
 requires the separately installed bd CLI.
EOF

# postinst/postrm: refresh the icon and desktop caches so the dock icon and
# the "New Window" desktop action appear without a re-login. Guarded — the
# package must install cleanly on minimal systems.
cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
    gtk-update-icon-cache -q /usr/share/icons/hicolor 2>/dev/null || true
command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database -q /usr/share/applications 2>/dev/null || true
exit 0
EOF
cat > "$STAGE/DEBIAN/postrm" <<'EOF'
#!/bin/sh
set -e
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
    gtk-update-icon-cache -q /usr/share/icons/hicolor 2>/dev/null || true
command -v update-desktop-database >/dev/null 2>&1 && \
    update-desktop-database -q /usr/share/applications 2>/dev/null || true
exit 0
EOF
chmod 755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm"

mkdir -p dist
OUT="dist/BeadsViewerv${VER}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$STAGE" "$OUT" > /dev/null
echo "── Built $OUT"
dpkg-deb -I "$OUT" | sed -n '1,6p'
