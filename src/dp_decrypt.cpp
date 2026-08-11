// Copyright 2026 Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// dp_decrypt — Stage 4 (CLIENT). Decrypt the result ciphertext and print the
// scalar (slot 0). The harness compares it to the cleartext value. This is the
// ONLY stage that reads the secret key (sk.bin) — it never leaves the client,
// so the Fog can't do what this stage does.
//
//   dp_decrypt --keydir <dir> --result <result.ct>

#include "dotprod.h"

#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) try {
    std::string keydir, resultPath;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--keydir" && i + 1 < argc) keydir     = argv[++i];
        else if (a == "--result" && i + 1 < argc) resultPath = argv[++i];
        else if (a == "--help" || a == "-h") {
            std::cerr << "Usage: " << argv[0] << " --keydir <dir> --result <result.ct>\n";
            return 0;
        } else { std::cerr << "Unknown arg: " << a << "\n"; return 1; }
    }
    if (keydir.empty() || resultPath.empty()) {
        std::cerr << "Usage: " << argv[0] << " --keydir <dir> --result <result.ct>\n";
        return 1;
    }

    // decrypt uses sk only, so it loads no evaluation keys (348 MB unread).
    auto cc = dotprod::LoadContext(keydir);
    auto sk = dotprod::LoadSecretKey(keydir, cc);

    Ciphertext<DCRTPoly> result;
    if (!Serial::DeserializeFromFile(resultPath, result, SerType::BINARY))
        throw std::runtime_error("cannot load " + resultPath);

    double val = dotprod::DecryptSlot0(cc, sk, result);
    // Stable, greppable line the harness parses (slot 0 = the op's scalar result).
    std::cout << std::setprecision(10) << "result = " << val << "\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "[decrypt] error: " << e.what() << "\n";
    return 1;
}
