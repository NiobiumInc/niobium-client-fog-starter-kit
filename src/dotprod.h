// Copyright 2026 Niobium Microsystems, Inc.
// Licensed under the Apache License, Version 2.0.
//
// dotprod.h — the tiny example workload: an encrypted dot-product.
//
// Two length-N vectors a, b are each packed into one CKKS ciphertext. The
// server computes  sum_i a_i*b_i  homomorphically (one ct*ct EvalMult, then a
// rotate-and-sum reduction into slot 0) without ever seeing the plaintext.
//
// The point of the kit is the Fog integration, not the FHE math — so the circuits
// are deliberately small (the dot-product itself is depth 1: a single
// ciphertext*ciphertext multiply). The one exception is the ACTIVATION op,
// which is compute-heavy on purpose.
//
// Shared by every stage binary (keygen / encrypt / compute / decrypt). Only the
// compute stage is Niobium-instrumented; the kernel itself is plain OpenFHE.
#pragma once

#include "openfhe.h"

// OpenFHE serialization headers (needed by every stage that reads/writes
// CryptoContext, keys, or ciphertexts to disk).
#include "ciphertext-ser.h"
#include "cryptocontext-ser.h"
#include "key/key-ser.h"
#include "scheme/ckksrns/ckksrns-ser.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace lbcrypto;

namespace dotprod {

// ── CKKS parameters ───────────────────────────────────────────────────────
// RING_DIM is pinned to 65536. At MULT_DEPTH=20 the 128-bit security check
// needs a ring this large (see docs/EXPERIMENTING.md). Lowering MULT_DEPTH
// (the first four ops fit in 2) would let OpenFHE use a smaller ring, which
// shrinks the upload — experiment with the CKKS parameters and validate any
// change end to end.
inline constexpr usint RING_DIM        = 65536;
inline constexpr usint MULT_DEPTH      = 20;  // deep enough for the activation op; 20 is the safe
                                              // max at RING_DIM=65536 (24+ trips OpenFHE's 128-bit
                                              // security check).
inline constexpr usint SCALING_MOD_SIZE = 50;
inline constexpr usint FIRST_MOD_SIZE   = 60;

// Largest vector length the generated rotation keys support (rotate-and-sum uses
// powers of two < N). Toy uses N=8, small uses N=32; both fit under 32.
inline constexpr int MAX_N = 32;

// ── ACTIVATION op knobs (compile-time; edit + rebuild + --reset to retune) ──
// The compute lever: a degree-D Chebyshev eval costs ~log2(D)+1 levels, so keep
// ACT_REPEAT*(log2(ACT_DEGREE)+1) <= MULT_DEPTH. Raise DEGREE/REPEAT until compute
// dominates wall time.
inline constexpr uint32_t ACT_DEGREE = 8192;  // ~14 levels; the compute knob
inline constexpr uint32_t ACT_REPEAT = 1;     // chained evals (one big eval > many small)
inline constexpr double   ACT_LO     = 0.0;   // Chebyshev domain: must cover a_i in [1, MAX_N]
inline constexpr double   ACT_HI     = 33.0;  // MAX_N=32 < 33

// Build the CKKS CryptoContext used by every stage (identical params so keys,
// ciphertexts, and the compiled program all line up).
CryptoContext<DCRTPoly> BuildContext();

// Persist / load the crypto context + key material. keydir layout:
//   cc.bin  public context      pk.bin  public key
//   sk.bin  secret key (CLIENT-ONLY)     mk.bin  eval-mult (relin) key
//   rk.bin  eval rotation keys
void SaveContextAndKeys(const std::filesystem::path& keydir,
                        const CryptoContext<DCRTPoly>& cc,
                        const KeyPair<DCRTPoly>& kp);

// Load cc + eval-mult + rotation keys INTO the returned context (so tag_keys()
// and EvalMult/EvalRotate work). Does not touch the secret key.
CryptoContext<DCRTPoly> LoadContextWithEvalKeys(const std::filesystem::path& keydir);

PublicKey<DCRTPoly>  LoadPublicKey(const std::filesystem::path& keydir,
                                   const CryptoContext<DCRTPoly>& cc);
PrivateKey<DCRTPoly> LoadSecretKey(const std::filesystem::path& keydir,
                                   const CryptoContext<DCRTPoly>& cc);

// Encrypt a plaintext vector into one packed CKKS ciphertext (zero-padded).
Ciphertext<DCRTPoly> EncryptVector(const CryptoContext<DCRTPoly>& cc,
                                   const PublicKey<DCRTPoly>& pk,
                                   const std::vector<double>& v);

// Decrypt and return slot 0 (where DotKernel leaves the scalar result).
double DecryptSlot0(const CryptoContext<DCRTPoly>& cc,
                    const PrivateKey<DCRTPoly>& sk,
                    const Ciphertext<DCRTPoly>& ct);

// ── The example operations ────────────────────────────────────────────────
// A handful of circuits, selected by --op, each leaving a scalar in slot 0 via
// a rotate-and-sum reduction. The first four are trivial on purpose (DOT/
// WEIGHTED = one ct*ct; MUL_CONST = one ct*plaintext); ACTIVATION burns most of
// the MULT_DEPTH=20 budget on a high-degree Chebyshev eval (knobs above).
enum class Op {
    ADD,        // slot0 = sum_i (a_i + b_i)
    MUL_CONST,  // slot0 = sum_i (k * a_i)          (ciphertext * plaintext scalar)
    DOT,        // slot0 = sum_i (a_i * b_i)         (ciphertext * ciphertext)
    WEIGHTED,   // slot0 = sum_i (a_i * b_i) + bias  (dot + plaintext bias)
    ACTIVATION, // slot0 = sum_i cos^ACT_REPEAT(a_i) (high-degree Chebyshev — compute-heavy)
};

Op   ParseOp(const std::string& s);           // throws on unknown
const char* OpName(Op op);

// Run the selected circuit. k is used only by MUL_CONST, bias only by WEIGHTED.
// Pure OpenFHE — the SDK's probes fire on these calls when the TU is built
// with OPENFHE_CPROBES (that's what the compile step captures).
Ciphertext<DCRTPoly> Kernel(const CryptoContext<DCRTPoly>& cc,
                            const Ciphertext<DCRTPoly>& a,
                            const Ciphertext<DCRTPoly>& b,
                            int n, Op op, double k, double bias);

// Parse a comma-separated list of doubles ("1,2,3.5").
std::vector<double> ParseVector(const std::string& csv);

}  // namespace dotprod
