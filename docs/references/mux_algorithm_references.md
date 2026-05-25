# Mux Algorithm References

This document preserves the historical and mathematical background behind the
hashing and indexing techniques used in `src/net/http/mux.c`.  The ideas were
carried over into the router implementation, but the **production source code**
has been refactored to use standard systems-programming terminology so that
future maintainers can focus on control logic rather than cultural context.

---

## 1. Lo Shu Magic Square (Naak-seo / 洛書)

**Concept**
A 3×3 magic square documented in ancient Chinese mathematics and later studied
extensively by the Joseon scholar **Choi Seok-jeong (崔錫鼎, 1646–1715)** in his
work *Gusuryak* (九數略).  In a magic square all rows, columns, and diagonals
sum to the same constant (15 for Lo Shu).  Choi expanded this into larger
orders and explored their combinatorial properties.

**Application in mux.c**
The square was used as a deterministic lookup table to map a route signature
to a small integer coordinate.  The equal row/column sums were intended to keep
bucket-pressure uniform as route counts grow.

**Current code name**
- `CWIST_ROUTING_HASH_SLOT` (was `CWIST_LO_SHU`)
- `cwist_routing_hash_slot()` (was `cwist_magic_square_coord()`)

---

## 2. Orthogonal Latin-Square Permutation

**Concept**
Latin squares and their orthogonal pairs are combinatorial designs studied in
classical Korean mathematics by **Choi Seok-jeong** in *Gusuryak* (九數略) and
later examined by **Hong Jeong-ha (洪正夏)** in *Gu-iljip* (九一集).  Two Latin
squares are orthogonal when every ordered pair of symbols occurs exactly once.
Choi’s work on these structures predates the formal European treatment of
orthogonal Latin squares by more than a century.

**Application in mux.c**
Two 4×4 orthogonal tables (`MUX_PERM_TABLE_A` / `MUX_PERM_TABLE_B`) were used
to fold a route byte and its segment index into a compact 4-bit nibble.  This
nibble perturbs the 128-bit route signature during segment mixing.

**Current code name**
- `MUX_PERM_TABLE_A` (was `NAM_LS_PRIMARY`)
- `MUX_PERM_TABLE_B` (was `NAM_LS_SECONDARY`)
- `mux_byte_permute()` (was `nam_latin_merge()`)

---

## 3. Coefficient Matrix & Linear System Solver

**Concept**
*Jeungseung Gaebangbeop* (乘除開方法) is a Joseon-era technique for scaling and
normalizing ratios.  The matrix used here is a 3×3 fixed-coefficient matrix
that produces a solvable linear system for three unknown weights.

**Application in mux.c**
The matrix (`MUX_COEFF_MATRIX`) and a Gaussian-elimination solver
(`mux_solve_linear3()`) derive three weights from the high and low 64-bit
halves of the route signature.  Those weights are then blended into the final
bucket index.

**Current code name**
- `MUX_COEFF_MATRIX` (was `JOSEON_RATIO_MATRIX`)
- `mux_solve_linear3()` (was `cwist_dawonsul_solve3()`)
- `mux_derive_coeffs()` (was `cwist_jungseung_coeffs()`)

---

## 4. Iterative Magnitude Refinement

**Concept**
al-Kāshī (15th-century astronomer and mathematician) popularised iterative
root-finding methods.  The specific step used here is a two-iteration
Newton-Raphson-style square-root refinement.

**Application in mux.c**
Refines an intermediate magnitude guess before it is fed into the final mixing
step.  Improves bit diffusion across different bucket sizes.

**Current code name**
- `mux_refine_magnitude()` (was `cwist_al_kashi_refine()`)

---

## 5. Single-Step Linear Extrapolation

**Concept**
Euler’s method for ordinary differential equations predicts the next state by
adding the current slope multiplied by a small step size.

**Application in mux.c**
Predicts the next hash-mix state from the refined magnitude and a slope derived
from signature byte differences.  Keeps the bucket index stable under small
path perturbations.

**Current code name**
- `mux_predict_state()` (was `cwist_euler_predict()`)

---

## Naming Rationale

While the mathematical ideas above are valid for hash-distribution design,
embedding historical person names, era names, or cultural concepts directly into
production identifiers creates unnecessary cognitive load for later engineers.
The refactored names describe **what the code does** (routing hash slot, byte
permutation, linear solver, magnitude refinement, state prediction) rather than
**where the idea came from**.

For further reading on the original mathematics, consult the respective fields
of combinatorics, numerical analysis, and linear algebra literature.
