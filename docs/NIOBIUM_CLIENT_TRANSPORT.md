# Compile once, run many: the FHETCH transport

How the dot-product workload is compiled once and run many times: locally for validation, or on the Fog over HTTP. The [`fetch-by-similarity-submission`](https://github.com/NiobiumInc/fetch-by-similarity-submission) repo follows the same client-over-HTTP path.

Terminology: users see **compile** and **run**. Under the hood the SDK API calls these `start()`/`stop()` (capturing a `.fhetch`) and `replay()`. This doc uses the customer words and names the API calls where relevant.

## The compile/run engine: libnbfhetch

`dp_compute_sdk` links the niobium-client SDK's `libnbfhetch`. Its `niobium::compiler()` compiles the computation into a portable `.fhetch` program plus a `fhetch_replay.json` manifest, and the run step (`replay()`) dispatches to `nbcc_fhetch_replay`, which becomes the FHETCH transport when `NBCC_FHETCH_SERVER` is set. (The compiler's other implementation, `libnbcc`, runs in-process and never touches the transport; this kit does not use it.)

Compiling is gated on the program cache; running (`replay()`) executes what was compiled. One `run_submission.py` pass can do both:

- **Compile (on a cache miss).** Capture the circuit as a compiled program. It is cached in a `dotprod_compute_*/` dir written to the working directory (the repo root when you run from there, as the examples do), named for the op, the vector length, the `k` and `bias` constants, and a fingerprint of the compute binary. A run whose circuit or constants differ from that name compiles a new program.
- **Run.** `replay()` executes the compiled program, through the FHETCH simulator locally or over the transport when `NBCC_FHETCH_SERVER` is set (exported by `fog submit`), and rehydrates the result.

A single `fog submit` therefore compiles (if needed) and then runs, in one command: the first run of a circuit is a cold start, later runs skip the compile.

## Three ways to run it

All three use the same binary (`dp_compute_sdk`) and the same OpenFHE (the client's instrumented 1.4.2), and differ in who does the arithmetic. There is no separate plain-OpenFHE build.

| Mode | What happens | Where the arithmetic happens | Needs |
|------|--------------|------------------------------|-------|
| **`--cpu`** | run the circuit through OpenFHE | your machine | just the client built (no account) |
| **`--sim`** | compile if needed, then run | the bundled FHETCH simulator | just the client built (no account) |
| **(no flag)** | compile if needed, then run | a remote Fog worker, via `scripts/fog submit` | a Niobium account |

### Mode 1: `--cpu` (no account)

```bash
scripts/build_task.sh                                    # builds the client's OpenFHE + the stages
python3 harness/run_submission.py toy --op dot --cpu       # -> PASS
```

No server, no network. `dp_compute_sdk` computes the result through the client's OpenFHE and writes it, so `dp_decrypt` confirms `result == expected`.

### Mode 2: `--sim` (no account)

```bash
python3 harness/run_submission.py toy --op dot --sim       # -> PASS
```

The compiled program runs through `fhetch_sim`, the simulator the client build produces, so this exercises the compile path and the program itself rather than the OpenFHE calls. The harness points `NBCC_FHETCH_SIM` at the binary in your client build.

### Mode 3: the Fog (the customer path)

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

What a job uploads is set by the circuit it runs. `dp_keygen` builds each op's context at the depth that op consumes and generates rotation keys for the shifts its reduction performs, and `dp_compute_sdk` loads the relinearization key only for the ops that multiply ciphertext by ciphertext, so a key the circuit cannot use is never in the context to be uploaded. At N=8 a `dot` job moves 27 MB and an `activation` job 216 MB.

Levers beyond that: a faster uplink, or a host closer to the Fog region. `RING_DIM` is pinned at 65536, which is what the Fog runs; the other CKKS parameters are their own exercise (see [EXPERIMENTING.md](EXPERIMENTING.md), and validate end to end).
