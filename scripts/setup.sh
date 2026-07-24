#!/usr/bin/env bash
# Copyright 2026 Niobium Microsystems, Inc.
# Licensed under the Apache License, Version 2.0.
#
# setup.sh — one-command dependency/environment setup for the Fog starter kit.
#
# Companion to scripts/build_task.sh:
#   setup.sh      gets your ENVIRONMENT ready (toolchain check, Python venv,
#                 OpenSSL) — run it ONCE per machine.
#   build_task.sh COMPILES the client + stages — run it whenever code changes.
#
#   scripts/setup.sh            # set up deps/env, then print the next steps
#   scripts/setup.sh --build    # ...and also run scripts/build_task.sh at the end
#
# Idempotent and safe to re-run. Nothing here needs sudo: system packages are
# only CHECKED up front (with an install hint if missing) — the preflight fails
# early if the toolchain, cmake version, Python venv support, or OpenSSL is
# absent, rather than after a multi-minute build. It does NOT install them. Python
# deps go in a local .venv (so no PEP 668 "externally-managed-environment" error).
# This does NOT touch your Fog account or ~/.fog — that's the "Run it on the Fog"
# step in the README Quickstart.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
OS="$(uname -s)"

DO_BUILD=0
for a in "$@"; do
  case "$a" in
    --build) DO_BUILD=1 ;;
    -h|--help) sed -n 's/^# \{0,1\}//p' "$0" | sed -n '1,20p'; exit 0 ;;
    *) echo "setup.sh: unknown argument '$a' (only --build is supported)" >&2; exit 2 ;;
  esac
done

say() { printf '\n=== %s\n' "$*"; }
FOGCLI="$ROOT/niobium-client/scripts/fog"

# Detect OpenSSL once — the preflight (step 1) uses it to fail early if it's
# missing, and step 4 reuses the result. macOS: Homebrew openssl@3 (the C++
# transport's find_package(OpenSSL) misses it otherwise), capturing the path in
# SSL. Linux: system dev files (libssl-dev), via pkg-config or the header.
SSL=""
have_openssl() {
  if [[ "$OS" == "Darwin" ]]; then
    if command -v brew >/dev/null 2>&1 && brew --prefix openssl@3 >/dev/null 2>&1; then
      SSL="$(brew --prefix openssl@3)"
    elif [[ -d /opt/homebrew/opt/openssl@3 ]]; then SSL=/opt/homebrew/opt/openssl@3
    elif [[ -d /usr/local/opt/openssl@3 ]]; then SSL=/usr/local/opt/openssl@3
    fi
    [[ -n "$SSL" ]]
  else
    { command -v pkg-config >/dev/null 2>&1 && pkg-config --exists openssl 2>/dev/null; } \
      || [[ -e /usr/include/openssl/ssl.h || -e /usr/local/include/openssl/ssl.h ]]
  fi
}

# --- 1. preflight: verify EVERYTHING setup + the build need, up front ----------
# Report + instruct; never auto-install system packages (no surprise sudo/brew).
# A green here must mean "you're good" — so we check what steps 2-4 and the build
# actually use, not just that binaries exist. Keep the install hints in sync with
# the README "What you need" section.
say "[1/4] preflight — toolchain, Python venv, OpenSSL"
missing=()
for t in git cmake make python3; do command -v "$t" >/dev/null 2>&1 || missing+=("$t"); done
if ! command -v c++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1 \
   && ! command -v g++ >/dev/null 2>&1; then missing+=("a C++ compiler"); fi
# python3 alone isn't enough: `python3 -m venv` (step 3) needs the venv+ensurepip
# module, a SEPARATE package on Debian/Ubuntu (python3-venv). Catch it here rather
# than crashing mid-setup with a cryptic "ensurepip is not available".
if command -v python3 >/dev/null 2>&1 && ! python3 -c 'import ensurepip, venv' >/dev/null 2>&1; then
  missing+=("python3-venv (the venv/ensurepip module)")
fi
# OpenSSL: the C++ transport's find_package(OpenSSL) needs it — catch it now, not
# after the multi-minute build. (On macOS this also captures SSL for step 4.)
have_openssl || missing+=("OpenSSL dev files (macOS: openssl@3 / Linux: libssl-dev)")
if ((${#missing[@]})); then
  echo "  missing: ${missing[*]}" >&2
  if [[ "$OS" == "Darwin" ]]; then
    echo "  install: xcode-select --install && brew install cmake openssl@3 python3" >&2
  else
    echo "  install: sudo apt-get update && sudo apt-get install -y build-essential cmake python3 python3-venv python3-pip libssl-dev zlib1g-dev git" >&2
  fi
  exit 1
fi
# `cmake` presence isn't enough — the build needs >= 3.18 (cmake_minimum_required).
cmake_ver="$(cmake --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' || true)"
if [[ -n "$cmake_ver" && "$(printf '3.18\n%s\n' "$cmake_ver" | sort -V | head -1)" != "3.18" ]]; then
  echo "  error: cmake $cmake_ver found, but the build needs >= 3.18" >&2
  exit 1
fi
echo "  ok: git, cmake ${cmake_ver:-?}, make, python3 (+venv), C++ compiler, OpenSSL"

# --- 2. niobium-client submodule (provides scripts/fog, used for the Fog run) -
say "[2/4] niobium-client submodule"
if [[ ! -e "$FOGCLI" ]]; then
  git submodule update --init niobium-client
fi
# Nested submodules (the client's OpenFHE source + libnbfhetch) — the big
# download the build needs. Shallow (--depth 1) to keep it small; fall back to a
# full recursive init if the server won't serve the pinned commit shallowly.
# Skipped when you point at a standalone client via NIOBIUM_CLIENT_DIR.
if [[ -z "${NIOBIUM_CLIENT_DIR:-}" && -e "$FOGCLI" ]]; then
  git -C niobium-client submodule update --init --recursive --depth 1 \
    || git -C niobium-client submodule update --init --recursive
fi
echo "  ok: niobium-client checked out ($([[ -e "$FOGCLI" ]] && echo 'scripts/fog present' || echo 'no scripts/fog — bump the pin'))"

# --- 3. Python venv + deps (this is what avoids the PEP 668 pip error) --------
say "[3/4] Python venv + deps (.venv)"
if [[ ! -d .venv ]]; then python3 -m venv .venv && echo "  created .venv"; else echo "  reusing .venv"; fi
.venv/bin/python -m pip install --quiet --upgrade pip
if [[ -f requirements.txt ]]; then
  .venv/bin/python -m pip install --quiet -r requirements.txt
  echo "  installed requirements.txt into .venv"
fi
# Put `fog` on PATH when the venv is active, so `source .venv/bin/activate`
# lets you type `fog …` instead of `niobium-client/scripts/fog …` — scoped to this
# repo, no global PATH changes. (Prefer it globally? See docs/FOG_CLI.md.)
# A wrapper, NOT a symlink: the fog CLI locates its transport client relative to
# __file__ WITHOUT resolving symlinks, so a symlinked `fog submit` dies with
# "nbcc_fhetch_replay not found (set NBCC_FHETCH_REPLAY_BIN)". The wrapper execs
# the real script, and resolves it relative to its own location so it survives a
# repo move. rm -f first: writing through a pre-existing symlink from an earlier
# setup run would clobber the client script itself.
if [[ -e "$FOGCLI" ]]; then
  rm -f .venv/bin/fog
  cat > .venv/bin/fog <<'WRAP'
#!/bin/sh
exec "$(cd "$(dirname "$0")/../.." && pwd)/niobium-client/scripts/fog" "$@"
WRAP
  chmod +x .venv/bin/fog
  echo "  installed .venv/bin/fog -> execs niobium-client/scripts/fog (type 'fog' with the venv active)"
fi

# --- 4. macOS: wire OpenSSL into the venv's activate (validated in the preflight) -
say "[4/4] OpenSSL (C++ transport TLS)"
if [[ "$OS" == "Darwin" ]]; then
  # SSL was captured by the preflight's OpenSSL check. Make `source
  # .venv/bin/activate` also export OPENSSL_ROOT_DIR, so activating the venv is the
  # one thing to remember (build_task.sh then picks it up).
  marker="# niobium-fog: OpenSSL for the C++ transport"
  if ! grep -qF "$marker" .venv/bin/activate 2>/dev/null; then
    printf '\n%s\nexport OPENSSL_ROOT_DIR=%q\n' "$marker" "$SSL" >> .venv/bin/activate
  fi
  echo "  OPENSSL_ROOT_DIR=$SSL (wired into .venv/bin/activate)"
else
  echo "  Linux: system OpenSSL is auto-detected by the build — nothing to wire"
fi

# --- done: next steps ---------------------------------------------------------
say "setup complete"
cat <<'EOF'
  Next:
    source .venv/bin/activate      # activates the venv (+ OPENSSL_ROOT_DIR on macOS)
    scripts/build_task.sh          # compile the client + stages (first time: many minutes)

  Then, with the venv active ('fog' is on PATH — no path prefix needed):
    python3 harness/run_submission.py 0 --op dot         # local run (CPU, no account)
    # Fog run — see the README "Run it on the Fog" step (fog login, then
    # fog submit ... --target FOG --skip-build).
EOF

if ((DO_BUILD)); then
  say "--build: running scripts/build_task.sh"
  [[ "$OS" == "Darwin" && -n "$SSL" ]] && export OPENSSL_ROOT_DIR="$SSL"
  exec scripts/build_task.sh
fi
