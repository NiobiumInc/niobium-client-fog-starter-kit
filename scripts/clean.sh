#!/usr/bin/env bash
# Copyright 2026 Niobium Microsystems, Inc.
# Licensed under the Apache License, Version 2.0.
#
# clean.sh — remove what runs and builds leave behind.
#
# Companion to scripts/setup.sh (environment) and scripts/build_task.sh (compile).
#
#   scripts/clean.sh            # run artifacts: keys, inputs, compiled programs, metrics
#   scripts/clean.sh --build    # ...and the build tree (the stages recompile next run)
#   scripts/clean.sh --all      # ...and the .venv, the client's build, fetched sources
#   scripts/clean.sh --dry-run  # list what would go, delete nothing
#
# Everything removed here is ignored by .gitignore, and the deletion runs through
# `git clean -X`, which only ever touches ignored files. A tracked file cannot be
# removed by this script, whatever the paths below say.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SCOPE=run
DRY=0
for a in "$@"; do
  case "$a" in
    --build)   SCOPE=build ;;
    --all)     SCOPE=all ;;
    --dry-run) DRY=1 ;;
    -h|--help) tail -n +2 "$0" | sed -n 's/^# \{0,1\}//p' | sed -n '1,15p'; exit 0 ;;
    *) echo "clean.sh: unknown argument '$a' (--build, --all, --dry-run)" >&2; exit 2 ;;
  esac
done

git rev-parse --git-dir >/dev/null 2>&1 || {
  echo "clean.sh: not a git repository, so there is no ignore list to clean against" >&2
  exit 1; }

FLAGS=(-Xd)
((DRY)) && FLAGS+=(-n) || FLAGS+=(-f)

say() { printf '\n=== %s\n' "$*"; }

# --all hands the whole ignore list to git, which adds .venv, third_party, and
# the client's own build tree. Rebuilding the client is the long step, so this
# scope is for reclaiming disk rather than for getting back to a run.
if [[ "$SCOPE" == all ]]; then
  say "clean --all: every ignored file in the working tree"
  git clean "${FLAGS[@]}"
  ((DRY)) || echo "  next step: scripts/setup.sh --build"
  exit 0
fi

# Per-run outputs: key sets and encrypted inputs under io/, the compiled programs
# and their traces, the metrics each run writes, and the replay working dirs.
paths=(io measurements serialized_probes)
for g in dotprod_compute_* nbcc_fhetch_replay_source_* fhetch_replay.json timing_summary_*.json; do
  for m in $g; do [ -e "$m" ] && paths+=("$m"); done
done
[[ "$SCOPE" == build ]] && paths+=(build)

existing=()
for p in "${paths[@]}"; do [ -e "$p" ] && existing+=("$p"); done

label="$SCOPE artifacts"
((DRY)) && label="$label (dry run)"
say "clean: $label"
if ((${#existing[@]} == 0)); then
  echo "  nothing to remove"
  exit 0
fi

freed=$(du -sk "${existing[@]}" 2>/dev/null | awk '{s+=$1} END {print s+0}')
git clean "${FLAGS[@]}" -- "${existing[@]}"
if ((DRY == 0)); then
  awk -v kb="$freed" 'BEGIN {printf "  freed %.1f MB\n", kb/1024}'
  [[ "$SCOPE" == build ]] && echo "  next step: any run rebuilds the stages, or scripts/build_task.sh"
fi
exit 0
