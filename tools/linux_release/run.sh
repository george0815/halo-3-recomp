#!/usr/bin/env bash
# Builds the Linux release in an Ubuntu 22.04 container (Containerfile,
# build.sh) with podman, so it runs on distributions with glibc 2.35 and
# later. Needs tools/build_windows_deps.sh's sources (SDL3, shaderc) in
# build/windows-deps-src. Output: build/package_linux_compat and the archive
# in build/release_archives.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# cgroupfs: podman's systemd cgroup manager fails in some sessions.
PODMAN=(podman --cgroup-manager=cgroupfs)
"${PODMAN[@]}" build -t h3recomp-linux-release -f "$ROOT/tools/linux_release/Containerfile" "$ROOT/tools/linux_release"
"${PODMAN[@]}" run --rm -v "$ROOT:/src" --userns=keep-id h3recomp-linux-release bash /src/tools/linux_release/build.sh
