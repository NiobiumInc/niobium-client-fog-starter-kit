#!/usr/bin/env python3
# Copyright 2026 Niobium Microsystems, Inc.
# Licensed under the Apache License, Version 2.0.
"""
run_submission.py — drive the encrypted dot-product workload end to end.

Single pass: (build) -> keygen -> encrypt -> compute -> decrypt -> verify.
Compile once, run many: dp_compute_sdk records the program on a cache miss, then
runs it on the device whenever one is wired in (NBCC_FHETCH_SERVER, set by
`fog submit`). So a single `fog submit` compiles (if needed) and runs on the Fog
in one invocation — the first run is a cold start (record + run), later runs only
replay. A plain local run (no server) stops after the record with the CPU result.
This script never starts a server. See docs/NIOBIUM_CLIENT_TRANSPORT.md.

    python3 harness/run_submission.py 0 --target FOG            # local: compile + CPU-verify
    niobium-client/scripts/fog submit \\
        python3 harness/run_submission.py 0 --target FOG --skip-build   # compile + run on the Fog
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from params import InstanceParams, instance_name, TOY, SMALL, OPS, K_CONST, BIAS

ROOT = Path(__file__).resolve().parent.parent      # repo root
BUILD = ROOT / "build"                              # scripts/build_task.sh output
# The client is the vendored submodule by default, or a standalone clone if
# NIOBIUM_CLIENT_DIR is set (see docs/USING_THE_CLIENT.md). Same override is
# honored by scripts/build_task.sh and CMakeLists.txt.
CLIENT = Path(os.environ.get("NIOBIUM_CLIENT_DIR", ROOT / "niobium-client"))


def compute_binary() -> str:
    """The only compute stage — dp_compute_sdk. On the first run it compiles +
    writes the CPU result (local math verify, no server); on later runs it runs
    the compiled program over the transport (the Fog)."""
    return "dp_compute_sdk"


def lib_env() -> dict:
    """Runtime env for the stage binaries. Covers the client's OpenFHE +
    libnbfhetch. If a transport server is set (NBCC_FHETCH_SERVER — e.g. by
    `fog submit`), point NBCC_FHETCH_REPLAY at the client forwarder so replay()
    ships over HTTP. We NEVER set NBCC_FHETCH_SERVER/TOKEN ourselves — those flow
    in from `fog submit`."""
    env = os.environ.copy()
    libs = [ROOT / "third_party" / "openfhe" / "lib",         # CPU build
            CLIENT / "vendor" / "lib" / "openfhe" / "lib"]     # transport build
    for d in (CLIENT / "build" / "vendor" / "niobium-fhetch",
              CLIENT / "build" / "_deps" / "niobium-fhetch-build"):
        if d.exists():
            libs.append(d)
    libp = os.pathsep.join(str(x) for x in libs)
    for var in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
        env[var] = libp + (os.pathsep + env[var] if env.get(var) else "")
    if env.get("NBCC_FHETCH_SERVER") and not env.get("NBCC_FHETCH_REPLAY"):
        fwd = CLIENT / "build" / "src" / "fhetch_transport" / "nbcc_fhetch_replay"
        if fwd.exists():
            env["NBCC_FHETCH_REPLAY"] = str(fwd)
    return env


def parse_output(text: str) -> dict:
    out = {}
    m = re.findall(r"result\s*=\s*([-\d.eE+]+)", text)
    if m:
        out["result"] = float(m[-1])
    posts = re.findall(r"POSTing (\d+) bytes", text)
    if posts:
        out["post_bytes"] = int(posts[-1])
    return out


def _fog_conf():
    """Resolve the fog-api URL + token the same way niobium-client/scripts/fog
    does: env first (FOG_API_URL / FOG_API_TOKEN), else the [fog] section of
    ~/.fog/{config,credentials}. Returns (api_url, token|None)."""
    import configparser
    home = Path(os.environ.get("FOG_HOME") or (Path.home() / ".fog"))
    api = os.environ.get("FOG_API_URL")
    tok = os.environ.get("FOG_API_TOKEN")
    if not api:
        cp = configparser.ConfigParser(); cp.read(home / "config")
        api = cp.get("fog", "api_url", fallback="https://api.niobium.co")
    if not tok:
        cp = configparser.ConfigParser(); cp.read(home / "credentials")
        tok = cp.get("fog", "api_token", fallback=None)
    return api.rstrip("/"), tok


def fetch_fog_timing(retries: int = 5, delay: float = 1.0):
    """Best-effort: after a Fog run, pull the worker-side timing from fog-api's
    `GET /jobs/<id>/timing` (the worker's `timing_summary.json`, reported to the API
    after the job finishes). `fog submit` puts the job id in NBCC_FHETCH_SERVER
    (.../jobs/<id>/run), and the api token/url live in ~/.fog.

    Returns a dict with the two figures we surface (either may be absent):
      fog_wall_ms — the Fog worker's wall time, from the artifact whose content
                    carries `wall_ms`.
      fpga_ms     — on-device FPGA execution, from the `fpga_ms` field.
    Returns {} when it's not a Fog run (the local run) or the timing wasn't
    reachable. NEVER raises: timing is a bonus, not part of the PASS/FAIL
    contract. The retry loop covers the timing showing up shortly after the job
    completes (the two figures can arrive separately, so we accumulate across attempts)."""
    server = os.environ.get("NBCC_FHETCH_SERVER", "")
    m = re.search(r"/jobs/([^/]+)/run", server)
    if not m:
        return {}                                     # local run — no device
    jid = m.group(1)
    collected = {}
    try:
        import ssl, urllib.request, urllib.error
        try:
            import certifi
            ctx = ssl.create_default_context(cafile=certifi.where())
        except Exception:
            ctx = ssl.create_default_context()
        api, tok = _fog_conf()
        if not tok:
            return {}
        url = f"{api}/jobs/{jid}/timing"
        for attempt in range(retries):
            try:
                req = urllib.request.Request(url, headers={"X-Api-Token": tok})
                with urllib.request.urlopen(req, timeout=30, context=ctx) as resp:
                    arts = json.loads(resp.read() or b"[]")
            except (urllib.error.URLError, ValueError):
                arts = []
            for a in arts:
                content = a.get("content") if isinstance(a, dict) else None
                if not isinstance(content, dict):
                    continue
                if content.get("fpga_ms") is not None and "fpga_ms" not in collected:
                    collected["fpga_ms"] = float(content["fpga_ms"])
                if content.get("wall_ms") is not None and "fog_wall_ms" not in collected:
                    collected["fog_wall_ms"] = float(content["wall_ms"])
            if len(collected) == 2:
                return collected                      # both figures present
            if attempt < retries - 1:
                time.sleep(delay)                     # not drained yet — wait
    except Exception:
        pass
    return collected                                  # whatever we gathered (may be partial)


def run_query(size: int, args, params: InstanceParams):
    io = ROOT / "io" / instance_name(size)
    keydir, querydir = io / "keys", io / "query"   # keys + inputs shared across ops
    result = io / f"{args.op}.result.ct"           # result differs per op
    for d in (keydir, querydir):
        d.mkdir(parents=True, exist_ok=True)
    env = lib_env()

    def run(name, *a, capture=False):
        return subprocess.run([str(BUILD / name), *map(str, a)],
                              check=not capture, capture_output=capture, text=True, env=env)

    if not (keydir / "cc.bin").exists():
        run("dp_keygen", "--keydir", keydir)

    a_csv = ",".join(str(x) for x in params.vector_a())
    b_csv = ",".join(str(x) for x in params.vector_b())
    run("dp_encrypt", "--keydir", keydir, "--a", a_csv, "--b", b_csv, "--out", querydir)

    # Remove any stale result first: a failed compute/run must NOT be masked by a
    # leftover result.ct from an earlier run (that would be a false PASS).
    if result.exists():
        result.unlink()

    t0 = time.time()
    cbin = compute_binary()
    compute_args = ["--keydir", keydir, "--query-dir", querydir, "--result", result,
                    "--n", params.n, "--op", args.op, "--k", K_CONST, "--bias", BIAS]
    if cbin == "dp_compute_sdk":
        # MUST be "--opt-level O3", not "-O3": the SDK's init() reads --opt-level
        # (and forwards the right compile settings to the backend); a bare -O3 is
        # ignored at this layer.
        compute_args += [f"--target={args.target}", "--opt-level", f"O{args.optimization}"]
    # The compute stage runs with its output captured (silent) and, on the Fog, is
    # the slow step (upload + FPGA) — announce it so the prior `[encrypt]` line
    # doesn't look like what's hanging.
    if os.environ.get("NBCC_FHETCH_SERVER"):
        print("[harness] computing on the Fog — streaming keys + ciphertext to the worker, then "
              "running on the FPGA (the slow part; `fog list` shows progress)...", flush=True)
    else:
        print("[harness] computing on CPU...", flush=True)
    cp = run(cbin, *compute_args, capture=True)
    wall = round(time.time() - t0, 2)

    # Exit code 3 from dp_compute_sdk: the program is compiled but no device is
    # wired in (a plain local re-run). Not a failure — signal main() to show a
    # "run it on the Fog" nudge instead of a FAIL.
    if cp.returncode == 3:
        return {"wall_s": wall, "needs_device": True}, cp.stdout + cp.stderr

    # Only decrypt if the compute stage actually succeeded — otherwise there is no
    # (fresh) result to read, and proceeding would parse nothing (-> clean FAIL).
    if cp.returncode == 0:
        dp = run("dp_decrypt", "--keydir", keydir, "--result", result, capture=True)
        dp_out = "\n" + dp.stdout + dp.stderr
    else:
        dp_out = f"\n[harness] compute stage FAILED (exit {cp.returncode}); skipping decrypt"

    log = cp.stdout + cp.stderr + dp_out
    m = parse_output(log)
    m["wall_s"] = wall
    return m, log


def main() -> int:
    # Line-buffer our own output so progress lines (e.g. "computing…") appear live
    # and in order relative to the stage subprocesses, even when stdout is captured
    # or piped (a Fog submit, a log file) rather than a terminal.
    sys.stdout.reconfigure(line_buffering=True)
    p = argparse.ArgumentParser(description="Run the encrypted dot-product workload.")
    p.add_argument("size", type=int, choices=range(TOY, SMALL + 1),
                   help="Instance size (0=toy N=8, 1=small N=32).")
    p.add_argument("--op", choices=OPS, default="dot",
                   help="Which tiny circuit to run (default dot). Each leaves a "
                        "scalar in slot 0; see docs/NIOBIUM_CLIENT_TRANSPORT.md.")
    p.add_argument("--target", default="FOG",
                   help="Where the compiled program runs (default FOG).")
    p.add_argument("-O", "--optimization", type=int, choices=[0, 1, 2, 3], default=3,
                   help="Compile opt level (default 3 — required for the FPGA target; leave it).")
    p.add_argument("--tol", type=float, default=1e-2,
                   help="PASS window: relative error of the decrypted dot-product (default 1e-2).")
    p.add_argument("--skip-build", action="store_true",
                   help="Skip scripts/build_task.sh (assume build/ is present).")
    p.add_argument("--reset", action="store_true",
                   help="Clear this size's keys/inputs and the cached compiled program(s) so "
                        "the workload re-compiles from scratch. Use after editing the circuit "
                        "(src/) or the inputs (params.py).")
    args = p.parse_args()

    if not args.skip_build and not (BUILD / "dp_compute_sdk").exists():
        print("[harness] building (scripts/build_task.sh) ...")
        subprocess.run(["bash", str(ROOT / "scripts" / "build_task.sh")], check=True)

    params = InstanceParams(args.size)

    if args.reset:
        io_dir = ROOT / "io" / params.name
        if io_dir.exists():
            shutil.rmtree(io_dir, ignore_errors=True)
        # The compiled program is cached in dotprod_compute_*/ dirs written to the
        # working directory (and/or repo root); clear both so this recompiles.
        prog_dirs = {p for base in (Path.cwd(), ROOT)
                     for p in base.glob("dotprod_compute_*") if p.is_dir()}
        for d in prog_dirs:
            shutil.rmtree(d, ignore_errors=True)
        print(f"[harness] --reset: cleared io/{params.name} + {len(prog_dirs)} cached program(s)")

    expected = params.expected(args.op)
    # Show where the run actually happens, not the compile target: a plain local
    # run verifies on CPU (no NBCC_FHETCH_SERVER), a `fog submit` runs on the FPGA.
    on_fog = os.environ.get("NBCC_FHETCH_SERVER") is not None
    where = "Fog FPGA" if on_fog else "local CPU verify"
    print(f"\n[harness] === {params.name} op={args.op} (N={params.n}, {where}) ===")
    print(f"[harness] expected {args.op} result (cleartext, slot 0) = {expected}")

    metrics, log = run_query(args.size, args, params)

    if metrics.get("needs_device"):
        # Local re-run of an already-compiled (op, N): the compiled program runs on
        # a device, and none is wired in here. Show a Fog call-to-action, not a FAIL.
        md = ROOT / "measurements" / params.name
        md.mkdir(parents=True, exist_ok=True)
        (md / f"{args.op}.log").write_text(log)
        print(f"\n[harness] '{args.op}' is already compiled and verified locally.")
        print("[harness] To actually run it, submit it to the Fog:")
        print(f"[harness]     fog submit python3 harness/run_submission.py {args.size} "
              f"--op {args.op} --target FOG --skip-build")
        print("[harness] Get Fog access -> https://console.niobium.co/request-account")
        print("[harness] (or re-verify locally with --reset)")
        return 0

    timing = fetch_fog_timing()        # best-effort; {} when not on the Fog
    for key in ("fog_wall_ms", "fpga_ms"):
        if timing.get(key) is not None:
            metrics[key] = timing[key]
    got = metrics.get("result")
    rel = abs(got - expected) / max(abs(expected), 1e-9) if got is not None else None
    ok = rel is not None and rel <= args.tol
    verdict = "PASS" if ok else "FAIL"

    metrics.update(size=params.name, n=params.n, op=args.op, target=args.target,
                   expected=expected, rel_err=rel, verdict=verdict, passed=ok)
    md = ROOT / "measurements" / params.name
    md.mkdir(parents=True, exist_ok=True)
    (md / f"{args.op}.log").write_text(log)
    (md / f"{args.op}.json").write_text(json.dumps(metrics, indent=2))

    tail = "" if ok else "\n" + "\n".join(log.strip().splitlines()[-15:])
    # Surface the two server-side figures: the worker wall and the on-device FPGA
    # execution. The client round-trip wall (wall_s) and upload size (post_bytes)
    # are still recorded in the metrics JSON, just not printed here.
    fwall = f" fog_wall={metrics['fog_wall_ms']}ms" if "fog_wall_ms" in metrics else ""
    fpga  = f" fpga={metrics['fpga_ms']}ms" if "fpga_ms" in metrics else ""
    print(f"[harness] got={got} expected={expected} rel_err={rel} "
          f"(tol={args.tol}){fwall}{fpga} -> {verdict}{tail}")
    if not ok and re.search(r"approximation error is too high", log, re.I):
        print("[harness] hint: OpenFHE rejected the decryption "
              '("approximation error is too high"). This almost always means the '
              "circuit exceeds the multiplicative-depth budget — each "
              "ciphertext×ciphertext multiply spends one level. Raise MULT_DEPTH in "
              "src/dotprod.h, then rebuild (scripts/build_task.sh) and re-run with "
              "--reset.")
    print("\n[harness] ===== summary =====")
    print(f"  {verdict:<5} {params.name:<6} {args.op:<10} N={params.n:<3} "
          f"result={got!s:<14} expected={expected}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
