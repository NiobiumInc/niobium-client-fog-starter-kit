# Using the niobium-client: vendored, standalone, or in your own repo

This kit ships the client as a pinned submodule because that gives a customer a one-shot clone and a reproducible build. The client is also a normal repo, and you can point the kit (or any app) at a standalone clone you build once and share across projects.

> Building and validating locally needs no account. Running on the Fog requires a Niobium account, as in the [README](../README.md).

---

## 1. Vendored submodule (the default)

What `scripts/build_task.sh` does out of the box:

```bash
git submodule update --init niobium-client          # lands the pinned commit
git -C niobium-client submodule update --init --recursive
scripts/build_task.sh                               # builds the client + this app
```

The gitlink is pinned to a fog-capable client commit (one with `scripts/fog`) and committed, so the submodule update can't silently revert it (`git submodule status` shows the exact pin). This is what a customer following the README gets.

Pros: reproducible (the exact client version travels with the app); one clone gets everything. Cons: each app builds its own OpenFHE (multi-GB, slow the first time); the pin is only as fresh as you keep it.

---

## 2. Standalone client, shared across apps

Build the client once, then point every app at it with `NIOBIUM_CLIENT_DIR`. The kit's `build_task.sh`, `CMakeLists.txt`, and `harness/run_submission.py` all honor that variable (the default is the vendored submodule).

```bash
# once, anywhere:
git clone --recursive https://github.com/NiobiumInc/niobium-client.git ~/niobium-client
export OPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3   # macOS only (Intel: /usr/local/opt/openssl@3)
make -C ~/niobium-client release          # builds its OpenFHE + libnbfhetch + transport + scripts/fog

# then, per app / per shell:
export NIOBIUM_CLIENT_DIR=~/niobium-client
scripts/build_task.sh                     # skips the submodule; builds this app against ~/niobium-client
python3 harness/run_submission.py 0 --op dot --cpu                     # verify on CPU
"$NIOBIUM_CLIENT_DIR"/scripts/fog submit \
    python3 harness/run_submission.py 0 --op dot --target FOG --skip-build   # run on the Fog
```

> The one-time local setup is the same for both modes (submodule or standalone) and isn't per-app: the toolchain ([What you need](../README.md#what-you-need)) and the API key via `fog login` (see the [README Quickstart, step 4](../README.md#4-run-it-on-the-fog) or [`FOG_CLI.md`](FOG_CLI.md)). Do it once; every app and client mode reuses `~/.fog`.

A fresh clone of `main` has no submodule pin to manage, already ships `scripts/fog`, and forwards `--opt-level`, so there's nothing to bump or commit. The [build and run requirements](NIOBIUM_CLIENT_TRANSPORT.md#what-the-kit-configures-for-you) still apply when you build outside the kit's scripts (macOS OpenSSL, certifi, `-O3`, `OPENFHE_CPROBES`, the Linux link flags).

The one rule: build your app against the same client's OpenFHE. Mixing a system or Homebrew OpenFHE with the client's `libnbfhetch` fails at link time with `undefined reference to lbcrypto::...`. `CMAKE_PREFIX_PATH` must point at `$NIOBIUM_CLIENT_DIR/vendor/lib/openfhe` (`build_task.sh` does this).

Pros: build the heavy client once, reuse it everywhere; easy to iterate on the client itself. Cons: you own version compatibility (keep it fog-capable and recent enough to forward `--opt-level`); no pinned reproducibility.

---

## 3. Pointing fetch-by-similarity-submission at a shared standalone client

The public [`fetch-by-similarity-submission`](https://github.com/NiobiumInc/fetch-by-similarity-submission) repo follows this same client pattern.

> It owns its own build and run instructions, including its own harness flags (size args, opt-level form, and so on). To run it, follow that repo's README; this kit deliberately doesn't copy its commands, which would drift. The shared concepts (compile/run, the trust boundary) are covered once in [`NIOBIUM_CLIENT_TRANSPORT.md`](NIOBIUM_CLIENT_TRANSPORT.md) and [`PRIVACY.md`](PRIVACY.md).

The one cross-cutting bit its README doesn't cover is reusing a shared standalone client instead of building its own vendored submodule. It hardcodes the relative submodule path (`submission/niobium-client`) and doesn't read `NIOBIUM_CLIENT_DIR`, so redirect the path itself, then build and run per its README.

**a) Symlink the submodule path to your clone (non-invasive):**

```bash
cd fetch-by-similarity-submission
git submodule deinit -f submission/niobium-client && rm -rf submission/niobium-client
ln -s ~/niobium-client submission/niobium-client   # its submodule update becomes a no-op
```

**b) Or bump the submodule in place** (stay on a submodule, at a fresher commit):

```bash
cd fetch-by-similarity-submission/submission/niobium-client
git fetch origin main && git checkout origin/main && make sync
cd ../.. && git add submission/niobium-client && git commit -m "bump client"   # commit the gitlink, or builds revert it
```

---

## 4. Wiring the client into your own repo

To make a brand-new app Fog-capable, replicate the contract this kit implements (see [`src/`](../src) and [`CMakeLists.txt`](../CMakeLists.txt)):

1. **Add the client**, either as a submodule (pin and commit the gitlink) or as a standalone clone via `NIOBIUM_CLIENT_DIR`.
2. **Build against the client's OpenFHE and libnbfhetch**: `find_package(OpenFHE)` from `<client>/vendor/lib/openfhe`; `find_library(nbfhetch ...)` from `<client>/build/...`; put `<client>/vendor/niobium-fhetch/include` on the include path (that is where `niobium/compiler.h` lives). On Linux add `-Wl,--no-as-needed -Wl,--disable-new-dtags` (deferred `lbcrypto::` symbols); zlib is optional. Give the binaries an rpath to the client's OpenFHE and nbfhetch lib dirs, or export `LD_LIBRARY_PATH`/`DYLD_LIBRARY_PATH` when you run them. The kit's [`../CMakeLists.txt`](../CMakeLists.txt) is a 90-line working reference for all of this, and the build itself is the client once, then a normal CMake configure pointed at it:

   ```bash
   make -C "$NIOBIUM_CLIENT_DIR" release      # the client + its OpenFHE (long, once)
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_PREFIX_PATH="$NIOBIUM_CLIENT_DIR/vendor/lib/openfhe"
   cmake --build build -j
   ```
3. **Instrument the compute stage**: compile the TU(s) that touch OpenFHE with `-DNIOBIUM_COMPILER -DOPENFHE_CPROBES` (without `OPENFHE_CPROBES`, `EvalMult(ct,ct)` captures zero `mul` instructions at compile time). Then, around the circuit:
   ```cpp
   niobium::compiler().init(argc, argv);            // consumes --target / --opt-level
   niobium::compiler().set_program_info(...); set_build_info(...);
   niobium::compiler().cache_parameters({{"variant", ...}});   // whatever changes the circuit
   niobium::compiler().capture_crypto_context(cc); // order: context -> inputs -> keys
   niobium::compiler().tag_input("x", ctX); ...
   niobium::compiler().tag_keys(cc);
   if (!niobium::compiler().is_cache_valid()) {     // COMPILE (first run)
       niobium::compiler().start();
       auto out = my_circuit(cc, ...);              // plain OpenFHE
       niobium::compiler().probe("out", out);
       niobium::compiler().stop();
   } else {                                         // RUN (later run, cached program)
       niobium::compiler().replay();                // SDK API: runs the compiled program
       niobium::compiler().result(cc, "out", out);
   }
   ```
4. **Keep the harness server-agnostic**: set `NBCC_FHETCH_REPLAY` to the client forwarder only when `NBCC_FHETCH_SERVER` is set, and never start your own server. `scripts/fog submit` provides the server (a Fog worker). See [`../harness/run_submission.py`](../harness/run_submission.py) (`lib_env`).
5. **Compile the program at `-O3`**, a runtime argument on your compute stage's command line (`init(argc, argv)` consumes it), not a build flag:

   ```bash
   build/my_compute --target FOG --opt-level O3 ...   # not a bare -O3: init() ignores that form
   ```

   Mandatory for the FPGA target; this kit's harness passes it by default.

The example in this repo is the minimal reference implementation of this contract; the full transport story is in [`NIOBIUM_CLIENT_TRANSPORT.md`](NIOBIUM_CLIENT_TRANSPORT.md).
