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
// RING_DIM is pinned to 65536, which is the ring the Fog runs. MULT_DEPTH is the
// ceiling a circuit may reach: each op's keys are generated at the depth that
// op's own circuit consumes (see DeclaredDepth / MeasureDepth below), and 20 is
// the safe maximum here, since 24 or more trips OpenFHE's 128-bit security check.
inline constexpr usint RING_DIM        = 65536;
inline constexpr usint MULT_DEPTH      = 20;
inline constexpr usint SCALING_MOD_SIZE = 40;
inline constexpr usint FIRST_MOD_SIZE   = 60;

// Hybrid key switching splits the modulus chain into this many digits, and an
// evaluation key carries one column per digit. Fewer digits means smaller keys
// and a larger auxiliary modulus, which OpenFHE checks against the security
// level when it builds the context.
inline constexpr uint32_t KEY_SWITCH_DIGITS = 1;

// Largest vector length dp_keygen will size rotation keys for (rotate-and-sum
// uses powers of two < N). Toy uses N=8, small uses N=32; both fit under 32.
inline constexpr int MAX_N = 32;

// Towers left in the result ciphertext once the circuit is done. Every op leaves
// its scalar in slot 0 carrying whatever is left of the modulus chain, and
// decrypting that value needs only the last tower. cc->Compress() drops the rest,
// shrinking the ciphertext that travels back from the server and the result.ct on
// disk. Raise it if a deeper circuit needs headroom at decrypt.
inline constexpr uint32_t RESULT_TOWERS = 1;

// ── ACTIVATION op knobs (compile-time) ─────────────────────────────────────
// The compute lever: a degree-D Chebyshev eval costs ~log2(D)+1 levels. Raise
// DEGREE/REPEAT until compute dominates wall time, keeping
// ACT_REPEAT*(log2(ACT_DEGREE)+1) within MULT_DEPTH, and refresh the op's entry
// in DeclaredDepth() afterwards (docs/EXPERIMENTING.md, "Depth and key size").
inline constexpr uint32_t ACT_DEGREE = 8192;  // ~14 levels; the compute knob
inline constexpr uint32_t ACT_REPEAT = 1;     // chained evals (one big eval > many small)
inline constexpr double   ACT_LO     = 0.0;   // Chebyshev domain: must cover a_i in [1, MAX_N]
inline constexpr double   ACT_HI     = 33.0;  // MAX_N=32 < 33

// Build the CKKS CryptoContext at a given multiplicative depth. Keygen picks the
// depth and serializes the context into cc.bin; every other stage loads it back,
// so they cannot disagree about parameters.
CryptoContext<DCRTPoly> BuildContext(usint depth = MULT_DEPTH);

// Names the parameter set a key directory holds. Keys and ciphertexts are bound
// to the context that produced them, so each parameter set gets its own
// directory: changing a parameter generates a new set beside the old one instead
// of colliding with it, and a stale set cannot be picked up by mistake.
std::string ContextFingerprint(usint depth);
// The shifts the rotate-and-sum reduction performs over N slots, and the only
// rotations any op does. dp_keygen generates keys for exactly this set, and
// ReduceSum() walks the same list, so key material and circuit cannot drift.
// A new op that rotates differently extends this function, in one place.
std::vector<int32_t> RotationIndices(int n);


// Persist / load the crypto context + key material. keydir layout:
//   cc.bin  public context      pk.bin  public key
//   sk.bin  secret key (CLIENT-ONLY)     mk.bin  eval-mult (relin) key
//   rk.bin  eval rotation keys
void SaveContextAndKeys(const std::filesystem::path& keydir,
                        const CryptoContext<DCRTPoly>& cc,
                        const KeyPair<DCRTPoly>& kp);

// Load cc, plus whichever evaluation keys the caller is about to use, INTO the
// returned context. Loading a key is what puts it in the context, and the server
// stage's tag_keys() uploads exactly what the context holds, so a stage that
// loads nothing extra ships nothing extra: `add` and `mul_const` leave the 87 MB
// mk.bin on disk, and the client stages (encrypt, decrypt) load neither eval key.
// Never touches the secret key.
CryptoContext<DCRTPoly> LoadContext(const std::filesystem::path& keydir,
                                    bool withRelinKey = false,
                                    bool withRotationKeys = false);

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
// WEIGHTED = one ct*ct; MUL_CONST = one ct*plaintext); ACTIVATION spends fifteen
// levels on a high-degree Chebyshev eval (knobs above).
enum class Op {
    ADD,        // slot0 = sum_i (a_i + b_i)
    MUL_CONST,  // slot0 = sum_i (k * a_i)          (ciphertext * plaintext scalar)
    DOT,        // slot0 = sum_i (a_i * b_i)         (ciphertext * ciphertext)
    WEIGHTED,   // slot0 = sum_i (a_i * b_i) + bias  (dot + plaintext bias)
    ACTIVATION, // slot0 = sum_i cos^ACT_REPEAT(a_i) (high-degree Chebyshev — compute-heavy)
};

Op   ParseOp(const std::string& s);           // throws on unknown
const char* OpName(Op op);

// True for the ops that multiply ciphertext by ciphertext and therefore need the
// relinearization key (mk.bin, ~87 MB at these parameters). `add` adds, and
// `mul_const` multiplies by a plaintext scalar, so neither needs one, and the
// server stage does not load it for them.
bool NeedsRelinKey(Op op);

// True for the ops whose circuit reads the second vector. mul_const multiplies a
// by a plaintext scalar and activation transforms a alone, so tagging b for them
// would upload a ciphertext the circuit never touches.
bool UsesVectorB(Op op);

// Depth each shipped op needs, recorded from a measurement so a run of a known
// example does not pay to rediscover it. Returns 0 for an op that isn't listed,
// which is the signal to measure. Editing one of these kernels means the
// recorded value is a claim about code that changed: re-measure with
// `dp_keygen --print-depth --op <op> --n <N>` and update the entry.
usint DeclaredDepth(Op op);

// The depth to build a context at: the declared value when there is one, the
// measured value when there isn't. A new op therefore works without touching a
// depth constant, and the five shipped ones cost nothing to look up.
usint RequiredDepth(Op op, int n);

// Levels this op's circuit consumes, measured rather than declared: the kernel
// runs on dummy data in a throwaway context and reports the level of its result.
// An FHE circuit cannot branch on values, so level use is a property of the code
// and its compile-time constants, and a count taken at a toy ring is exactly the
// count at RING_DIM. Editing a kernel or a constant changes what this returns,
// with no depth constant to keep in sync by hand.
usint MeasureDepth(Op op, int n);

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
