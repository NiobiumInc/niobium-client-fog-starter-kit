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
    std::string keydir;
    int n = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--keydir" && i + 1 < argc) keydir = argv[++i];
        else if (a == "--n" && i + 1 < argc) n = std::stoi(argv[++i]);
        else if (a == "--help" || a == "-h") {
            std::cerr << "Usage: " << argv[0] << " --keydir <dir> --n <N>\n"; return 0;
        } else { std::cerr << "Unknown arg: " << a << "\n"; return 1; }
    }
    if (keydir.empty() || n <= 0) {
        std::cerr << "Usage: " << argv[0] << " --keydir <dir> --n <N>\n"
                  << "  --n is the vector length these keys serve; the rotation keys are "
                     "sized to it.\n"; return 1;
    }
    if (n > dotprod::MAX_N) {
        std::cerr << "[keygen] error: --n " << n << " exceeds MAX_N (" << dotprod::MAX_N
                  << "); raise MAX_N in src/dotprod.h and rebuild.\n"; return 1;
    }

    auto cc = dotprod::BuildContext();
    auto kp = cc->KeyGen();
    cc->EvalMultKeyGen(kp.secretKey);

    // Rotation keys for the rotate-and-sum reduction over N slots: the shifts
    // ReduceSum() performs, from the same function it walks, and no others.
    std::vector<int32_t> idx = dotprod::RotationIndices(n);
    cc->EvalRotateKeyGen(kp.secretKey, idx);

    dotprod::SaveContextAndKeys(keydir, cc, kp);
    std::cout << "[keygen] ring=" << cc->GetRingDimension()
              << " multDepth=" << dotprod::MULT_DEPTH
              << " rotations=" << idx.size() << " (n=" << n << ")"
              << " -> " << keydir << "\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "[keygen] error: " << e.what() << "\n";
    return 1;
}
