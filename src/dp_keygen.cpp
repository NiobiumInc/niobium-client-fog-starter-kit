// Copyright 2026 Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// dp_keygen — Stage 1 (CLIENT). Generate the CKKS context + keys and write them
// to <keydir>/. The secret key never leaves this machine.
//
//   dp_keygen --keydir <dir>

#include "dotprod.h"

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char* argv[]) try {
    std::string keydir;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--keydir" && i + 1 < argc) keydir = argv[++i];
        else if (a == "--help" || a == "-h") {
            std::cerr << "Usage: " << argv[0] << " --keydir <dir>\n"; return 0;
        } else { std::cerr << "Unknown arg: " << a << "\n"; return 1; }
    }
    if (keydir.empty()) { std::cerr << "Usage: " << argv[0] << " --keydir <dir>\n"; return 1; }

    auto cc = dotprod::BuildContext();
    auto kp = cc->KeyGen();
    cc->EvalMultKeyGen(kp.secretKey);

    // Rotation keys for the rotate-and-sum reduction: powers of two < MAX_N.
    std::vector<int32_t> idx;
    for (int32_t r = 1; r < dotprod::MAX_N; r <<= 1) idx.push_back(r);
    cc->EvalRotateKeyGen(kp.secretKey, idx);

    dotprod::SaveContextAndKeys(keydir, cc, kp);
    std::cout << "[keygen] ring=" << cc->GetRingDimension()
              << " multDepth=" << dotprod::MULT_DEPTH
              << " rotations=" << idx.size()
              << " -> " << keydir << "\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "[keygen] error: " << e.what() << "\n";
    return 1;
}
