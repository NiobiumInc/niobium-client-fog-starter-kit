#!/usr/bin/env bash
# Copyright 2026 Niobium Microsystems, Inc.
# Licensed under the Apache License, Version 2.0.
#
# build_task.sh — self-contained SDK build. The
# niobium-client submodule builds its OWN OpenFHE + libnbfhetch + transport, and
# the dot-product stages build against that. No compiler checkout needed.
# Run from anywhere; resolves the repo root itself. See docs/NIOBIUM_CLIENT_TRANSPORT.md.
#
# The submodule gitlink is already pinned to a Fog-capable client commit
# (one with scripts/fog) and COMMITTED, so the `git submodule update` below is a
# no-op and cannot revert it. That is the whole reason the pin is committed: an
# uncommitted bump gets reset here. (`git submodule status` shows the exact pin.)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Client location: $NIOBIUM_CLIENT_DIR (a standalone clone you manage) else the
# vendored submodule. See docs/USING_THE_CLIENT.md.
CLIENT_DIR="${NIOBIUM_CLIENT_DIR:-$ROOT/niobium-client}"

if [[ -z "${NIOBIUM_CLIENT_DIR:-}" ]]; then
  echo "=== [1/3] sync the vendored niobium-client submodule (+ nested) ==="
  git submodule update --init niobium-client
  git -C niobium-client submodule update --init --recursive
else
  echo "=== [1/3] using standalone client: $CLIENT_DIR (skipping submodule sync) ==="
  [[ -d "$CLIENT_DIR" ]] || { echo "error: NIOBIUM_CLIENT_DIR '$CLIENT_DIR' not found" >&2; exit 1; }
fi

echo "=== [2/3] build + install the client's OpenFHE + libnbfhetch + transport (make release, install-release) ==="
echo "         (heavy the first time — compiles OpenFHE once; a no-op if already built)"
if [[ "$(uname -s)" == "Darwin" && -z "${OPENSSL_ROOT_DIR:-}" ]]; then
  echo "warning (macOS): OPENSSL_ROOT_DIR is unset — the client's HTTPS transport may build" >&2
  echo "         without TLS, and 'fog submit' would then fail. Export it first:" >&2
  echo "         export OPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3   # Intel: /usr/local/opt/openssl@3" >&2
fi
make -C "$CLIENT_DIR" release        # installs OpenFHE to <client>/vendor/lib/openfhe

OPENFHE_PREFIX="$CLIENT_DIR/vendor/lib/openfhe"
[[ -d "$OPENFHE_PREFIX" ]] || {
  echo "error: client OpenFHE not at $OPENFHE_PREFIX after 'make release'" >&2; exit 1; }

# `release` builds libnbfhetch but leaves it in the build tree. `install-release`
# puts it, its headers, and NiobiumFhetchConfig.cmake under
# <client>/vendor/lib/niobium-client, which is what lets a SEPARATE app resolve the
# SDK with find_package(NiobiumFhetch) — the stages in this repo link it directly,
# so this step is for the apps you build next against the same client.
make -C "$CLIENT_DIR" install-release
SDK_PREFIX="$CLIENT_DIR/vendor/lib/niobium-client"
# The config is installed to <prefix>/<libdir>/cmake/NiobiumFhetch, and <libdir> is
# lib or lib64 depending on the platform, so look for the file rather than a fixed path.
if [[ -z "$(find "$SDK_PREFIX" -name NiobiumFhetchConfig.cmake -print -quit 2>/dev/null)" ]]; then
  echo "warning: no NiobiumFhetchConfig.cmake under $SDK_PREFIX. This repo still builds" >&2
  echo "         (it links the client directly), but a separate app resolving the SDK" >&2
  echo "         with find_package(NiobiumFhetch) will not configure." >&2
fi

echo "=== [3/3] build the dot-product stages + SDK compute server against the client's OpenFHE ==="
# Wipe build/ if it was configured for a different repo path or a different
# client dir — a stale CMakeCache otherwise aborts configure ("CMakeCache
# directory is different", e.g. after moving the repo) or silently reuses the
# old client's cached library paths (after switching NIOBIUM_CLIENT_DIR).
if [[ -f build/CMakeCache.txt ]]; then
  prev_home=$(sed -n 's/^CMAKE_HOME_DIRECTORY:[^=]*=//p' build/CMakeCache.txt)
  prev_client=$(sed -n 's/^NB_CLIENT_DIR:[^=]*=//p' build/CMakeCache.txt)
  if [[ "$prev_home" != "$ROOT" || "$prev_client" != "$CLIENT_DIR" ]]; then
    echo "note: build/ was configured elsewhere (repo or client moved) — wiping for a clean configure."
    rm -rf build
  fi
fi
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DNIOBIUM_SDK_BUILD=ON \
  -DNB_CLIENT_DIR="$CLIENT_DIR" \
  -DCMAKE_PREFIX_PATH="$OPENFHE_PREFIX"
cmake --build build -j \
  --target dp_keygen dp_encrypt dp_decrypt dp_compute_sdk

echo "=== done: binaries in $ROOT/build ==="
ls -la build/dp_keygen build/dp_encrypt build/dp_decrypt build/dp_compute_sdk
