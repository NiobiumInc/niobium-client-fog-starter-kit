# Experimenting with the kit

Change something, re-run, and watch the result track the change. Every edit below ends in a `PASS` line you can check yourself, locally or on the Fog.

## How re-runs work

The compiled program is cached in a `dotprod_compute_*/` directory, written to the directory you run from (the repo root in every example here). Its name carries what the program was built from: the op, the vector length, the `k` and `bias` constants, and a fingerprint of the compute binary. Change any of those and the next run compiles a new program, so an edit reaches the simulator and the Fog on its own.

The harness brings `build/` up to date with `src/` before each run, so a kernel edit is compiled before it is executed. `--skip-build` runs against the binaries as they stand.

- **`--cpu`** computes the answer on your machine every run.
- **`--sim` and the Fog** execute the cached program, compiling it first when there isn't one. The first submit of a new circuit is a cold start: it compiles and then runs on the Fog in the same command.

`--reset` clears the instance's keys and inputs along with the cached programs, which is how you regenerate keys from scratch.

`scripts/clean.sh` removes the keys, inputs, compiled programs, and metrics. `--build` also removes the build tree. `--all` removes everything untracked: those two plus the venv, the fetched sources, and the client's build. `--dry-run` lists without deleting.

## Before editing anything: try switching ops

The kit ships five circuits, and `--op` selects one. Each caches its own compiled program:

| `--op` | Computes | Description |
|---|---|---|
| `add` | Σ(aᵢ + bᵢ) | slot-wise addition, then the rotate-and-sum reduction |
| `mul_const` | Σ(k·aᵢ) | multiplying a ciphertext by a plaintext scalar |
| `dot` (default) | Σ(aᵢ·bᵢ) | multiplying a ciphertext by a ciphertext |
| `weighted` | Σ(aᵢ·bᵢ) + bias | chaining a multiply with a plaintext addition |
| `activation` | Σ cos(aᵢ) | a degree-8192 Chebyshev polynomial on ciphertext, the deep-circuit workout |

```bash
python3 harness/run_submission.py 0 --op add --cpu        # -> 72
python3 harness/run_submission.py 0 --op mul_const --cpu  # -> 108
```

## Two kinds of edit

The workload is spread across two files. [../src/dotprod.cpp](../src/dotprod.cpp) holds the circuits: the C++ kernels that compute on ciphertext. [../harness/params.py](../harness/params.py) holds the input vectors, plus an `expected()` function that computes the same math on the plain inputs. After every run, the harness decrypts the result and checks it against that cleartext answer.

**Edit the inputs (Python), no rebuild.** In [../harness/params.py](../harness/params.py#L56), change `vector_b` to all ones:

```python
56    def vector_b(self):
57        # b = all ones (was [N, N-1, ..., 1])
58        return [1.0] * self.n
```

Run it:

```bash
python3 harness/run_submission.py 0 --op dot --cpu
```

The expected value moves from 120 to 36 (`a = [1..8]` dotted with ones), and the PASS means the encrypted pipeline (encrypt, compile, run, decrypt) computed the same 36 from your edited inputs. The harness derives the expected value from the file you just changed, never from a hardcoded answer.

**Edit the circuit (C++).** In `Kernel` in [../src/dotprod.cpp](../src/dotprod.cpp#L141), double the dot product. The extra `EvalMult` scales the product ciphertext by the plaintext 2.0 before the reduction:

```cpp
141        case Op::DOT:  // was: ReduceSum(cc, cc->EvalMult(a, b), n)
142            return ReduceSum(cc, cc->EvalMult(cc->EvalMult(a, b), 2.0), n);
```

The circuit's cleartext twin lives in `expected()` in [../harness/params.py](../harness/params.py#L68) and needs the same change, or the harness will check the result against a stale answer:

```python
68        if op == "dot":
69            return float(2 * sum(x * y for x, y in zip(a, b)))
```

The kernels are compiled code, so rebuild, then run:

```bash
scripts/build_task.sh
python3 harness/run_submission.py 0 --op dot --cpu       # expected and result are now 240.0
```

To add a brand-new op instead of bending `dot`, the next section walks one through end to end.

## Add an op end to end

A worked example: **sum of squares**, `Σ aᵢ²`. It introduces one new idea, multiplying a ciphertext by itself, and reuses the same rotate-and-sum reduction. Three edits:

In the snippets below, numbered lines are the files as they are today; lines marked `+` are the ones you add.

**1. The circuit.** In [../src/dotprod.h](../src/dotprod.h#L98), add `SUMSQ` to the `Op` enum:

```cpp
 98    enum class Op {
 99        ADD,        // slot0 = sum_i (a_i + b_i)
100        MUL_CONST,  // slot0 = sum_i (k * a_i)          (ciphertext * plaintext scalar)
101        DOT,        // slot0 = sum_i (a_i * b_i)         (ciphertext * ciphertext)
102        WEIGHTED,   // slot0 = sum_i (a_i * b_i) + bias  (dot + plaintext bias)
103        ACTIVATION, // slot0 = sum_i cos^ACT_REPEAT(a_i) (high-degree Chebyshev — compute-heavy)
  +        SUMSQ,      // slot0 = sum_i (a_i * a_i)
104    };
```

Then handle it in [../src/dotprod.cpp](../src/dotprod.cpp#L105), in `ParseOp`:

```cpp
109        if (s == "weighted")  return Op::WEIGHTED;
110        if (s == "activation") return Op::ACTIVATION;
  +        if (s == "sumsq")     return Op::SUMSQ;
```

in `OpName`:

```cpp
120            case Op::ACTIVATION: return "activation";
  +            case Op::SUMSQ:      return "sumsq";
```

and in `Kernel`, where the new op is one line next to `dot`, multiplying `a` by itself instead of by `b`:

```cpp
141        case Op::DOT:
142            return ReduceSum(cc, cc->EvalMult(a, b), n);           // sum_i (a_i*b_i)
  +        case Op::SUMSQ:
  +            return ReduceSum(cc, cc->EvalMult(a, a), n);           // sum_i (a_i*a_i)
```

**2. The ground truth.** In [../harness/params.py](../harness/params.py#L20), add `"sumsq"` to `OPS`:

```python
20    OPS = ["add", "mul_const", "dot", "weighted", "activation", "sumsq"]
```

and give `expected()` the matching branch, so the harness knows the cleartext answer to check the decrypted result against:

```python
68        if op == "dot":
69            return float(sum(x * y for x, y in zip(a, b)))
  +        if op == "sumsq":
  +            return float(sum(x * x for x in a))
```

**3. Run it.** The harness rebuilds the stages, and the new op compiles on its first run:

```bash
scripts/build_task.sh
python3 harness/run_submission.py 0 --op sumsq --cpu       # a = [1..8]  ->  sum of squares = 204
```

Expect `result=204.0 ... -> PASS`. It runs on the Fog exactly like a built-in op:

```bash
fog submit python3 harness/run_submission.py 0 --op sumsq --target FOG --skip-build
```

## Longer vectors

Both input vectors derive from the length `N` (`a = [1, 2, ..., N]`, `b = [N, N-1, ..., 1]`), and `expected()` recomputes from them, so growing an instance is one edit. The sizes live in the `_LEN` map in [../harness/params.py](../harness/params.py#L16):

```python
16    _LEN = {TOY: 8, SMALL: 32}   # N must stay <= dotprod.h MAX_N (32)
```

Up to 32, change the value and re-run with `--reset`, which regenerates the instance's keys and inputs at the new length. Rotate-and-sum shifts the ciphertext by powers of two below `N`, and `dp_keygen` generates a rotation key for each of those shifts, so a longer vector means one more key per doubling.

Past 32, `dp_keygen` stops with the constant to raise, `MAX_N` in [../src/dotprod.h](../src/dotprod.h#L51):

```cpp
51    inline constexpr int MAX_N = 32;
```

Raise it, then re-run with `--reset`; the harness rebuilds the stages, since `MAX_N` is compiled into them.

## Turn up the compute

The `activation` op is the kit's compute lever: it evaluates a degree-`ACT_DEGREE` Chebyshev polynomial on ciphertext, and a degree-D evaluation costs about log₂(D)+1 levels of depth. Its knobs live in [../src/dotprod.h](../src/dotprod.h): raise `ACT_DEGREE`, or chain evaluations with `ACT_REPEAT`, until the on-device time dominates the run.

A deeper polynomial needs a longer modulus chain, so update the op's depth alongside it (next section) and re-run with `--reset` to generate keys at the new depth.

## Depth and key size

Multiplicative depth is how many chained ciphertext-times-ciphertext multiplies the parameters allow, and it sets the length of the modulus chain that every key and ciphertext is expressed over. Each op's keys are generated at the depth that op's circuit consumes, which is why the four small ops share a 25 MB key set at one level while `activation` uses a 142 MB set at fifteen:

```
io/toy/r65536_d1_s40_f60/keys       add, mul_const, dot, weighted
io/toy/r65536_d15_s40_f60/keys      activation
```

The directory name is the parameters the keys were built with, so changing any of them generates a new set rather than colliding with the old one, and ops that need the same parameters share.

`DeclaredDepth()` in [../src/dotprod.cpp](../src/dotprod.cpp) records the depth of each shipped op. An op that isn't listed there is measured instead: `dp_keygen` runs its kernel on dummy data in a throwaway context and reads the level the result reached, which is exact because an FHE circuit cannot branch on values. A new op therefore needs no depth set by hand.

After editing a kernel or a constant that changes how many levels a shipped op spends, refresh its entry:

```bash
build/dp_keygen --print-depth --op activation --n 8
```

Too little depth surfaces at decrypt as OpenFHE's *"approximation error is too high"*, since each ciphertext-times-ciphertext multiply spends one level.

## Bring your own workload

1. Replace `Kernel` in [../src/dotprod.cpp](../src/dotprod.cpp) with your FHE circuit (the `Op` enum and `--op` wiring show how to carry several). Your op's depth is measured for you; rotations wider than `MAX_N` need that constant raised in [../src/dotprod.h](../src/dotprod.h). At `RING_DIM` 65536 a depth of 20 is the safe maximum, and 24 or more fails OpenFHE's 128-bit security check at context creation. Then update the instance registry in [../harness/params.py](../harness/params.py): your op(s) in `OPS`, any constants, and the `expected()` cleartext ground truth the harness checks against.
2. Declare what your circuit uses, in [../src/dotprod.cpp](../src/dotprod.cpp): `RotationIndices()` for the shifts it performs, `NeedsRelinKey()` if it multiplies ciphertext by ciphertext, and `UsesVectorB()` if it reads the second vector. Each of these decides what `dp_keygen` generates and what the server uploads; the defaults for an unlisted op are the conservative ones.
3. Tag your real inputs with `tag_input(...)` in [../src/dp_compute_sdk.cpp](../src/dp_compute_sdk.cpp) (order: context, inputs, keys), and `probe(...)` each output.
4. Keep the harness contract: compile on the first run, and never start your own server; `fog submit` provides it. Everything else (build, submodule, compile flags, TLS) carries over.

The full integration contract for a brand-new repo is in [USING_THE_CLIENT.md](USING_THE_CLIENT.md); the run modes and the compile/run flow are in [NIOBIUM_CLIENT_TRANSPORT.md](NIOBIUM_CLIENT_TRANSPORT.md).

## Share one client across apps

To build the heavy client once and reuse it instead of building the submodule per app: clone niobium-client standalone, `make release` it, and `export NIOBIUM_CLIENT_DIR=/path/to/niobium-client`. The build script, CMake, and the harness all honor it. [USING_THE_CLIENT.md](USING_THE_CLIENT.md) has the full guide.
