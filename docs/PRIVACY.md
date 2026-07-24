# Why the Fog can't see your data

The Fog uses fully homomorphic encryption (FHE) to compute on your data without ever decrypting it. Your program runs on purpose-built hardware over ciphertext, and the Fog never receives your secret key or any plaintext. It cannot decrypt your inputs, the intermediate values, or the result, because decryption needs a key the client never sends.

## Two roles, one boundary

```
  CLIENT  (your machine, trusted)             │  the Fog  (server, untrusted)
  ───────────────────────────────             │  ────────────────────────────
  • generates keys (holds the SECRET key)     │  • compiles your program
  • encrypts the plaintext inputs             │  • runs it on the FPGA over ciphertext
  • decrypts the returned result              │  • returns an encrypted result
                                              │
  has: secret key, plaintext, clear result    │  has: ciphertext + public keys only
                             trust boundary ──┘        (no secret key, so no decryption)
```

- Client: the stages you run locally. `dp_keygen`, `dp_encrypt`, and `dp_decrypt` handle the secret key, and the compile side of `dp_compute_sdk` runs locally too.
- Fog: where the compiled program runs (`fog submit`). It receives only what it needs to compute on ciphertext.

## Three kinds of key

CKKS, the FHE scheme used here, has three kinds of key material:

- The **secret key** decrypts. It stays on the client and never leaves your machine.
- The **public key** encrypts. `dp_encrypt` uses it, locally.
- The **evaluation keys** (relinearization and rotation, `mk.bin` / `rk.bin`) let a third party multiply and rotate ciphertext without being able to decrypt. They are derived from the secret key but reveal nothing that recovers it or the plaintext, so they are safe to send to the Fog.

## What crosses the wire

| Stays on the client (your machine) | Sent to the Fog |
|---|---|
| **secret key** `sk.bin` | crypto context (public parameters) |
| **plaintext** inputs `a`, `b` | **evaluation keys** `mk.bin`, `rk.bin` |
| the **decrypted result** | **ciphertext** inputs `a.ct`, `b.ct` |
| | the **compiled program** (`.fhetch`) |

After a run, the client's key material, `sk.bin` included, sits in `io/<instance>/keys/` (`toy` or `small`), with the encrypted inputs and the result beside it in the same instance directory. The uploaded payload holds everything sent, and `sk.bin` is not in it.

## Check it yourself

1. **The server stage never opens the secret key.** [`../src/dp_compute_sdk.cpp`](../src/dp_compute_sdk.cpp) calls `LoadContextWithEvalKeys()` (crypto context + `mk.bin` + `rk.bin`) and loads `a.ct` / `b.ct`. It never reads `sk.bin`:
   ```bash
   grep -n "sk.bin\|LoadSecretKey" src/dp_compute_sdk.cpp    # -> no matches
   grep -rn "LoadSecretKey" src/    # -> the shared helper (dotprod.h/.cpp) and its one caller: dp_decrypt.cpp
   ```
2. **Only the client decrypts.** `dp_keygen` writes `sk.bin`; `dp_decrypt` is the only stage that reads it (see [`../src/dp_decrypt.cpp`](../src/dp_decrypt.cpp)). Neither runs on the Fog.
3. **The result comes back encrypted.** The Fog returns a ciphertext. It becomes a number only after `dp_decrypt` applies your secret key, locally.

## What the Fog does see

FHE hides data values. It does not hide the shape of the computation. Because the Fog compiles and runs your program, it can see:

- the operations and their structure (this kit even labels them: `--op dot`);
- sizes and counts: ciphertext dimensions, how many inputs, the payload size;
- timing.

It cannot see any plaintext value: your inputs, anything computed from them, or the result. If the structure of your computation is itself sensitive, that calls for its own design work (padding, fixed-shape circuits).
