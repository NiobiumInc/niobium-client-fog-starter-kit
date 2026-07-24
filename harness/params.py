# Copyright 2026 Niobium Microsystems, Inc.
# Licensed under the Apache License, Version 2.0.
"""
params.py — tiny instance registry for the encrypted dot-product workload.

Each size fixes a vector length N and two deterministic vectors a, b. The
cleartext dot-product sum_i a_i*b_i is the ground truth the harness checks the
decrypted result against. Everything is intentionally tiny — the point of the
kit is the Fog integration, not the FHE math.
"""

TOY = 0
SMALL = 1

_NAMES = ["toy", "small"]
_LEN = {TOY: 8, SMALL: 32}   # N must stay <= dotprod.h MAX_N (32)

# Ops the kit demos (must match dotprod.h Op + the harness --op choices). Each
# leaves a scalar in slot 0; the cleartext value below is the PASS ground truth.
OPS = ["add", "mul_const", "dot", "weighted", "activation"]

# Constants baked into the recorded circuit for the constant-using ops. These are
# the single source of truth — the harness passes them to the compute stage AND
# computes the expected value with them, so they can't drift.
K_CONST = 3.0   # mul_const: sum_i (K * a_i)
BIAS = 10.0     # weighted:  sum_i (a_i * b_i) + BIAS

# ACTIVATION: MUST match ACT_REPEAT in src/dotprod.h — the cleartext ground truth
# applies cos() exactly as many times as the compiled circuit does.
ACT_REPEAT = 1


def instance_name(size: int) -> str:
    if size < TOY or size > SMALL:
        return "unknown"
    return _NAMES[size]


class InstanceParams:
    """Per-size vector length + the two deterministic input vectors."""

    def __init__(self, size: int):
        if size < TOY or size > SMALL:
            raise ValueError(f"invalid size {size} (expected {TOY}..{SMALL})")
        self.size = size
        self.n = _LEN[size]

    @property
    def name(self) -> str:
        return instance_name(self.size)

    def vector_a(self):
        # a = [1, 2, ..., N]
        return [float(i + 1) for i in range(self.n)]

    def vector_b(self):
        # b = [N, N-1, ..., 1]
        return [float(self.n - i) for i in range(self.n)]

    def expected(self, op: str) -> float:
        """Cleartext ground truth (slot 0) for the given op — mirrors dotprod.cpp
        Kernel exactly, using the same K_CONST / BIAS the harness passes."""
        a, b = self.vector_a(), self.vector_b()
        if op == "add":
            return float(sum(x + y for x, y in zip(a, b)))
        if op == "mul_const":
            return float(sum(K_CONST * x for x in a))
        if op == "dot":
            return float(sum(x * y for x, y in zip(a, b)))
        if op == "weighted":
            return float(sum(x * y for x, y in zip(a, b)) + BIAS)
        if op == "activation":
            import math
            def act(x):
                for _ in range(ACT_REPEAT):
                    x = math.cos(x)
                return x
            return float(sum(act(x) for x in a))
        raise ValueError(f"unknown op: {op}")
