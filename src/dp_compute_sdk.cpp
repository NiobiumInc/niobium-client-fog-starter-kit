// Copyright 2026 Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// dp_compute_sdk — Stage 3 (SERVER), SDK / FHETCH-transport variant.
//
// This is the SERVER role, and it works across the trust boundary: it loads the
// crypto context + evaluation keys + ciphertext inputs, and NEVER the secret key
// (that stays on the client). It therefore cannot decrypt anything.
//
// Linked against the niobium-client SDK's libnbfhetch. Compile-once/run-many,
// and — like the reference server_encrypted_compute — one invocation both
// compiles (if needed) and runs on the target:
//   - record is gated on the program cache. The first run of an (op, N) captures
//     the program as a .fhetch; since our record runs the kernel on CPU it also
//     leaves a CPU result, so dp_decrypt can verify the math with no backend.
//   - then, whenever a device is wired in (NBCC_FHETCH_SERVER, set by
//     `scripts/fog submit`), replay() streams the compiled program over HTTP to
//     the Fog worker and runs it there. So `fog submit` compiles+runs in a single
//     command: the first run is a cold start (record + run), later runs skip the
//     record and only replay.
//   - a plain local run (no NBCC_FHETCH_SERVER) stops after the record and uses
//     the CPU result — there is no device to run on.
//
//   dp_compute_sdk --keydir <dir> --query-dir <dir> --result <result.ct> --n <N>
//                  [--target FOG] [--opt-level O3]

#include "dotprod.h"

#ifdef NIOBIUM_COMPILER
#include <niobium/compiler.h>   // resolved from niobium-fhetch/include (SDK)
#else
#error "dp_compute_sdk.cpp must be built with -DNIOBIUM_COMPILER"
#endif

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

// Distinct exit code for "compiled program present, but no device wired in" — a
// plain local re-run. Not an error: the harness turns it into a "run it on the
// Fog" nudge rather than a failure.
static constexpr int EXIT_NEEDS_DEVICE = 3;

static void usage(const char* p) {
    std::cerr << "Usage: " << p
              << " --keydir <dir> --query-dir <dir> --result <result.ct> --n <N>\n"
                 "                       [--op add|mul_const|dot|weighted|activation] [--k <double>] [--bias <double>]\n"
                 "                       [--target FOG] [--opt-level O3]\n";
}

int main(int argc, char* argv[]) try {
    // init() consumes --target and the opt level (--opt-level O3 / -O3) and
    // forwards them to the run backend; the target also selects the local
    // compile vs. transport-run dispatch.
    niobium::compiler().init(argc, argv);
    niobium::compiler().set_program_info(
        "dotprod_compute", "1.0", "Encrypted dot-product (SDK / FHETCH transport)");
    niobium::compiler().set_build_info(__FILE__, __LINE__, __TIMESTAMP__);

    std::string keydir, queryDir, resultPath, opStr = "dot";
    int n = 0;
    double kConst = 1.0, bias = 0.0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--keydir"    && i + 1 < argc) keydir     = argv[++i];
        else if (a == "--query-dir" && i + 1 < argc) queryDir   = argv[++i];
        else if (a == "--result"    && i + 1 < argc) resultPath = argv[++i];
        else if (a == "--n"         && i + 1 < argc) n          = std::stoi(argv[++i]);
        else if (a == "--op"        && i + 1 < argc) opStr      = argv[++i];
        else if (a == "--k"         && i + 1 < argc) kConst     = std::stod(argv[++i]);
        else if (a == "--bias"      && i + 1 < argc) bias       = std::stod(argv[++i]);
        else if (a == "--target"    && i + 1 < argc) { ++i; /* consumed by init() */ }
        else if (a == "--opt-level" && i + 1 < argc) { ++i; /* consumed by init() */ }
        else if (a.size() == 3 && a.substr(0, 2) == "-O") { /* consumed by init() */ }
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else { std::cerr << "Unknown arg: " << a << "\n"; usage(argv[0]); return 1; }
    }
    if (keydir.empty() || queryDir.empty() || resultPath.empty() || n <= 0) {
        usage(argv[0]); return 1;
    }
    dotprod::Op op = dotprod::ParseOp(opStr);

    auto cc = dotprod::LoadContextWithEvalKeys(keydir);

    Ciphertext<DCRTPoly> a, b;
    auto qd = std::filesystem::path(queryDir);
    if (!Serial::DeserializeFromFile((qd / "a.ct").string(), a, SerType::BINARY) ||
        !Serial::DeserializeFromFile((qd / "b.ct").string(), b, SerType::BINARY))
        throw std::runtime_error("cannot load a.ct/b.ct from " + queryDir);

    std::cout << "[compute-sdk] ring=" << cc->GetRingDimension()
              << " n=" << n << " op=" << dotprod::OpName(op) << "\n";

    // Cache key: both the op and the vector length change the circuit, so they
    // belong in the cache parameters. Same (op, N) => same compiled program =>
    // a run (cache hit). (k/bias are baked into the compiled program; changing
    // them needs a re-compile — the harness keeps them fixed.)
    niobium::Compiler::CacheParameters cacheParams = {
        {"op", dotprod::OpName(op)}, {"n", std::to_string(n)}};
    niobium::compiler().cache_parameters(cacheParams);

    // Tag order: context, then inputs, then keys.
    niobium::compiler().capture_crypto_context(cc);
    niobium::compiler().tag_input("a", a);
    niobium::compiler().tag_input("b", b);
    niobium::compiler().tag_keys(cc);

    // Gate the *record* on the program cache, then run on the device whenever one
    // is wired in — the reference server_encrypted_compute lifecycle. A single
    // `fog submit` thus compiles (if needed) and runs in one invocation.
    const bool cacheValid = niobium::compiler().is_cache_valid();
    const bool haveDevice = std::getenv("NBCC_FHETCH_SERVER") != nullptr;

    Ciphertext<DCRTPoly> result;

    // Record on a cache miss. Our record runs the kernel on CPU (probes fire via
    // OPENFHE_CPROBES), so it also leaves a valid CPU result — what dp_decrypt
    // verifies on a plain local run with no device.
    if (!cacheValid) {
        std::cout << "[compute-sdk] compiling (first run)...\n";
        niobium::compiler().start();
        result = dotprod::Kernel(cc, a, b, n, op, kConst, bias);
        niobium::compiler().probe("result", result);
        niobium::compiler().stop();
        std::cout << "[compute-sdk] compile done (cached program ready)\n";
    }

    // Then run on the device when one is wired in (NBCC_FHETCH_SERVER is set by
    // `fog submit`); replay() streams the compiled program to the Fog worker and
    // the device result overwrites the CPU result above.
    if (haveDevice) {
        std::cout << "[compute-sdk] running on device...\n";
        if (!niobium::compiler().replay()) {   // SDK API: runs the compiled program
            std::cerr << "[compute-sdk] run failed\n"; return 1;
        }
        if (!niobium::compiler().result(cc, "result", result)) {
            std::cerr << "[compute-sdk] result() failed\n"; return 1;
        }
        std::cout << "[compute-sdk] run done\n";
    } else if (cacheValid) {
        // Cached program but no device wired in: a plain local re-run has nothing
        // to execute it on (the first local run above is the compile + CPU verify).
        // Not an error — signal the harness so it shows a "run it on the Fog"
        // nudge instead of a failure.
        std::cerr << "[compute-sdk] compiled program present; no device wired in "
                     "(local re-run).\n";
        return EXIT_NEEDS_DEVICE;
    }

    if (!Serial::SerializeToFile(resultPath, result, SerType::BINARY))
        throw std::runtime_error("failed to serialize " + resultPath);
    std::cout << "[compute-sdk] result -> " << resultPath << "\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "[compute-sdk] error: " << e.what() << "\n";
    return 1;
}
