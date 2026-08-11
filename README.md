# Niobium Client + Fog Starter Kit

Run an encrypted workload on the Niobium Fog, [Niobium](https://niobium.co)'s fully homomorphic encryption (FHE) cloud platform. This kit uses the open-source [niobium-client](https://github.com/NiobiumInc/niobium-client) SDK to encrypt two vectors on your machine, compute their dot product on the Fog's FPGA hardware, and decrypt the result back on your machine. The dot product is the gentlest of the kit's circuits; the deepest, `activation`, runs a high-degree Chebyshev polynomial that keeps the FPGA busy — the run most representative of real FHE work. Your data arrives at the Fog encrypted, stays encrypted while it is computed on, and leaves encrypted. Only you hold the key that reads it.

The same pipeline also runs entirely on your CPU, so you can build the kit and verify every step locally while your Fog access is being set up (step 1 below). Running on the Fog itself takes a Niobium account.

If FHE is new to you: fully homomorphic encryption lets a server compute directly on encrypted data. The server never needs the plaintext, and only the holder of the secret key can decrypt the result. The glossary in [FHE terms used in this kit](#fhe-terms-used-in-this-kit) covers the handful of terms the kit relies on.

## What you need

- macOS (Apple Silicon or Intel) or Linux. On Windows, use WSL2.
- git, a C++17 compiler, CMake 3.18+, Python 3, and OpenSSL.

macOS:

```bash
xcode-select --install                  # clang + make (skip if already installed)
brew install cmake openssl@3 python3
```

Linux (Debian/Ubuntu):

```bash
sudo apt-get update && sudo apt-get install -y \
    build-essential cmake python3 python3-venv python3-pip libssl-dev zlib1g-dev git
```

The first build compiles the client's bundled OpenFHE once. Expect it to run for many minutes and use a few GB of disk; every build after that is fast.

## Quickstart

### 1. Get Fog access

Running on the Fog takes a Niobium account through the [developer partner program](https://niobium.co/press/niobium-opens-developer-partner-program-for-the-fog-the-first-iaas-purpose-built-for-fully) — **apply at <https://console.niobium.co/request-account>.** Access is granted in batches, so apply early. Once you're approved, `fog login` (step 4) provisions your API key.

You don't need to wait to start: build the kit and run it locally (steps 2–3) while your access is processed.

### 2. Clone and build

```bash
git clone https://github.com/NiobiumInc/niobium-client-fog-starter-kit.git
cd niobium-client-fog-starter-kit

scripts/setup.sh      # checks the toolchain, fetches the niobium-client submodule
                      # and its OpenFHE source, creates a .venv, and finds OpenSSL
                      # on macOS. No sudo, nothing global.

source .venv/bin/activate     # do this in every new shell; on macOS it also
                              # exports OPENSSL_ROOT_DIR so the build gets TLS
scripts/build_task.sh         # the long first build happens here
```

`scripts/setup.sh --build` runs the setup and the build as one command; the venv activation is still how later shells get `fog` on PATH.

**Note:** the first time the client configures, early in the long build, its log prints `fhetch_transport client: TLS enabled (OpenSSL 3.x)`. If you instead see a CMake warning that OpenSSL was not found, stop and fix that first (see [Troubleshooting](#troubleshooting)): the build will finish, but without TLS `fog submit` cannot reach the Fog in step 4.

### 3. Run it locally (verify on CPU)

`--cpu` computes with plain OpenFHE on your machine, so it needs no account and no device. The toy instance packs `a = [1,2,...,8]` and `b = [8,7,...,1]`, so the dot product is 120:

```bash
python3 harness/run_submission.py 0 --op dot --cpu   # 0 selects the toy instance (N=8)
```

```
[harness] === toy op=dot (N=8, local CPU verify) ===
[harness] expected dot result (cleartext, slot 0) = 120.0
[harness] computing on CPU...
[harness] got=120.0 expected=120.0 rel_err=0.0 (tol=0.01) -> PASS
```

`--sim` is the other local mode: it executes the compiled program through the bundled FHETCH simulator, which is the account-free rehearsal of a Fog run. [How it works](#how-it-works) explains the compile/run split.

### 4. Run it on the Fog

A bare `run_submission.py` targets the Fog, which is the harness default. `fog submit` is what runs it there: it provisions a job, waits for a worker, and re-runs the harness with that worker wired into the environment, so the circuit executes on the FPGA:

```bash
source .venv/bin/activate            # if this is a new shell; puts `fog` on PATH

fog login -u you@yourcompany.com     # console email; prompts for password
fog list                             # confirms auth (an empty list is fine)

fog submit python3 harness/run_submission.py 0 --op dot --target FOG --skip-build
```

Pass `--target FOG` explicitly: `fog submit` reads the target from the command itself, and exits with `--target is required` without it.

> **The first run is a cold start.** A `fog submit` of an `(op, N)` you haven't compiled yet compiles the program on your machine and *then* runs it on the FPGA in the same command, so it takes longer than later runs, which reuse the cached program and only execute.

```
[fog] POST https://api.niobium.co/jobs/ {mode:batch, target:FOG}
[fog] assigned <job-id> -> https://<lb>/<worker>/jobs/<job-id>/run
[nbcc_fhetch_replay] POSTing <N> bytes (streamed, ... target=FOG) -> https://.../run
[harness] got=120.00... expected=120.0 rel_err=... (tol=0.01) fog_wall=9544.7ms fpga=4.35ms -> PASS
```

The submit streams your keys and ciphertext to the worker before anything runs, so it is not instant; the wait depends on your connection. Let it finish rather than interrupting it, and watch progress with `fog list` from another shell. The full CLI reference is in [docs/FOG_CLI.md](docs/FOG_CLI.md).

## How it works

The Niobium model is compile once, run many. Compiling captures your OpenFHE computation as a portable `.fhetch` program; it happens on your machine, once per `(op, N)`. Running executes the cached program on a device, and `fog submit` makes that device the Fog's FPGA. A single `fog submit` does both when needed: if the program isn't cached yet it compiles first, then runs it on the FPGA (a cold start), so only the first run of each op pays the compile cost.

Three run modes select who does the arithmetic:

| mode | computes on | needs |
|---|---|---|
| (no flag) | the Fog's FPGA | a Niobium account, via `fog submit` |
| `--sim` | the bundled FHETCH simulator | nothing |
| `--cpu` | plain OpenFHE on your machine | nothing |

`--cpu` is what checks the math against the cleartext answer before a circuit reaches the Fog. `--sim` executes the same compiled program the FPGA would, so it exercises the compile path as well as the result.

The client and the Fog sit on opposite sides of a trust boundary:

```
  CLIENT (your machine)                       │  FOG (api.niobium.co)
  ─────────────────────                       │  ─────────────────────────
  dp_keygen   keys (the secret key stays)     │
  dp_encrypt  a.ct, b.ct                      │
  dp_compute  compile -> cached program       │
      └─ fog submit ── ciphertext + public keys + program ──►  run on the FPGA
                                              │      (no secret key here,
  dp_decrypt  ◄── encrypted result ───────────┼────── so it cannot decrypt)
```

The stage that runs server-side, [src/dp_compute_sdk.cpp](src/dp_compute_sdk.cpp), loads the crypto context, the evaluation keys, and the ciphertext inputs. It has no code path that reads the secret key: `sk.bin` is written by `dp_keygen` and read only by `dp_decrypt`, and both of those run on your machine. [docs/PRIVACY.md](docs/PRIVACY.md) walks the boundary in detail, including what the Fog can still observe (circuit shape, sizes, timing).

## Try the other ops

The harness selects a circuit with `--op`. Each computes a scalar into slot 0 and checks it against the cleartext answer. The first four are near-instant; `activation` is the compute-heavy one, built so the FHE work itself dominates a run (see [Performance](#performance)):

| `--op` | Computes | What it adds |
|---|---|---|
| `add` | Σ(aᵢ + bᵢ) | slot-wise addition, then the rotate-and-sum reduction |
| `mul_const` | Σ(k·aᵢ) | multiplying a ciphertext by a plaintext scalar |
| `dot` (default) | Σ(aᵢ·bᵢ) | multiplying a ciphertext by a ciphertext |
| `weighted` | Σ(aᵢ·bᵢ) + bias | chaining a multiply with a plaintext addition |
| `activation` | Σ cos(aᵢ) | a degree-8192 Chebyshev polynomial on ciphertext, the deep-circuit workout |

```bash
python3 harness/run_submission.py 0 --op add --cpu   # then mul_const, weighted, activation
```

Each op caches its own compiled program. Instance size `1` runs the same ops on 32-element vectors, and every op runs on the Fog through `fog submit` as in the Quickstart: the first submit of a new op is a cold start, and later runs reuse the cached program.

From here, [docs/EXPERIMENTING.md](docs/EXPERIMENTING.md) walks through editing the inputs, adding a new op end to end, and, once you've run the built-ins, dropping in your own workload. To build the client once and share it across several apps, see [docs/USING_THE_CLIENT.md](docs/USING_THE_CLIENT.md).

## Performance

A Fog run prints two worker-side timings:

```
[harness] got=120.0 ... fog_wall=14075.0ms fpga=83.75ms -> PASS
```

`fog_wall` is the worker's wall time for the job and `fpga` is the on-device execution time it measured; both are fetched after the run from the API's per-job timing and omitted when unavailable (a local run has neither). The client-side round-trip and upload size are recorded in the metrics JSON under `measurements/`, and `fog get <id>` shows the transfer breakdown for a job (`bytes_in`, `upload_seconds`).

For the small ops, a job's end-to-end time is split between the worker and uploading your keys and ciphertext, so it partly reflects your connection. `activation` is the exception: its deep circuit makes the on-device number the one that moves.

How much a job uploads depends on which circuit you run, because the keys are built for the circuit that uses them. The simple `dot` workload, which multiplies two encrypted vectors and adds up the products, requires 27 MB of keys. The more complex `activation` circuit, which applies a smooth nonlinear function (a cosine, approximated by a degree-8192 polynomial) to every encrypted value before adding them up, requires 216 MB. The CKKS parameters live in [src/dotprod.h](src/dotprod.h) and are worth experimenting with (see [docs/EXPERIMENTING.md](docs/EXPERIMENTING.md)), validating any change end to end. Or run from a host with a faster uplink.

## What's in the repo

```
niobium-client-fog-starter-kit/
├─ scripts/
│   ├─ setup.sh             one-time environment setup (toolchain, submodule, .venv)
│   └─ build_task.sh        builds the client SDK and the stage binaries
├─ harness/
│   ├─ run_submission.py    drives keygen -> encrypt -> compute -> decrypt -> verify
│   └─ params.py            instance sizes, input vectors, and expected results
├─ src/                     the example workload (swap it for your own)
│   ├─ dotprod.h / .cpp     CKKS context, serialization, the op kernels
│   ├─ dp_keygen.cpp        client stage: generates keys, writes the secret key
│   ├─ dp_encrypt.cpp       client stage: encrypts the inputs
│   ├─ dp_compute_sdk.cpp   server stage: compiles or runs the circuit, no secret key
│   └─ dp_decrypt.cpp       client stage: decrypts the result
├─ niobium-client/          the SDK, pinned as a git submodule
├─ CMakeLists.txt           builds the stages against the client's OpenFHE
└─ docs/
    ├─ EXPERIMENTING.md             edit the example, add ops, then bring your own workload
    ├─ NIOBIUM_CLIENT_TRANSPORT.md  run modes, the compile/run flow, environment requirements
    ├─ USING_THE_CLIENT.md          vendored vs standalone client, your own repo
    ├─ FOG_CLI.md                   `fog` command reference
    └─ PRIVACY.md                   the trust boundary in detail
```

## FHE terms used in this kit

| Term | Meaning here |
|---|---|
| ciphertext | An encrypted value. One CKKS ciphertext holds a whole vector. The Fog only ever sees these. |
| slot | One position in the packed vector inside a ciphertext. Inputs `a` and `b` are packed one value per slot. |
| CKKS | The FHE scheme used here. Its arithmetic on real numbers is approximate, so results are checked against a small tolerance instead of exact equality. |
| multiplicative depth | How many chained ciphertext-times-ciphertext multiplies the parameters allow, fixed at key generation. Each op's keys are generated at the depth its own circuit needs: one level for the four small ops, fifteen for `activation`. |
| rotation | A cyclic shift of a ciphertext's slots. Rotate-and-sum uses it to turn per-slot products into a single total in slot 0. |
| evaluation keys | Public key material (`mk.bin`, `rk.bin`) that lets the server multiply and rotate ciphertext without being able to decrypt. The secret key never leaves your machine. |

## Troubleshooting

| Symptom | Fix |
|---|---|
| `pip` fails with `externally-managed-environment` | Install into the kit's venv: run `scripts/setup.sh`, then `source .venv/bin/activate`. |
| macOS build fails at `find_package(OpenSSL)`, or no "TLS enabled" line | `brew install openssl@3`, re-run `scripts/setup.sh`, activate the venv, rebuild. |
| A run prints "the default mode runs on the Fog, and no worker is wired in here" | The bare command targets the Fog. Run it under `fog submit`, or use `--cpu` or `--sim` to stay local. |
| `fog login` can't reach the API, or TLS errors | Make sure the venv is active so the CLI finds `certifi`, and that `~/.fog/config`, if you have one, doesn't point `api_url` away from `https://api.niobium.co`. |
| `fog submit` exits with `--target is required` | Pass `--target FOG` inside the submitted command itself. |
| `fog submit` returns HTTP 401 | Token missing or expired; run `fog login` again. |
| `fog submit` returns HTTP 403 | Your account isn't provisioned for that target; contact Niobium. |
| `fog submit` seems to hang | It is uploading keys and ciphertext. `fog list` in another shell shows the job state. |
| Decrypt fails with "approximation error is too high" | The circuit needs more multiplicative depth than its keys were generated for. For an op you edited, refresh its depth (see [docs/EXPERIMENTING.md](docs/EXPERIMENTING.md#depth-and-key-size)). |
| Backend fatal error during compile (address space / allocation) | The compile ran without `-O3`. Keep the harness's default opt level. |

Several of these trace back to environment requirements the kit normally configures for you; those are covered in [docs/NIOBIUM_CLIENT_TRANSPORT.md](docs/NIOBIUM_CLIENT_TRANSPORT.md#what-the-kit-configures-for-you).

## Get in touch

For questions, access requests, or trouble with a job, reach Niobium at <https://niobium.co/contact> or through the console at <https://console.niobium.co>. More about the Fog platform is at <https://niobium.co>. External code contributions aren't being accepted yet while the contribution policy and CLA are finalized; [CONTRIBUTING.md](CONTRIBUTING.md) has the current status.

## License

Apache 2.0, see [LICENSE](LICENSE). That covers the source in this repository. The Fog service and the compiler it runs are provided to Niobium customers under their own terms.
