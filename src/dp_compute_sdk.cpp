// Copyright 2026 Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// dp_compute_sdk — Stage 3 (SERVER), SDK / FHETCH-transport variant.
//
// This is the SERVER role, and it works across the trust boundary: it loads the
// crypto context + evaluation keys + ciphertext inputs, and NEVER the secret key
// (that stays on the client). It therefore cannot decrypt anything.
//
// Linked against the niobium-client SDK's libnbfhetch. Compile-once/run-many:
// the first run of an (op, N) records the program as a .fhetch, and runs of the
// same (op, N) execute that cached program. One invocation does both when needed.
//
// Three run modes, chosen with --mode (default fog). The mode decides who does
// the arithmetic, and the recording follows from that:
//   - fog   hollow record, then replay() over the transport to the Fog worker
//           wired in by `scripts/fog submit` (NBCC_FHETCH_SERVER). The FPGA does
//           the math and result() reconstructs the values here.
//   - sim   hollow record, then replay() through the bundled FHETCH simulator
//           (fhetch_sim). Same path as fog, no account and no device.
//   - cpu   real-math OpenFHE on this machine. The kernel itself produces the
//           result, so the math is verified with no device and no account.
//
// Hollow recording skips the polynomial math while preserving structure and
// firing probes, so the two modes that hand execution to something else generate
// the trace and nothing more. `fog submit` never pays for math the FPGA redoes.
//
//   dp_compute_sdk --keydir <dir> --query-dir <dir> --result <result.ct> --n <N>
//                  [--mode cpu|sim|fog] [--target FOG] [--opt-level O3]

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

// Distinct exit code for "--mode fog, but no Fog worker is wired in". Not an
// error: the harness turns it into a "submit it to the Fog" nudge rather than a
// failure.
static constexpr int EXIT_NEEDS_DEVICE = 3;

static void usage(const char* p) {
    std::cerr << "Usage: " << p
              << " --keydir <dir> --query-dir <dir> --result <result.ct> --n <N>\n"
                 "                       [--op add|mul_const|dot|weighted|activation] [--k <double>] [--bias <double>]\n"
                 "                       [--mode cpu|sim|fog] [--target FOG] [--opt-level O3]\n";
}

int main(int argc, char* argv[]) try {
    // init() consumes --target and the opt level (--opt-level O3 / -O3) and
    // forwards them to the run backend; the target also selects the local
    // compile vs. transport-run dispatch.
    niobium::compiler().init(argc, argv);
    niobium::compiler().set_program_info(
        "dotprod_compute", "1.0", "Encrypted dot-product (SDK / FHETCH transport)");
    niobium::compiler().set_build_info(__FILE__, __LINE__, __TIMESTAMP__);

    std::string keydir, queryDir, resultPath, opStr = "dot", modeStr = "fog";
    int n = 0;
    double kConst = 1.0, bias = 0.0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--keydir"    && i + 1 < argc) keydir     = argv[++i];
        else if (a == "--query-dir" && i + 1 < argc) queryDir   = argv[++i];
        else if (a == "--result"    && i + 1 < argc) resultPath = argv[++i];
        else if (a == "--n"         && i + 1 < argc) n          = std::stoi(argv[++i]);
        else if (a == "--op"        && i + 1 < argc) opStr      = argv[++i];
        else if (a == "--mode"      && i + 1 < argc) modeStr    = argv[++i];
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
    if (modeStr != "cpu" && modeStr != "sim" && modeStr != "fog") {
        std::cerr << "Unknown --mode '" << modeStr << "' (cpu|sim|fog)\n";
        usage(argv[0]); return 1;
    }
    // cpu computes here; sim and fog hand the computation to an executor, so they
    // record hollow and let that executor produce the values.
    const bool cpuMode = (modeStr == "cpu");
    const bool fogMode = (modeStr == "fog");
    const bool hollow  = !cpuMode;

    // Preflight before any key or ciphertext work: a fog run with no worker has
    // nothing to run on, and the message is more useful than a failure 30s later.
    if (fogMode && std::getenv("NBCC_FHETCH_SERVER") == nullptr) {
        std::cerr << "[compute-sdk] --mode fog needs a Fog worker. Submit it with "
                     "`fog submit` (which wires NBCC_FHETCH_SERVER), or run it "
                     "locally with --mode cpu or --mode sim.\n";
        return EXIT_NEEDS_DEVICE;
    }
    dotprod::Op op = dotprod::ParseOp(opStr);

    // Load the rotation keys the reduction needs, and the relinearization key
    // only for the ops that multiply ciphertext by ciphertext. tag_keys() ships
    // what the context holds, so `add` and `mul_const` never upload mk.bin.
    const bool needRelin = dotprod::NeedsRelinKey(op);
    auto cc = dotprod::LoadContext(keydir, needRelin, /*withRotationKeys=*/true);

    // Load the inputs this circuit reads. mul_const multiplies by a plaintext
    // scalar and activation transforms a alone, so b stays on disk for them.
    const bool usesB = dotprod::UsesVectorB(op);
    Ciphertext<DCRTPoly> a, b;
    auto qd = std::filesystem::path(queryDir);
    if (!Serial::DeserializeFromFile((qd / "a.ct").string(), a, SerType::BINARY))
        throw std::runtime_error("cannot load a.ct from " + queryDir);
    if (usesB && !Serial::DeserializeFromFile((qd / "b.ct").string(), b, SerType::BINARY))
        throw std::runtime_error("cannot load b.ct from " + queryDir);

    std::cout << "[compute-sdk] ring=" << cc->GetRingDimension()
              << " n=" << n << " op=" << dotprod::OpName(op) << "\n";

    // Cache key: both the op and the vector length change the circuit, so they
    // belong in the cache parameters. Same (op, N) => same compiled program =>
    // a run (cache hit). (k/bias are baked into the compiled program; changing
    // them needs a re-compile — the harness keeps them fixed.)
    niobium::Compiler::CacheParameters cacheParams = {
        {"op", dotprod::OpName(op)}, {"n", std::to_string(n)},
        // k and bias are baked into the compiled program, so they belong in the
        // cache key too. Without them, editing K_CONST or BIAS in params.py runs
        // the program built with the old value and reports a mismatch against the
        // new expectation, with nothing pointing at the stale program.
        {"k", std::to_string(kConst)}, {"bias", std::to_string(bias)}};
    // The circuit itself lives in this binary, so a rebuilt binary means a
    // possibly different circuit: fingerprint it, and editing src/ recompiles the
    // program instead of running the one recorded from the previous kernel.
    try {
        const long long stamp = static_cast<long long>(
            std::filesystem::last_write_time(argv[0]).time_since_epoch().count());
        cacheParams.push_back({"build", std::to_string(stamp)});
    } catch (const std::exception&) {
        // argv[0] not resolvable (unusual): fall through with the parameters above.
    }
    niobium::compiler().cache_parameters(cacheParams);

    // Tag order: context, then inputs, then keys.
    niobium::compiler().capture_crypto_context(cc);
    niobium::compiler().tag_input("a", a);
    if (usesB) niobium::compiler().tag_input("b", b);   // untagged means not uploaded
    niobium::compiler().tag_keys(cc);

    // Gate the *record* on the program cache: the first run of an (op, N) writes
    // the .fhetch, later runs of the same (op, N) reuse it.
    const bool cacheValid = niobium::compiler().is_cache_valid();

    Ciphertext<DCRTPoly> result;

    // The circuit, then Compress: the scalar in slot 0 needs one tower to decrypt,
    // and dropping the rest is what the result ciphertext travels as. Compress is
    // part of the circuit, so it is inside the recording and the executor performs
    // it too — the value that comes back is already small.
    auto compute = [&] {
        auto r = dotprod::Kernel(cc, a, b, n, op, kConst, bias);
        return cc->Compress(r, dotprod::RESULT_TOWERS);
    };

    if (!cacheValid) {
        std::cout << "[compute-sdk] compiling (first run of this op/N"
                  << (hollow ? ", hollow record" : "") << ")...\n";
        niobium::compiler().enable_hollow_mode(hollow);
        niobium::compiler().start();
        result = compute();
        niobium::compiler().probe("result", result);
        niobium::compiler().stop();
        niobium::compiler().enable_hollow_mode(false);
        std::cout << "[compute-sdk] compile done (cached program ready)\n";
    } else if (cpuMode) {
        // The program is cached, and cpu mode computes here by definition. Run
        // the kernel outside the recorder, so re-verifying needs no --reset.
        result = compute();
    }

    // sim and fog execute the cached program; result() reads back the values the
    // executor computed, overwriting the hollow placeholder from the record pass.
    if (!cpuMode) {
        std::cout << "[compute-sdk] running on "
                  << (fogMode ? "the Fog" : "the local simulator") << "...\n";
        if (!niobium::compiler().replay()) {   // SDK API: runs the compiled program
            std::cerr << "[compute-sdk] run failed\n"; return 1;
        }
        if (!niobium::compiler().result(cc, "result", result)) {
            std::cerr << "[compute-sdk] result() failed\n"; return 1;
        }
        std::cout << "[compute-sdk] run done\n";
    }

    if (!Serial::SerializeToFile(resultPath, result, SerType::BINARY))
        throw std::runtime_error("failed to serialize " + resultPath);
    std::cout << "[compute-sdk] result -> " << resultPath << "\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "[compute-sdk] error: " << e.what() << "\n";
    return 1;
}
