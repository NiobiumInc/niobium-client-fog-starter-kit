// Copyright 2026 Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// dotprod.cpp — shared implementation for the encrypted dot-product workload.
// Compiled into every stage binary. When compiled into the SDK compute stage it
// carries -DNIOBIUM_COMPILER -DOPENFHE_CPROBES so Kernel's EvalMult/EvalRotate
// fire the SDK's compile-time probes; otherwise it's plain OpenFHE.

#include "dotprod.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace dotprod {

namespace fs = std::filesystem;

std::string ContextFingerprint(usint depth) {
    return "r" + std::to_string(RING_DIM) + "_d" + std::to_string(depth) +
           "_s" + std::to_string(SCALING_MOD_SIZE) + "_f" + std::to_string(FIRST_MOD_SIZE);
}

CryptoContext<DCRTPoly> BuildContext(usint depth) {
    CCParams<CryptoContextCKKSRNS> params;
    params.SetMultiplicativeDepth(depth);
    params.SetScalingModSize(SCALING_MOD_SIZE);
    params.SetFirstModSize(FIRST_MOD_SIZE);
    params.SetSecurityLevel(HEStd_128_classic);
    // Pin the ring dimension (see dotprod.h). At MULT_DEPTH=20 the 128-bit
    // security check needs a ring this large (20 is the safe max here).
    params.SetRingDim(RING_DIM);

    auto cc = GenCryptoContext(params);
    cc->Enable(PKE);
    cc->Enable(KEYSWITCH);
    cc->Enable(LEVELEDSHE);
    cc->Enable(ADVANCEDSHE);   // EvalChebyshevFunction (ACTIVATION op) needs ADVANCEDSHE
    return cc;
}

usint DeclaredDepth(Op op) {
    switch (op) {
        case Op::ADD:        return 1;
        case Op::MUL_CONST:  return 1;
        case Op::DOT:        return 1;
        case Op::WEIGHTED:   return 1;
        case Op::ACTIVATION: return 15;
    }
    return 0;
}

usint RequiredDepth(Op op, int n) {
    if (usint declared = DeclaredDepth(op)) return declared;
    return MeasureDepth(op, n);
}

usint MeasureDepth(Op op, int n) {
    // A counting context, not a protecting one: ring 2^10 so the probe is ~64x
    // cheaper than the real thing, no security level because nothing here is
    // secret, and MULT_DEPTH as the ceiling to measure against. The dummy inputs
    // are constants and the keys are discarded when this returns.
    CCParams<CryptoContextCKKSRNS> params;
    params.SetMultiplicativeDepth(MULT_DEPTH);
    params.SetScalingModSize(SCALING_MOD_SIZE);
    params.SetFirstModSize(FIRST_MOD_SIZE);
    params.SetSecurityLevel(HEStd_NotSet);
    params.SetRingDim(1 << 10);

    auto cc = GenCryptoContext(params);
    cc->Enable(PKE); cc->Enable(KEYSWITCH); cc->Enable(LEVELEDSHE); cc->Enable(ADVANCEDSHE);

    auto kp = cc->KeyGen();
    cc->EvalMultKeyGen(kp.secretKey);
    cc->EvalRotateKeyGen(kp.secretKey, RotationIndices(n));

    const std::vector<double> dummy(n, 1.0);
    auto pt = cc->MakeCKKSPackedPlaintext(dummy);
    auto a  = cc->Encrypt(kp.publicKey, pt);
    auto b  = cc->Encrypt(kp.publicKey, pt);

    auto r = Kernel(cc, a, b, n, op, 1.0, 0.0);
    // GetLevel() counts the rescales the circuit performed. A result still
    // carrying a squared scale needs one more level to rescale into, so add it.
    usint used = static_cast<usint>(r->GetLevel()) + (r->GetNoiseScaleDeg() > 1 ? 1 : 0);
    return used > 0 ? used : 1;                    // OpenFHE wants at least one
}

void SaveContextAndKeys(const fs::path& keydir,
                        const CryptoContext<DCRTPoly>& cc,
                        const KeyPair<DCRTPoly>& kp) {
    fs::create_directories(keydir);
    if (!Serial::SerializeToFile((keydir / "cc.bin").string(), cc, SerType::BINARY))
        throw std::runtime_error("failed to write cc.bin");
    if (!Serial::SerializeToFile((keydir / "pk.bin").string(), kp.publicKey, SerType::BINARY))
        throw std::runtime_error("failed to write pk.bin");
    if (!Serial::SerializeToFile((keydir / "sk.bin").string(), kp.secretKey, SerType::BINARY))
        throw std::runtime_error("failed to write sk.bin");

    std::ofstream mkfs((keydir / "mk.bin").string(), std::ios::binary);
    if (!cc->SerializeEvalMultKey(mkfs, SerType::BINARY))
        throw std::runtime_error("failed to write mk.bin");

    std::ofstream rkfs((keydir / "rk.bin").string(), std::ios::binary);
    if (!cc->SerializeEvalAutomorphismKey(rkfs, SerType::BINARY))
        throw std::runtime_error("failed to write rk.bin");
}

CryptoContext<DCRTPoly> LoadContext(const fs::path& keydir,
                                    bool withRelinKey, bool withRotationKeys) {
    CryptoContext<DCRTPoly> cc;
    if (!Serial::DeserializeFromFile((keydir / "cc.bin").string(), cc, SerType::BINARY))
        throw std::runtime_error("cannot load cc.bin from " + keydir.string());

    if (withRelinKey) {
        std::ifstream mkfs((keydir / "mk.bin").string(), std::ios::binary);
        if (!mkfs.good() || !cc->DeserializeEvalMultKey(mkfs, SerType::BINARY))
            throw std::runtime_error("cannot load mk.bin");
    }

    if (withRotationKeys) {
        std::ifstream rkfs((keydir / "rk.bin").string(), std::ios::binary);
        if (!rkfs.good() || !cc->DeserializeEvalAutomorphismKey(rkfs, SerType::BINARY))
            throw std::runtime_error("cannot load rk.bin");
    }
    return cc;
}

PublicKey<DCRTPoly> LoadPublicKey(const fs::path& keydir,
                                  const CryptoContext<DCRTPoly>& /*cc*/) {
    PublicKey<DCRTPoly> pk;
    if (!Serial::DeserializeFromFile((keydir / "pk.bin").string(), pk, SerType::BINARY))
        throw std::runtime_error("cannot load pk.bin from " + keydir.string());
    return pk;
}

PrivateKey<DCRTPoly> LoadSecretKey(const fs::path& keydir,
                                   const CryptoContext<DCRTPoly>& /*cc*/) {
    PrivateKey<DCRTPoly> sk;
    if (!Serial::DeserializeFromFile((keydir / "sk.bin").string(), sk, SerType::BINARY))
        throw std::runtime_error("cannot load sk.bin from " + keydir.string());
    return sk;
}

Ciphertext<DCRTPoly> EncryptVector(const CryptoContext<DCRTPoly>& cc,
                                   const PublicKey<DCRTPoly>& pk,
                                   const std::vector<double>& v) {
    auto pt = cc->MakeCKKSPackedPlaintext(v);   // zero-pads to ring/2 slots
    return cc->Encrypt(pk, pt);
}

double DecryptSlot0(const CryptoContext<DCRTPoly>& cc,
                    const PrivateKey<DCRTPoly>& sk,
                    const Ciphertext<DCRTPoly>& ct) {
    Plaintext pt;
    cc->Decrypt(sk, ct, &pt);
    pt->SetLength(1);
    return pt->GetCKKSPackedValue()[0].real();
}

Op ParseOp(const std::string& s) {
    if (s == "add")       return Op::ADD;
    if (s == "mul_const") return Op::MUL_CONST;
    if (s == "dot")       return Op::DOT;
    if (s == "weighted")  return Op::WEIGHTED;
    if (s == "activation") return Op::ACTIVATION;
    throw std::runtime_error("unknown op: '" + s + "' (add|mul_const|dot|weighted|activation)");
}

const char* OpName(Op op) {
    switch (op) {
        case Op::ADD:       return "add";
        case Op::MUL_CONST: return "mul_const";
        case Op::DOT:       return "dot";
        case Op::WEIGHTED:  return "weighted";
        case Op::ACTIVATION: return "activation";
    }
    return "?";
}

std::vector<int32_t> RotationIndices(int n) {
    std::vector<int32_t> idx;
    for (int32_t r = 1; r < n; r <<= 1)         // the rotate-and-sum shifts
        idx.push_back(r);
    return idx;
}

bool NeedsRelinKey(Op op) {
    switch (op) {
        case Op::ADD:                           // EvalAdd(ct, ct)
        case Op::MUL_CONST:  return false;      // EvalMult(ct, plaintext scalar)
        case Op::DOT:                           // EvalMult(ct, ct)
        case Op::WEIGHTED:                      // EvalMult(ct, ct) + bias
        case Op::ACTIVATION: return true;       // Chebyshev: many EvalMult(ct, ct)
    }
    return true;                                // unknown op: assume it multiplies
}

static Ciphertext<DCRTPoly> ReduceSum(const CryptoContext<DCRTPoly>& cc,
                                      Ciphertext<DCRTPoly> acc, int n) {
    for (int32_t r : RotationIndices(n))        // rotate-and-sum into slot 0
        acc = cc->EvalAdd(acc, cc->EvalRotate(acc, r));
    return acc;
}

Ciphertext<DCRTPoly> Kernel(const CryptoContext<DCRTPoly>& cc,
                            const Ciphertext<DCRTPoly>& a,
                            const Ciphertext<DCRTPoly>& b,
                            int n, Op op, double k, double bias) {
    switch (op) {
        case Op::ADD:
            return ReduceSum(cc, cc->EvalAdd(a, b), n);            // sum_i (a_i+b_i)
        case Op::MUL_CONST:
            return ReduceSum(cc, cc->EvalMult(a, k), n);           // sum_i (k*a_i)
        case Op::DOT:
            return ReduceSum(cc, cc->EvalMult(a, b), n);           // sum_i (a_i*b_i)
        case Op::WEIGHTED: {
            auto s = ReduceSum(cc, cc->EvalMult(a, b), n);         // sum_i (a_i*b_i)
            return cc->EvalAdd(s, bias);                           // + bias
        }
        case Op::ACTIVATION: {
            // Element-wise nonlinear activation via a high-degree Chebyshev series —
            // a deep, compute-bound circuit. Input stays one ciphertext. (b is unused.)
            auto g  = [](double x) { return std::cos(x); };        // R -> [-1,1], stable to iterate
            auto ct = cc->EvalChebyshevFunction(g, a, ACT_LO, ACT_HI, ACT_DEGREE);
            for (uint32_t r = 1; r < ACT_REPEAT; ++r)
                ct = cc->EvalChebyshevFunction(g, ct, -1.0, 1.0, ACT_DEGREE);  // cos-output domain
            return ReduceSum(cc, ct, n);                           // slot0 = sum_i g^ACT_REPEAT(a_i)
        }
    }
    throw std::runtime_error("unhandled op");
}

std::vector<double> ParseVector(const std::string& csv) {
    std::vector<double> out;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty())
            out.push_back(std::stod(tok));
    }
    if (out.empty())
        throw std::runtime_error("empty vector: '" + csv + "'");
    return out;
}

}  // namespace dotprod
