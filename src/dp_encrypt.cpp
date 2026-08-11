// Copyright 2026 Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// dp_encrypt — Stage 2 (CLIENT). Encrypt the two input vectors with the public
// key into a.ct and b.ct. Plaintext values arrive as comma-separated lists.
//
//   dp_encrypt --keydir <dir> --a "1,2,3,4,5,6,7,8" --b "8,7,6,5,4,3,2,1" --out <dir>

#include "dotprod.h"

#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) try {
    std::string keydir, aCsv, bCsv, outdir;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--keydir" && i + 1 < argc) keydir = argv[++i];
        else if (a == "--a"      && i + 1 < argc) aCsv   = argv[++i];
        else if (a == "--b"      && i + 1 < argc) bCsv   = argv[++i];
        else if (a == "--out"    && i + 1 < argc) outdir = argv[++i];
        else if (a == "--help" || a == "-h") {
            std::cerr << "Usage: " << argv[0]
                      << " --keydir <dir> --a <csv> --b <csv> --out <dir>\n"; return 0;
        } else { std::cerr << "Unknown arg: " << a << "\n"; return 1; }
    }
    if (keydir.empty() || aCsv.empty() || bCsv.empty() || outdir.empty()) {
        std::cerr << "Usage: " << argv[0]
                  << " --keydir <dir> --a <csv> --b <csv> --out <dir>\n"; return 1;
    }

    // encrypt uses pk only, so it loads no evaluation keys (348 MB unread).
    auto cc = dotprod::LoadContext(keydir);
    auto pk = dotprod::LoadPublicKey(keydir, cc);
    auto a  = dotprod::ParseVector(aCsv);
    auto b  = dotprod::ParseVector(bCsv);
    if (a.size() != b.size())
        throw std::runtime_error("a and b must have equal length");

    std::filesystem::create_directories(outdir);
    auto ctA = dotprod::EncryptVector(cc, pk, a);
    auto ctB = dotprod::EncryptVector(cc, pk, b);
    if (!Serial::SerializeToFile((std::filesystem::path(outdir) / "a.ct").string(), ctA, SerType::BINARY) ||
        !Serial::SerializeToFile((std::filesystem::path(outdir) / "b.ct").string(), ctB, SerType::BINARY))
        throw std::runtime_error("failed to serialize ciphertexts");

    std::cout << "[encrypt] n=" << a.size() << " -> " << outdir << "/{a,b}.ct\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << "[encrypt] error: " << e.what() << "\n";
    return 1;
}
