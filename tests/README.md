# GlobalIVPass regression suite v3

This suite contains exactly 29 LLVM IR cases. `run_regressions.py` refuses to run if the directory is stale or mixed with an older suite.

Key corrections from v2:
- `03_overwrite_before_read.ll` is the affine-domain overwrite-before-read case (`store 10; load; +1; store`).
- the old loop-IV-dependent version is retained separately as `28_loop_iv_dependent_reject.ll`.
- case 22 is correctly classified as supported: preheader initialization makes an early exit before the first body store semantics-preserving.
- case 22 now has an executable wrapper and participates in differential execution.

Run:

```bash
python3 run_regressions.py \
  --plugin /path/to/GlobalIVPass.so \
  --llvm-root /home/micodiy/bin/llvm-22.1.8 \
  --keep-output
```

Expected preamble:

```text
suite=v4-2026-08-28 cases=29
```


Runner v4 canonicalizes the non-semantic `; ModuleID = ...` line before the second-pass idempotence comparison.
