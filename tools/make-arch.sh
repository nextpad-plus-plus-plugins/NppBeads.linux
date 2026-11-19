#!/bin/bash
# Build the installable BeadsViewer Arch Linux package (.pkg.tar.zst) — the
# Arch counterpart of make-deb.sh / make-rpm.sh, following the Nextpad++
# app's make-arch.sh recipe (see its header for WHY a container and WHY the
# source tree is copied and filtered — the same reasoning holds: build-*/
# CMakeCaches pin host paths, and an Arch package must link ARCH's
# libraries, which run far ahead of the Ubuntu build hosts).
#
#   tools/make-arch.sh                       # build for THIS machine's arch
#   NPP_ARCH_REFRESH=1 tools/make-arch.sh    # re-pull Arch, rebuild image
#
# Output: dist/BeadsViewerv<VER>_<debarch>.pkg.tar.zst
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

VER=$(sed -n 's/^project(NppBeads VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$VER" ] || { echo "VERSION not found in CMakeLists.txt" >&2; exit 1; }

OCI=${NPP_OCI:-podman}
command -v "$OCI" >/dev/null || {
    echo "$OCI not found — install it with:  sudo apt install podman" >&2; exit 1; }

case "$(uname -m)" in
  x86_64)  BASE_IMAGE=docker.io/library/archlinux:base-devel; DEBARCH=amd64 ;;
  aarch64) BASE_IMAGE=${NPP_ARCH_IMAGE:-docker.io/menci/archlinuxarm:base-devel}; DEBARCH=arm64 ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac
BUILDER_IMAGE=localhost/beadsviewer-arch-build:latest

# ── builder image (cached; webkitgtk-6.0 is the heavy pull) ─────────────
if [ -n "${NPP_ARCH_REFRESH:-}" ] || ! "$OCI" image exists "$BUILDER_IMAGE" 2>/dev/null; then
    echo "── Building the Arch builder image (one-off, several minutes)"
    OLD_IMG=$("$OCI" image inspect --format '{{.Id}}' "$BUILDER_IMAGE" 2>/dev/null || true)
    "$OCI" pull "$BASE_IMAGE"
    CTX=$(mktemp -d /tmp/beadsviewer-arch-ctx-XXXXXX)
    trap 'rm -rf "$CTX"' EXIT
    printf '%s\n' \
      "FROM $BASE_IMAGE" \
      "# pacman >= 7 sandboxes downloads (Landlock); rootless podman cannot" \
      "# grant it inside the container — disable, as the app script does." \
      "RUN sed -i '/^\\[options\\]/a DisableSandbox' /etc/pacman.conf" \
      "# Three RUN layers on purpose: one giant layer (-Syu + the webkit" \
      "# stack + cache clean) peaked over this VM's free disk during the" \
      "# layer-commit tar (observed: 'no space left on device' writing" \
      "# WebKit-6.0.gir). Smaller layers commit one at a time." \
      "RUN pacman -Syu --noconfirm && pacman -Scc --noconfirm" \
      "RUN pacman -S --needed --noconfirm base-devel cmake pkgconf && \\" \
      "    pacman -Scc --noconfirm" \
      "RUN pacman -S --needed --noconfirm \\" \
      "        gtk4 libadwaita webkitgtk-6.0 json-glib libsoup3 libsecret && \\" \
      "    pacman -Scc --noconfirm" \
      "RUN useradd -m builder" \
    | "$OCI" build -t "$BUILDER_IMAGE" -f - "$CTX" >/dev/null
    rm -rf "$CTX"; trap - EXIT
    NEW_IMG=$("$OCI" image inspect --format '{{.Id}}' "$BUILDER_IMAGE")
    if [ -n "$OLD_IMG" ] && [ "$OLD_IMG" != "$NEW_IMG" ]; then
        "$OCI" rmi "$OLD_IMG" >/dev/null 2>&1 || true
    fi
    echo "── Builder image ready: $BUILDER_IMAGE"
fi

# ── filtered source copy ────────────────────────────────────────────────
STAGE=$(mktemp -d /tmp/beadsviewer-arch-XXXXXX)
trap 'rm -rf "$STAGE"' EXIT
SRCDIR="$STAGE/beadsviewer-$VER"

echo "── Staging a clean source copy"
rsync -a \
      --exclude='build/' --exclude='build-release/' --exclude='build-pkg/' \
      --exclude='.git/' --exclude='dist/' \
      --exclude='*.o' --exclude='*.a' --exclude='*.so' \
      --exclude='*.deb' --exclude='*.rpm' --exclude='*.pkg.tar.zst' \
      "$ROOT/" "$SRCDIR/"

tar -C "$STAGE" -czf "$STAGE/beadsviewer-$VER.tar.gz" "beadsviewer-$VER"
rm -rf "$SRCDIR"
cp packaging/PKGBUILD "$STAGE/"
sed -i "s/^pkgver=.*/pkgver=$VER/" "$STAGE/PKGBUILD"

# ── build ───────────────────────────────────────────────────────────────
echo "── Building BeadsViewer $VER in $BASE_IMAGE"
"$OCI" run --rm -v "$STAGE:/work" -w /work "$BUILDER_IMAGE" bash -c '
    chown -R builder:builder /work
    # PKGEXT pinned: Arch Linux ARM still defaults to .pkg.tar.xz.
    su builder -c "cd /work && PKGEXT=.pkg.tar.zst makepkg -f --noconfirm --nodeps --noprogressbar"
    rc=$?
    # Rootless podman: hand ownership back or the host cannot even delete
    # the staging dir (subuid-owned files inside a mode-700 mktemp dir).
    chown -R root:root /work
    exit $rc
'

PKG=$(ls -t "$STAGE"/*.pkg.tar.zst 2>/dev/null | grep -v -- '-debug-' | head -1 || true)
[ -n "$PKG" ] || { echo "makepkg produced no package" >&2; exit 1; }

mkdir -p "$ROOT/dist"
OUT="dist/BeadsViewerv${VER}_${DEBARCH}.pkg.tar.zst"
cp "$PKG" "$ROOT/$OUT"
echo "── Built $OUT"
"$OCI" run --rm -v "$STAGE:/w:ro" "$BUILDER_IMAGE" \
    bash -c "pacman -Qip '/w/$(basename "$PKG")'" 2>/dev/null | sed -n '1,12p'
