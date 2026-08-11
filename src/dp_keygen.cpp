// Copyright 2026 Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// dp_keygen — Stage 1 (CLIENT). Generate the CKKS context + keys and write them
// to <keydir>/. The secret key never leaves this machine.
//
// The rotation keys are sized to the vector length: rotate-and-sum over N slots
// shifts by 1, 2, ... < N, so N=8 needs three keys and N=32 needs five. Each key
// is ~87 MB at ring 2^16 and depth 20, and every one of them is uploaded on a Fog
// run, so keys the circuit never uses are the largest thing on the wire.
//
//   dp_keygen --keydir <dir> --n <N>

#include "dotprod.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) try {
    std::string keybase, opStr = "dot";
    int n = 0;
    bool printDepth = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--keybase" && i + 1 < argc) keybase = argv[++i];
        else if (a == "--n" && i + 1 < argc) n = std::stoi(argv[++i]);
        else if (a == "--op" && i + 1 < argc) opStr = argv[++i];
        else if (a == "--print-depth") printDepth = true;
        else if (a == "--help" || a == "-h") {
            std::cerr << "Usage: " << argv[0] << " --keybase <dir> --op <op> --n <N>\n"
                      << "       " << argv[0] << " --print-depth --op <op> --n <N>\n"; return 0;
        } else { std::cerr << "Unknown arg: " << a << "\n"; return 1; }
    }

    // Measure and report, for filling in DeclaredDepth() after a kernel changes.
    if (printDepth) {
        if (n <= 0) { std::cerr << "--print-depth needs --n <N>\n"; return 1; }
        std::cout << dotprod::MeasureDepth(dotprod::ParseOp(opStr), n) << "\n";
        return 0;
    }
    if (keybase.empty() || n <= 0) {
        std::cerr << "Usage: " << argv[0] << " --keybase <dir> --n <N>\n"
                  << "  --n is the vector length these keys serve; the rotation keys are "
                     "sized to it.\n"
                  << "  Keys land in <keybase>/<parameter fingerprint>/keys, and that path "
                     "is printed as KEYDIR=.\n"; return 1;
    }

    // One directory per parameter set. The path is printed for the harness to
    // read, so nothing has to reconstruct the naming rule in two languages.
    // Depth this op's circuit needs: the recorded value for a shipped op, a
    // measurement for anything else. Key size scales with the modulus chain, so
    // the shallow ops stop paying for levels the deep one needs.
    const dotprod::Op op = dotprod::ParseOp(opStr);
    const usint depth = dotprod::RequiredDepth(op, n);
    const std::filesystem::path keydir =
        std::filesystem::path(keybase) / dotprod::ContextFingerprint(depth) / "keys";
    if (std::filesystem::exists(keydir / "cc.bin")) {
        std::cout << "KEYDIR=" << keydir.string() << "\n";
        std::cerr << "[keygen] reusing keys for " << dotprod::ContextFingerprint(depth) << "\n";
        return 0;
    }
    if (n > dotprod::MAX_N) {
        std::cerr << "[keygen] error: --n " << n << " exceeds MAX_N (" << dotprod::MAX_N
                  << "); raise MAX_N in src/dotprod.h and rebuild.\n"; return 1;
    }

    auto cc = dotprod::BuildContext(depth);
    auto kp = cc->KeyGen();
    cc->EvalMultKeyGen(kp.secretKey);

    // Rotation keys for the rotate-and-sum reduction over N slots: the shifts
    // ReduceSum() performs, from the same function it walks, and no others.
    std::vector<int32_t> idx = dotprod::RotationIndices(n);
    cc->EvalRotateKeyGen(kp.secretKey, idx);

    dotprod::SaveContextAndKeys(keydir, cc, kp);
    std::cout << "KEYDIR=" << keydir.string() << "\n";
    std::cerr << "[keygen] ring=" << cc->GetRingDimension()
              << " multDepth=" << depth
              << " rotations=" << idx.size() << " (n=" << n << ")"
              << " -> " << keydir.string() << "\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "[keygen] error: " << e.what() << "\n";
    return 1;
}
