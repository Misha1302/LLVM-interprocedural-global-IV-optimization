#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
CASES = ROOT / "cases"

TRANSFORM = {
    "01_basic_add.ll": ["g"],
    "02_affine_mul_add.ll": ["g"],
    "03_overwrite_before_read.ll": ["g"],
    "04_i8_wrap.ll": ["g"],
    "05_two_globals.ll": ["g", "h"],
    "06_call_read.ll": ["g"],
    "07_call_write.ll": ["g"],
    "08_transitive_call.ll": ["g"],
    "09_call_other_global.ll": ["g"],
    "19_underaligned_counterexample.ll": ["g"],
    "21_two_exit_edges_same_block_supported.ll": ["g"],
    "22_early_exit_before_store_supported.ll": ["g"],
    "23_call_write_before_store_needs_init.ll": ["g"],
    "24_overwrite_before_read_call_no_init.ll": ["g"],
    "27_cancel_then_mul_supported.ll": ["g"],
    "29_initialized_early_exit_supported.ll": ["g"],
}

REJECT = {
    "10_external_call_reject.ll",
    "11_atomic_reject.ll",
    "12_volatile_reject.ll",
    "13_branch_inside_loop_reject.ll",
    "14_two_exit_blocks_reject.ll",
    "15_no_preheader_reject.ll",
    "16_recursive_call_reject.ll",
    "17_intrinsic_assume_probe.ll",
    "18_addrspace_counterexample.ll",
    "25_non_linear_square_reject.ll",
    "26_cross_global_dependency_reject.ll",
    "28_loop_iv_dependent_reject.ll",
}

EXECUTE = {
    "01_basic_add.ll",
    "02_affine_mul_add.ll",
    "03_overwrite_before_read.ll",
    "04_i8_wrap.ll",
    "05_two_globals.ll",
    "06_call_read.ll",
    "07_call_write.ll",
    "08_transitive_call.ll",
    "09_call_other_global.ll",
    "19_underaligned_counterexample.ll",
    "22_early_exit_before_store_supported.ll",
    "23_call_write_before_store_needs_init.ll",
    "24_overwrite_before_read_call_no_init.ll",
    "27_cancel_then_mul_supported.ll",
}


SUITE_VERSION = "v4-2026-08-28"
EXPECTED_CASES = {
    "01_basic_add.ll",
    "02_affine_mul_add.ll",
    "03_overwrite_before_read.ll",
    "04_i8_wrap.ll",
    "05_two_globals.ll",
    "06_call_read.ll",
    "07_call_write.ll",
    "08_transitive_call.ll",
    "09_call_other_global.ll",
    "10_external_call_reject.ll",
    "11_atomic_reject.ll",
    "12_volatile_reject.ll",
    "13_branch_inside_loop_reject.ll",
    "14_two_exit_blocks_reject.ll",
    "15_no_preheader_reject.ll",
    "16_recursive_call_reject.ll",
    "17_intrinsic_assume_probe.ll",
    "18_addrspace_counterexample.ll",
    "19_underaligned_counterexample.ll",
    "20_returns_twice_probe.ll",
    "21_two_exit_edges_same_block_supported.ll",
    "22_early_exit_before_store_supported.ll",
    "23_call_write_before_store_needs_init.ll",
    "24_overwrite_before_read_call_no_init.ll",
    "25_non_linear_square_reject.ll",
    "26_cross_global_dependency_reject.ll",
    "27_cancel_then_mul_supported.ll",
    "28_loop_iv_dependent_reject.ll",
    "29_initialized_early_exit_supported.ll",
}

def run(cmd: list[str], *, timeout: int = 30) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
    )


def find_tool(name: str, llvm_root: Path | None) -> str | None:
    if llvm_root:
        p = llvm_root / "bin" / name
        if p.exists():
            return str(p)
    env_name = name.upper().replace("-", "_")
    if os.environ.get(env_name):
        return os.environ[env_name]
    return shutil.which(name)


def canonicalize_ir_for_comparison(ir: str) -> str:
    # opt rewrites the textual ModuleID from the current input filename.
    # It is not part of the LLVM IR semantics and necessarily differs between
    # the first and second regression-pass outputs.
    lines = ir.splitlines()
    if lines and lines[0].startswith("; ModuleID = "):
        lines = lines[1:]
    return "\n".join(lines).rstrip() + "\n"


def extract_block(ir: str, name: str) -> str:
    match = re.search(
        rf"(?ms)^{re.escape(name)}:\s*(.*?)(?=^[A-Za-z$._][A-Za-z0-9$._-]*:|\Z)",
        ir,
    )
    if not match:
        raise AssertionError(f"block {name!r} not found")
    return match.group(1)


def structural_checks(name: str, ir: str) -> list[str]:
    notes: list[str] = []
    expected_globals = TRANSFORM.get(name, [])

    if expected_globals:
        for g in expected_globals:
            if f"%{g}.local = alloca" not in ir:
                raise AssertionError(f"expected %{g}.local alloca")
    elif name in REJECT:
        if re.search(r"%[-A-Za-z0-9$._]+\.local = alloca", ir):
            raise AssertionError("case should be conservatively rejected")

    if name == "03_overwrite_before_read.ll" and "%g.initial" in ir:
        raise AssertionError("overwrite-before-read must not initialize local slot")

    if name in {"01_basic_add.ll", "02_affine_mul_add.ll", "03_overwrite_before_read.ll", "04_i8_wrap.ll"}:
        loop = extract_block(ir, "loop")
        if "ptr @g" in loop or "ptr addrspace(" in loop and "@g" in loop:
            raise AssertionError("direct @g access remained in simple localized loop")

    if name == "05_two_globals.ll":
        loop = extract_block(ir, "loop")
        if "ptr @g" in loop or "ptr @h" in loop:
            raise AssertionError("direct global access remained in two-global loop")

    if name == "06_call_read.ll":
        if "%g.before.call" not in ir:
            raise AssertionError("read call must flush local value before call")
        if "%g.after.call" in ir:
            raise AssertionError("read-only call must not reload @g after call")

    if name in {"07_call_write.ll", "08_transitive_call.ll"}:
        if "%g.before.call" not in ir or "%g.after.call" not in ir:
            raise AssertionError("write call must flush before and reload after call")

    if name == "09_call_other_global.ll":
        loop = extract_block(ir, "loop")
        if "%g.before.call" in loop or "%g.after.call" in loop:
            raise AssertionError("call touching only @h must not synchronize @g")

    if name == "18_addrspace_counterexample.ll":
        if "%g.local" in ir:
            raise AssertionError("non-alloca address-space global must be rejected")
        notes.append("addrspace counterexample rejected")

    if name == "21_two_exit_edges_same_block_supported.ll":
        if "%g.local" not in ir:
            raise AssertionError("two exit edges to one dedicated exit should be supported")

    if name == "22_early_exit_before_store_supported.ll":
        if "%g.initial" not in ir:
            raise AssertionError("early exit before first body access requires preheader initialization")
        notes.append("initialized local makes the early exit semantics-preserving")

    if name == "23_call_write_before_store_needs_init.ll":
        if "%g.initial" not in ir:
            raise AssertionError("write-capable call before first direct store requires initialization")
        if "%g.before.call" not in ir or "%g.after.call" not in ir:
            raise AssertionError("write-capable call needs flush and reload")

    if name == "24_overwrite_before_read_call_no_init.ll":
        if "%g.initial" in ir:
            raise AssertionError("guaranteed overwrite before read-only call must not initialize")
        if "%g.before.call" not in ir:
            raise AssertionError("read-only call still requires flush")

    if name == "19_underaligned_counterexample.ll":
        for line in ir.splitlines():
            if "@g" not in line:
                continue
            stripped = line.strip()
            if not (stripped.startswith("%") or stripped.startswith("store")):
                continue
            if ("load i32" in stripped or "store i32" in stripped) and "align 1" not in stripped:
                raise AssertionError(f"strengthened @g alignment: {stripped}")
        notes.append("generated @g accesses preserve safe align 1")

    if name == "17_intrinsic_assume_probe.ll":
        notes.append("llvm.assume currently causes conservative rejection (coverage limitation)")

    if name == "20_returns_twice_probe.ll":
        if "%g.local" in ir:
            notes.append("RISK PROBE: returns_twice call with no @g access is currently localized")
        else:
            notes.append("returns_twice call rejected")

    if name == "29_initialized_early_exit_supported.ll":
        if "%g.initial" not in ir:
            raise AssertionError("initialized early-exit case requires preheader initialization")
        notes.append("initialization makes early exit before store safe")

    return notes


def compile_and_run(clang: str, ir: Path, exe: Path) -> tuple[int, str, str]:
    cp = run([clang, "-Wno-override-module", "-x", "ir", str(ir), "-o", str(exe)], timeout=60)
    if cp.returncode != 0:
        raise AssertionError(f"clang failed:\n{cp.stderr}")
    rp = run([str(exe)], timeout=10)
    return rp.returncode, rp.stdout, rp.stderr


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--plugin", required=True, type=Path)
    parser.add_argument("--llvm-root", type=Path)
    parser.add_argument("--keep-output", action="store_true")
    args = parser.parse_args()

    opt = find_tool("opt", args.llvm_root)
    clang = find_tool("clang", args.llvm_root)
    if not opt:
        print("ERROR: opt not found", file=sys.stderr)
        return 2
    if not args.plugin.exists():
        print(f"ERROR: plugin not found: {args.plugin}", file=sys.stderr)
        return 2

    actual_cases = {p.name for p in CASES.glob("*.ll")}
    if actual_cases != EXPECTED_CASES:
        missing = sorted(EXPECTED_CASES - actual_cases)
        extra = sorted(actual_cases - EXPECTED_CASES)
        print(f"ERROR: stale/mixed suite ({SUITE_VERSION})", file=sys.stderr)
        if missing:
            print("  missing: " + ", ".join(missing), file=sys.stderr)
        if extra:
            print("  extra: " + ", ".join(extra), file=sys.stderr)
        return 2

    print(f"suite={SUITE_VERSION} cases={len(actual_cases)}")

    print(run([opt, "--version"]).stdout.splitlines()[0])
    failures: list[tuple[str, str]] = []
    warnings: list[tuple[str, str]] = []
    passes = 0

    out_root_obj = tempfile.TemporaryDirectory(prefix="global-iv-tests-")
    out_root = Path(out_root_obj.name)

    for case in sorted(CASES.glob("*.ll")):
        transformed = out_root / case.name
        cp = run([
            opt,
            f"-load-pass-plugin={args.plugin}",
            "-passes=global-iv,verify",
            "-S",
            str(case),
            "-o",
            str(transformed),
        ], timeout=30)

        if cp.returncode != 0:
            failures.append((case.name, f"opt/verify failed:\n{cp.stderr}"))
            print(f"FAIL {case.name}: opt/verify")
            continue

        ir = transformed.read_text()
        try:
            notes = structural_checks(case.name, ir)

            # A second pass must be a fixed point for this MVP. Besides catching
            # accidental repeated localization, this also verifies the IR again.
            transformed_twice = out_root / f"{case.stem}.twice.ll"
            cp2 = run([
                opt,
                f"-load-pass-plugin={args.plugin}",
                "-passes=global-iv,verify",
                "-S",
                str(transformed),
                "-o",
                str(transformed_twice),
            ], timeout=30)
            if cp2.returncode != 0:
                raise AssertionError(f"second opt/verify failed:\n{cp2.stderr}")

            ir_twice = transformed_twice.read_text()
            canonical_once = canonicalize_ir_for_comparison(ir)
            canonical_twice = canonicalize_ir_for_comparison(ir_twice)
            if canonical_twice != canonical_once:
                import difflib

                diff = "".join(difflib.unified_diff(
                    canonical_once.splitlines(keepends=True),
                    canonical_twice.splitlines(keepends=True),
                    fromfile=f"{case.name}: first pass",
                    tofile=f"{case.name}: second pass",
                    n=3,
                ))
                raise AssertionError(
                    "pass is not idempotent: second run changed canonical IR\n"
                    + diff[:12000]
                )

            if case.name in EXECUTE and clang:
                original_exe = out_root / f"{case.stem}.orig"
                transformed_exe = out_root / f"{case.stem}.new"
                before = compile_and_run(clang, case, original_exe)
                after = compile_and_run(clang, transformed, transformed_exe)
                if before != after:
                    raise AssertionError(
                        "differential mismatch:\n"
                        f"  original={before}\n"
                        f"  transformed={after}"
                    )

            passes += 1
            suffix = f" ({'; '.join(notes)})" if notes else ""
            print(f"PASS {case.name}{suffix}")
            for note in notes:
                if note.startswith("RISK PROBE"):
                    warnings.append((case.name, note))
        except Exception as exc:
            diagnostic_lines = [
                line for line in cp.stderr.splitlines()
                if line.startswith("Loop ") or "supported=" in line or line.strip() == "unsupported"
            ]
            detail = str(exc)
            if diagnostic_lines:
                detail += "\n  pass diagnostics: " + " | ".join(diagnostic_lines[-8:])
            failures.append((case.name, detail))
            print(f"FAIL {case.name}: {detail}")

    print(f"\npassed={passes} failed={len(failures)} warnings={len(warnings)}")
    if warnings:
        print("\nWarnings:")
        for name, msg in warnings:
            print(f"  {name}: {msg}")
    if failures:
        print("\nFailures:")
        for name, msg in failures:
            print(f"  {name}: {msg}")

    if args.keep_output:
        keep = ROOT / "last-output"
        if keep.exists():
            shutil.rmtree(keep)
        shutil.copytree(out_root, keep)
        print(f"kept transformed IR in {keep}")

    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
