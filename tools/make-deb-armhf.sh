#!/bin/bash
# Build the 32-bit ARM (armhf) BeadsViewer .deb inside a Debian TRIXIE
# container — the 32-bit leg of the package matrix, following the Nextpad++
# app's make-deb-armhf.sh shape.
#
# WHY TRIXIE, NOT BOOKWORM (the app uses bookworm for the lowest symbol
# floor): BeadsViewer and the panel use GTK 4.10+ APIs throughout
# (GtkFileDialog, GtkAlertDialog, GtkFileLauncher) — bookworm's GTK 4.8
# cannot compile them. Trixie (GTK 4.18, t64 ABI) is also what current
# 32-bit derivatives ship.
#
# WHY A CONTAINER: armhf executes natively on any x86_64 CPU — full build
# HOW IT RUNS: qemu-user emulation on the x86_64 build host (slow but
# scripted; same flow as the app armhf deb). Target: Pi OS 32-bit (trixie).
#
# Output: dist/BeadsViewerv<VER>_armhf.deb
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

VER=$(sed -n 's/^project(NppBeads VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$VER" ] || { echo "VERSION not found in CMakeLists.txt" >&2; exit 1; }
[ "$(uname -m)" = x86_64 ] || {
    echo "make-deb-armhf.sh needs an x86_64 host; run it on ubuntu24." >&2; exit 1; }
command -v podman >/dev/null 2>&1 || { echo "podman required" >&2; exit 1; }

# armhf is a different instruction set: needs qemu-user-static + binfmt
# (installed on ubuntu24). Preflight like the app script does.
podman run --rm --platform linux/arm/v7 docker.io/library/debian:trixie true     2>/dev/null || {
    echo "cannot run armhf containers here — need qemu-user-static + binfmt" >&2
    exit 1; }

IMAGE=beadsviewer-armhf-build
if ! podman image exists "$IMAGE"; then
    echo "── building the $IMAGE container image (one-time)"
    printf '%s\n' \
      "FROM docker.io/library/debian:trixie" \
      "RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive \\" \
      "    apt-get install -y -qq build-essential cmake pkg-config \\" \
      "        libgtk-4-dev libadwaita-1-dev libwebkitgtk-6.0-dev \\" \
      "        libjson-glib-dev libsoup-3.0-dev libsecret-1-dev file && \\" \
      "    rm -rf /var/lib/apt/lists/*" \
    | podman build --platform linux/arm/v7 -t "$IMAGE" -f - "$ROOT" >/dev/null
fi

STAGE="$HOME/beads-armhf-build"
rsync -a --delete \
      --exclude='build*/' --exclude=dist/ --exclude=.git/ \
      --exclude='*.o' --exclude='*.a' --exclude='*.so' \
      --exclude=parts/ --exclude=stage/ --exclude=prime/ \
      "$ROOT/" "$STAGE/"

echo "── Building BeadsViewer $VER (armhf, trixie container)"
podman run --rm --platform linux/arm/v7 -v "$STAGE":/src "$IMAGE" \
    bash -c "cd /src && tools/make-deb.sh"

mkdir -p "$ROOT/dist"
cp "$STAGE/dist/BeadsViewerv${VER}_armhf.deb" "$ROOT/dist/"
echo "── dist/BeadsViewerv${VER}_armhf.deb"
