# GlobalIVPass regression suite v4

The regression suite contains exactly 29 LLVM IR cases. `run_regressions.py` checks the case set before running, so a stale or mixed test directory fails immediately instead of silently producing misleading results.

The suite covers both transformations that should succeed and cases that must be rejected conservatively.

## Run

```bash
python3 run_regressions.py \
  --plugin /path/to/GlobalIVPass.so \
  --llvm-root /home/micodiy/bin/llvm-22.1.8 \
  --keep-output
```

The expected preamble is:

```text
suite=v4-2026-08-28 cases=29
```

A fully successful run ends with:

```text
passed=29 failed=0
```

## What the runner checks

For every case, the runner invokes `opt` with `global-iv,verify`. It then performs structural checks on the resulting IR.

The transformed IR is passed through the optimization a second time as well. After removing the non-semantic `; ModuleID = ...` line, the first and second results must match exactly. This makes repeated localization and other non-idempotent transformations visible.

For executable cases, and when `clang` is available, the runner also compiles and executes both the original and transformed modules and compares their observable results.

## Notable cases

- `03_overwrite_before_read.ll` covers an affine overwrite-before-read path (`store 10; load; +1; store`) and must not initialize the local slot unnecessarily.
- `18_addrspace_counterexample.ll` checks that a global in an unsupported address space is rejected.
- `19_underaligned_counterexample.ll` checks that generated global accesses do not strengthen alignment assumptions.
- `20_returns_twice_probe.ll` guards the `returns_twice` corner case.
- `22_early_exit_before_store_supported.ll` verifies that preheader initialization makes an early exit before the first body store safe.
- `23_call_write_before_store_needs_init.ll` checks synchronization around a write-capable call before the first direct store.
- `24_overwrite_before_read_call_no_init.ll` checks that a guaranteed overwrite can still avoid preheader initialization before a read-only call.
- `25_non_linear_square_reject.ll` and `26_cross_global_dependency_reject.ll` exercise unsupported value evolution.
- `28_loop_iv_dependent_reject.ll` keeps the loop-IV-dependent case separate from the affine overwrite test.
- `29_initialized_early_exit_supported.ll` covers the corresponding initialized early-exit path.

`--keep-output` copies the transformed files to `tests/last-output/` for manual inspection. That directory is generated output and should remain untracked.
