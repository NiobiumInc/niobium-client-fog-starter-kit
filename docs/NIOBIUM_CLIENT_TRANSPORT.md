# Compile once, run many: the FHETCH transport

How the dot-product workload is compiled once and run many times: locally for validation, or on the Fog over HTTP. The [`fetch-by-similarity-submission`](https://github.com/NiobiumInc/fetch-by-similarity-submission) repo follows the same client-over-HTTP path.

Terminology: users see **compile** and **run**. Under the hood the SDK API calls these `start()`/`stop()` (capturing a `.fhetch`) and `replay()`. This doc uses the customer words and names the API calls where relevant.

## The compile/run engine: libnbfhetch

`dp_compute_sdk` links the niobium-client SDK's `libnbfhetch`. Its `niobium::compiler()` compiles the computation into a portable `.fhetch` program plus a `fhetch_replay.json` manifest, and the run step (`replay()`) dispatches to `nbcc_fhetch_replay`, which becomes the FHETCH transport when `NBCC_FHETCH_SERVER` is set. (The compiler's other implementation, `libnbcc`, runs in-process and never touches the transport; this kit does not use it.)

Recording (compile) is gated on the program cache; running (`replay()`) happens whenever a device is wired in — so one `run_submission.py` pass can do both:

- **Compile (on a cache miss).** Run the circuit through OpenFHE on CPU and write the compiled program plus the CPU result ciphertext (so `dp_decrypt` can verify the math with no backend). The compiled program is cached per `(op, N)` in a `dotprod_compute_<op>_n_<N>/` dir written to the working directory (the repo root when you run from there, as the examples do), so re-running the same `(op, N)` reuses it. Editing the kernel source won't recompile it on its own, so after a change re-run with `--reset` (or `rm -rf dotprod_compute_*`).
- **Run (when a device is set).** `NBCC_FHETCH_SERVER` (exported by `fog submit`) sends the compiled program over the transport and rehydrates the result.

A single `fog submit` therefore compiles (if needed) and then runs, in one command — the first run of an `(op, N)` is a cold start (compile + run), later runs skip the compile and only run. A plain local run (no server) stops after the compile with the CPU result.

## Two ways to run it

Both use the same binary (`dp_compute_sdk`) and the same OpenFHE (the client's instrumented 1.4.2); they differ only in whether and where the run happens. There is no separate plain-OpenFHE build. Compiling is always local, so it validates the math on CPU with no backend before any device run.

| Mode | What happens | Where the run happens | Needs |
|------|--------------|-----------------------|-------|
| **1. Local compile + CPU verify** | compile only (no device wired in) | nowhere; the CPU result is written during compile | just the client built (no account) |
| **2. The Fog** | compile if needed, then run | a remote Fog worker, via `scripts/fog submit` | a Niobium account |

### Mode 1: local compile + CPU verify (no account)

```bash
scripts/build_task.sh                                   # builds the client's OpenFHE + the stages
python3 harness/run_submission.py 0 --op dot            # first run: compile + CPU-verify -> PASS
```

No server, no network. On the first run `dp_compute_sdk` computes the result through the client's OpenFHE and writes it, so `dp_decrypt` confirms `result == expected`. (Re-run with `--reset`, or delete the `dotprod_compute_*/` dir, to force a fresh compile; `io/` holds only keys and inputs.)

### Mode 2: the Fog (the customer path)

See the README's [Quickstart, step 4](../README.md#4-run-it-on-the-fog). `scripts/fog submit`:

1. `POST {api_url}/jobs/ {mode, target}` (header `X-Api-Token`), answered with `202` queued.
2. Long-polls `GET /jobs/{id}?wait=...` until a worker is assigned, yielding `{job_id, server_url, token}`.
3. Exports `NBCC_FHETCH_SERVER={server_url}/jobs/{id}/run` + `NBCC_FHETCH_TOKEN`, then `exec`s your harness command. The harness only ever sets `NBCC_FHETCH_REPLAY` (the forwarder), so the assigned worker URL and token flow straight through to the transport client.

`--target FOG` is read twice: fog-api reads it to pick the pinned stable FPGA, and the SDK's `init()` inside `dp_compute_sdk` consumes it to configure the run backend. Whether a device run happens at all depends on `NBCC_FHETCH_SERVER` being set (by `fog submit`); the harness only forwards the flag.

## What the kit configures for you

The kit's setup and build scripts satisfy these requirements out of the box. Be aware of them when you set up your own workload, standalone client, or config independently.

| Requirement | How the kit meets it |
|---|---|
| **TLS in the C++ transport.** On macOS, `find_package(OpenSSL)` misses Homebrew's OpenSSL, and the HTTPS POST fails without it. | `scripts/setup.sh` detects `openssl@3` and wires `OPENSSL_ROOT_DIR` into `.venv/bin/activate`, so activating the venv sets it before you build. Confirm `TLS enabled (OpenSSL 3.x)` in the config log. |
| **A CA bundle for the `fog` CLI.** The CLI needs `certifi` for TLS. | `scripts/setup.sh` installs `certifi` into the kit's `.venv` (activate it so the CLI finds it), which also sidesteps the PEP 668 `externally-managed-environment` pip error. |
| **Compile at `-O3`.** The FPGA target requires it; a lower opt level fails at compile. | The harness passes `--opt-level O3` by default (forwarded end-to-end to the backend's `-O3`). |
| **Probe instrumentation.** A compute TU built without `OPENFHE_CPROBES` captures zero ct*ct multiplies at compile time. | CMake compiles `dp_compute_sdk` (and its copy of `dotprod.cpp`) with `NIOBIUM_COMPILER OPENFHE_CPROBES`. |
| **Linux link flags.** libnbfhetch defers OpenFHE (`lbcrypto::`) symbols to the final link. | The SDK build adds `-Wl,--no-as-needed -Wl,--disable-new-dtags`. |

## Performance: upload-bound

For the small ops, a Fog run is dominated by the HTTPS upload of keys and serialized ciphertexts over a single TCP stream. How long that takes depends on your connection and the payload size, so measure it on your own setup rather than assuming a figure; even the toy job isn't instant. `fog get <id>` reports `bytes_in` and `upload_seconds` per job. The `activation` op is the exception: its deep circuit is built so the on-device compute outweighs the transfer.

The transport itself doesn't return on-device execution time. After a Fog run the harness fetches the worker-side figures from fog-api's per-job timing endpoint (`GET /jobs/<id>/timing`) and prints them as `fog_wall=...ms` (the worker's wall time) and `fpga=...ms` (on-device execution). The client-side round-trip and upload size are recorded in the run's metrics JSON under `measurements/`.

Levers to move less data or move it faster: a lower `MULT_DEPTH` (a shorter modulus chain), a faster uplink, or a host closer to the Fog region. At the shipped depth the 128-bit security level fixes `RING_DIM`, so it isn't a per-job knob; experimenting with the CKKS parameters is its own exercise (see [EXPERIMENTING.md](EXPERIMENTING.md), and validate end to end).
