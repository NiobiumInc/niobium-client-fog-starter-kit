#!/usr/bin/env python3
# Copyright 2026 Niobium Microsystems, Inc.
# Licensed under the Apache License, Version 2.0.
"""
run_submission.py — drive the encrypted dot-product workload end to end.

Single pass: (build) -> keygen -> encrypt -> compute -> decrypt -> verify.

Three run modes, all the same pipeline; the mode says who does the arithmetic:

    (no flag)  the Fog. dp_compute_sdk records the program hollow (structure and
               probes, no polynomial math) and the FPGA computes it. Needs a
               worker, which `fog submit` wires in via NBCC_FHETCH_SERVER.
    --sim      the same hollow record, executed by the bundled FHETCH simulator.
               No account, no device.
    --cpu      real OpenFHE math on this machine, which is what verifies the
               circuit before it ever reaches the Fog.

Compile once, run many: the first run of an (op, N) records the program, later
runs of the same (op, N) execute the cached one, so a single `fog submit` covers
both (a cold start records, then runs). This script never starts a server.
See docs/NIOBIUM_CLIENT_TRANSPORT.md.

    python3 harness/run_submission.py 0 --cpu                   # verify on CPU
    python3 harness/run_submission.py 0 --sim                   # local simulator
    niobium-client/scripts/fog submit \\
        python3 harness/run_submission.py 0 --target FOG --skip-build   # on the Fog
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
    """The only compute stage — dp_compute_sdk. It records the program on a cache
    miss and, in --sim / the Fog mode, executes it through the simulator or the
    worker; --cpu computes with real OpenFHE math here."""
    return "dp_compute_sdk"


def lib_env() -> dict:
    """Runtime env for the stage binaries. Covers the client's OpenFHE +
    libnbfhetch. If a transport server is set (NBCC_FHETCH_SERVER — e.g. by
    `fog submit`), point NBCC_FHETCH_REPLAY at the client forwarder so replay()
    ships over HTTP. We NEVER set NBCC_FHETCH_SERVER/TOKEN ourselves — those flow
    in from `fog submit`. For --sim, point NBCC_FHETCH_SIM at the simulator the
    client build produced, so the run doesn't depend on one being on PATH."""
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
    if not env.get("NBCC_FHETCH_SIM"):
        for sim in (CLIENT / "build" / "vendor" / "niobium-fhetch" / "fhetch_sim",
                    CLIENT / "vendor" / "niobium-fhetch" / "build" / "fhetch_sim"):
            if sim.exists():
                env["NBCC_FHETCH_SIM"] = str(sim)
                break
    return env


STAGES = ("dp_keygen", "dp_encrypt", "dp_decrypt", "dp_compute_sdk")


def rebuild_stages() -> None:
    """Bring build/ up to date with src/ before running anything.

    An edit to the circuit (src/dotprod.cpp, src/dotprod.h) has to reach the
    binaries, and checking only whether build/ exists would run the previous
    kernel and report a confident PASS for code you just replaced. CMake decides
    what actually recompiles, so this costs a moment when nothing changed. With
    no build tree yet, fall back to the full scripts/build_task.sh, which also
    fetches and builds the client.
    """
    if not (BUILD / "CMakeCache.txt").exists():
        print("[harness] building (scripts/build_task.sh) ...")
        subprocess.run(["bash", str(ROOT / "scripts" / "build_task.sh")], check=True)
        return
    cp = subprocess.run(["cmake", "--build", str(BUILD), "-j", "--target", *STAGES],
                        capture_output=True, text=True)
    if cp.returncode != 0:
        print(cp.stdout + cp.stderr)
        raise SystemExit("[harness] build failed — fix the error above, then re-run.")
    if any(w in cp.stdout for w in ("Building", "Linking")):
        print("[harness] rebuilt the stages from src/; the compiled program "
              "recompiles on this run to match")


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
    env = lib_env()

    def run(name, *a, capture=False):
        return subprocess.run([str(BUILD / name), *map(str, a)],
                              check=not capture, capture_output=capture, text=True, env=env)

    # dp_keygen owns the directory name: keys and ciphertexts belong to the
    # parameter set that produced them, so it puts them under a fingerprint of
    # those parameters and prints the path. It reuses an existing set rather than
    # regenerating, and a parameter change simply names a directory that doesn't
    # exist yet. The rotation keys are sized to N.
    kg = run("dp_keygen", "--keybase", io, "--op", args.op, "--n", params.n, capture=True)
    if kg.returncode != 0:
        print(kg.stdout + kg.stderr)
        raise SystemExit("[harness] keygen failed")
    print(kg.stderr.strip(), flush=True)
    keydir = Path(next(l for l in kg.stdout.splitlines()
                       if l.startswith("KEYDIR=")).split("=", 1)[1])
    paramdir = keydir.parent                       # <io>/<fingerprint>/
    querydir = paramdir / "query"                  # inputs are context-bound too
    result = paramdir / f"{args.op}.result.ct"     # result differs per op
    querydir.mkdir(parents=True, exist_ok=True)

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
        # The target is what replay() dispatches on: the Fog for a Fog run, and
        # "local" (the SDK default) for the simulator. Recording is unaffected by
        # it, so all three modes share one cached program.
        target = args.target if args.mode == "fog" else "local"
        compute_args += ["--mode", args.mode, f"--target={target}",
                         "--opt-level", f"O{args.optimization}"]
    # The compute stage runs with its output captured (silent) and, on the Fog, is
    # the slow step (upload + FPGA) — announce it so the prior `[encrypt]` line
    # doesn't look like what's hanging.
    if args.mode == "fog" and os.environ.get("NBCC_FHETCH_SERVER"):
        print("[harness] computing on the Fog — streaming keys + ciphertext to the worker, then "
              "running on the FPGA (the slow part; `fog list` shows progress)...", flush=True)
    elif args.mode == "fog":
        pass                       # no worker: the stage preflight says so, below
    elif args.mode == "sim":
        print("[harness] computing in the local FHETCH simulator...", flush=True)
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
    mode = p.add_mutually_exclusive_group()
    mode.add_argument("--cpu", dest="mode", action="store_const", const="cpu",
                      help="Compute with real OpenFHE math on this machine (verifies the "
                           "circuit; no account, no device).")
    mode.add_argument("--sim", dest="mode", action="store_const", const="sim",
                      help="Execute the compiled program in the local FHETCH simulator "
                           "(the account-free rehearsal of a Fog run).")
    p.set_defaults(mode="fog")
    p.add_argument("--target", default="FOG",
                   help="Fog target for the default mode (default FOG; FUNC_SIM is the "
                        "hardware-free functional simulator). Ignored by --cpu and --sim.")
    p.add_argument("-O", "--optimization", type=int, choices=[0, 1, 2, 3], default=3,
                   help="Compile opt level (default 3 — required for the FPGA target; leave it).")
    p.add_argument("--tol", type=float, default=1e-2,
                   help="PASS window: relative error of the decrypted dot-product (default 1e-2).")
    p.add_argument("--skip-build", action="store_true",
                   help="Don't rebuild the stages first (assume build/ is current). "
                        "`fog submit` passes this, since the build already happened.")
    p.add_argument("--reset", action="store_true",
                   help="Clear this size's keys/inputs and the cached compiled program(s) so "
                        "the workload re-compiles from scratch. Use after editing the circuit "
                        "(src/) or the inputs (params.py).")
    args = p.parse_args()

    if not args.skip_build:
        rebuild_stages()

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
    # Name where the arithmetic happens, which is what the mode selects.
    where = {"fog": "Fog FPGA", "sim": "local simulator",
             "cpu": "local CPU verify"}[args.mode]
    print(f"\n[harness] === {params.name} op={args.op} (N={params.n}, {where}) ===")
    print(f"[harness] expected {args.op} result (cleartext, slot 0) = {expected}")

    metrics, log = run_query(args.size, args, params)

    if metrics.get("needs_device"):
        # The default mode targets the Fog and no worker is wired in: this run was
        # started outside `fog submit`. Show how to submit it, not a FAIL.
        md = ROOT / "measurements" / params.name
        md.mkdir(parents=True, exist_ok=True)
        (md / f"{args.op}.log").write_text(log)
        print(f"\n[harness] the default mode runs '{args.op}' on the Fog, and no worker is "
              "wired in here.")
        print("[harness] Submit it:")
        print(f"[harness]     fog submit python3 harness/run_submission.py {args.size} "
              f"--op {args.op} --target FOG --skip-build")
        print("[harness] Get Fog access -> https://console.niobium.co/request-account")
        print(f"[harness] Or run it locally: --cpu (real math here) or --sim (the simulator)")
        return 0

    timing = fetch_fog_timing()        # best-effort; {} when not on the Fog
    for key in ("fog_wall_ms", "fpga_ms"):
        if timing.get(key) is not None:
            metrics[key] = timing[key]
    got = metrics.get("result")
    rel = abs(got - expected) / max(abs(expected), 1e-9) if got is not None else None
    ok = rel is not None and rel <= args.tol
    verdict = "PASS" if ok else "FAIL"

    metrics.update(size=params.name, n=params.n, op=args.op, mode=args.mode,
                   target=args.target if args.mode == "fog" else "local",
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
