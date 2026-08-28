#!/usr/bin/env bash
set -Eeuo pipefail

MIN_LLVM_MAJOR=22
PROJECT_ROOT="$PWD"
BUILD_DIR=""
STYLE_ONLY=0
CHECK_ONLY=0
KEEP_TEMP=0
TARGET_ARGS=()

usage() {
    cat <<'EOF'
Usage:
  ./apply-format.sh [options] [file-or-directory ...]

Options:
  --project-root DIR   Project root (default: current directory).
  --build-dir DIR      Use/generate compile_commands.json in this directory.
  --style-only         Only style: not/and/or, braces and clang-format.
                       Skip semantic const/const& changes.
  --check              Verify only; do not modify files.
  --keep-temp          Keep the temporary CMake directory for debugging.
  -h, --help           Show this help.

Environment overrides:
  CLANG_FORMAT=/path/to/clang-format
  CLANG_TIDY=/path/to/clang-tidy
  LLVM_CONFIG=/path/to/llvm-config
  CMAKE_BIN=/path/to/cmake

Default mode performs semantic const/const& fixes with clang-tidy. If no
compile_commands.json exists, the script generates one automatically with CMake
in a temporary directory. The project itself is not built.
EOF
}

while (($#)); do
    case "$1" in
        --project-root)
            [[ $# -ge 2 ]] || {
                echo "Missing value for --project-root" >&2
                exit 2
            }
            PROJECT_ROOT="$2"
            shift 2
            ;;
        --build-dir)
            [[ $# -ge 2 ]] || {
                echo "Missing value for --build-dir" >&2
                exit 2
            }
            BUILD_DIR="$2"
            shift 2
            ;;
        --style-only)
            STYLE_ONLY=1
            shift
            ;;
        --check)
            CHECK_ONLY=1
            shift
            ;;
        --keep-temp)
            KEEP_TEMP=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            TARGET_ARGS+=("$@")
            break
            ;;
        -*)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            TARGET_ARGS+=("$1")
            shift
            ;;
    esac
done

PROJECT_ROOT="$(cd -- "$PROJECT_ROOT" && pwd)"

RUNTIME_DIR="$(mktemp -d "${TMPDIR:-/tmp}/llvm-cpp-format.XXXXXX")"

cleanup() {
    if [[ "$KEEP_TEMP" -eq 1 ]]; then
        echo "Temporary files kept at: $RUNTIME_DIR"
    else
        rm -rf -- "$RUNTIME_DIR"
    fi
}

trap cleanup EXIT

FORMAT_CONFIG="$RUNTIME_DIR/clang-format.yaml"
TIDY_STYLE_CONFIG="$RUNTIME_DIR/clang-tidy-style.yaml"
TIDY_SEMANTIC_CONFIG="$RUNTIME_DIR/clang-tidy-semantic.yaml"

cat > "$FORMAT_CONFIG" <<'YAML'
BasedOnStyle: LLVM
Language: Cpp
Standard: Latest

ColumnLimit: 120
IndentWidth: 4
ContinuationIndentWidth: 4
BracedInitializerIndentWidth: 4
TabWidth: 4
UseTab: Never

NamespaceIndentation: All
AccessModifierOffset: -4
IndentCaseLabels: true

BreakBeforeBraces: Attach
InsertBraces: true

AllowShortBlocksOnASingleLine: Never
AllowShortFunctionsOnASingleLine: None
AllowShortIfStatementsOnASingleLine: Never
AllowShortLoopsOnASingleLine: false
AllowShortEnumsOnASingleLine: false

DerivePointerAlignment: false
PointerAlignment: Right
ReferenceAlignment: Right
QualifierAlignment: Left

SpacesInAngles: Leave

BreakBeforeBinaryOperators: All
BreakBeforeTernaryOperators: true

AlignAfterOpenBracket: false
BreakAfterOpenBracketFunction: true
BreakBeforeCloseBracketFunction: false

Cpp11BracedListStyle: FunctionCall
BreakAfterOpenBracketBracedList: true
BreakBeforeCloseBracketBracedList: true

BreakAfterReturnType: Automatic
PenaltyReturnTypeOnItsOwnLine: 1000000000
PenaltyExcessCharacter: 10000000

SpaceBeforeRangeBasedForLoopColon: false

IncludeBlocks: Preserve
SortIncludes: Never

ReflowComments: true
InsertNewlineAtEOF: true
YAML

cat > "$TIDY_STYLE_CONFIG" <<'YAML'
Checks: >
  -*,
  readability-operators-representation,
  readability-braces-around-statements
WarningsAsErrors: ''
HeaderFilterRegex: '.*'
FormatStyle: none
CheckOptions:
  readability-operators-representation.BinaryOperators: 'and;or;not'
  readability-operators-representation.OverloadedOperators: 'and;or;not'
  readability-braces-around-statements.ShortStatementLines: '0'
YAML

cat > "$TIDY_SEMANTIC_CONFIG" <<'YAML'
Checks: >
  -*,
  misc-const-correctness,
  performance-unnecessary-value-param,
  performance-unnecessary-copy-initialization,
  performance-for-range-copy
WarningsAsErrors: ''
HeaderFilterRegex: '.*'
FormatStyle: none
CheckOptions:
  misc-const-correctness.AnalyzeValues: 'true'
  misc-const-correctness.AnalyzeReferences: 'true'
  misc-const-correctness.AnalyzePointers: 'true'
  misc-const-correctness.WarnPointersAsValues: 'false'
  misc-const-correctness.WarnPointersAsPointers: 'true'
  misc-const-correctness.TransformValues: 'true'
  misc-const-correctness.TransformReferences: 'true'
  misc-const-correctness.TransformPointersAsValues: 'false'
  misc-const-correctness.TransformPointersAsPointers: 'true'
  performance-unnecessary-value-param.IncludeStyle: 'llvm'
YAML

resolve_tool() {
    local env_name="$1"
    local base="$2"
    local explicit="${!env_name:-}"
    local candidate=""

    if [[ -n "$explicit" ]]; then
        if [[ "$explicit" != */* ]]; then
            candidate="$(command -v -- "$explicit" 2>/dev/null || true)"
        else
            candidate="$explicit"
        fi

        [[ -n "$candidate" && -x "$candidate" ]] || {
            echo "$env_name is not executable: $explicit" >&2
            return 1
        }

        printf '%s\n' "$candidate"
        return 0
    fi

    for candidate in "${base}-${MIN_LLVM_MAJOR}" "$base"; do
        if command -v -- "$candidate" >/dev/null 2>&1; then
            command -v -- "$candidate"
            return 0
        fi
    done

    local matches=()

    shopt -s nullglob
    matches=("$HOME"/bin/llvm-22*/bin/"$base")
    shopt -u nullglob

    if ((${#matches[@]})); then
        candidate="$(printf '%s\n' "${matches[@]}" | sort -V | tail -n 1)"

        if [[ -x "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    fi

    return 1
}

version_info() {
    local tool="$1"
    local output
    local major
    local summary

    output="$("$tool" --version 2>&1)" || {
        echo "Cannot run: $tool --version" >&2
        return 1
    }

    major="$(sed -nE 's/.*[Vv]ersion[[:space:]]+([0-9]+)(\.[0-9]+)*.*/\1/p' <<<"$output" | head -n 1)"

    if [[ -z "$major" ]]; then
        major="$(grep -Eo '[0-9]+\.[0-9]+(\.[0-9]+)?' <<<"$output" | head -n 1 | cut -d. -f1 || true)"
    fi

    if [[ -z "$major" || ! "$major" =~ ^[0-9]+$ ]]; then
        echo "Could not determine LLVM major version from:" >&2
        printf '%s\n' "$output" >&2
        return 1
    fi

    if ((major < MIN_LLVM_MAJOR)); then
        echo "Need LLVM/Clang >= $MIN_LLVM_MAJOR, got:" >&2
        printf '%s\n' "$output" >&2
        return 1
    fi

    summary="$(grep -m1 -E '[Vv]ersion[[:space:]]+[0-9]+' <<<"$output" || true)"

    if [[ -z "$summary" ]]; then
        summary="$(head -n 1 <<<"$output")"
    fi

    summary="$(sed -E 's/^[[:space:]]+//' <<<"$summary")"
    printf '%s\n' "$summary"
}

CLANG_FORMAT_BIN="$(resolve_tool CLANG_FORMAT clang-format)" || {
    echo "clang-format $MIN_LLVM_MAJOR+ not found." >&2
    echo "Set CLANG_FORMAT=/path/to/clang-format." >&2
    exit 2
}

FORMAT_VERSION="$(version_info "$CLANG_FORMAT_BIN")" || exit 2
echo "Using clang-format: $FORMAT_VERSION"

CLANG_TIDY_BIN=""

if [[ "$STYLE_ONLY" -eq 0 ]]; then
    CLANG_TIDY_BIN="$(resolve_tool CLANG_TIDY clang-tidy)" || {
        echo "clang-tidy $MIN_LLVM_MAJOR+ is required for const/const& fixes." >&2
        echo "Set CLANG_TIDY=/path/to/clang-tidy or use --style-only." >&2
        exit 2
    }

    TIDY_VERSION="$(version_info "$CLANG_TIDY_BIN")" || exit 2
    echo "Using clang-tidy:   $TIDY_VERSION"
fi

LLVM_CONFIG_BIN="$(resolve_tool LLVM_CONFIG llvm-config || true)"

if [[ -n "$LLVM_CONFIG_BIN" ]]; then
    LLVM_CONFIG_VERSION="$(version_info "$LLVM_CONFIG_BIN")" || exit 2
    echo "Using llvm-config:  $LLVM_CONFIG_VERSION"
fi

CMAKE="${CMAKE_BIN:-}"

if [[ -n "$CMAKE" ]]; then
    if [[ "$CMAKE" != */* ]]; then
        CMAKE="$(command -v -- "$CMAKE" 2>/dev/null || true)"
    fi
else
    CMAKE="$(command -v cmake 2>/dev/null || true)"
fi

if ! "$CLANG_FORMAT_BIN" \
    --style="file:$FORMAT_CONFIG" \
    --dump-config \
    >/dev/null \
    2>"$RUNTIME_DIR/format-config.err"; then

    echo "clang-format rejected the generated style configuration:" >&2
    cat "$RUNTIME_DIR/format-config.err" >&2
    exit 2
fi

if [[ -n "$CLANG_TIDY_BIN" ]]; then
    if ! "$CLANG_TIDY_BIN" \
        --config-file="$TIDY_STYLE_CONFIG" \
        --list-checks \
        >/dev/null \
        2>"$RUNTIME_DIR/tidy-style.err"; then

        echo "clang-tidy rejected the style checks configuration:" >&2
        cat "$RUNTIME_DIR/tidy-style.err" >&2
        exit 2
    fi

    if ! "$CLANG_TIDY_BIN" \
        --config-file="$TIDY_SEMANTIC_CONFIG" \
        --list-checks \
        >/dev/null \
        2>"$RUNTIME_DIR/tidy-semantic.err"; then

        echo "clang-tidy rejected the const/const& configuration:" >&2
        cat "$RUNTIME_DIR/tidy-semantic.err" >&2
        exit 2
    fi
fi

is_cpp_file() {
    case "$1" in
        *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx|*.inc|*.ipp|*.tpp)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

is_translation_unit() {
    case "$1" in
        *.c|*.cc|*.cpp|*.cxx)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

collect_from_dir() {
    local dir="$1"

    find "$dir" \
        \( -type d \( \
            -name .git \
            -o -name build \
            -o -name 'build-*' \
            -o -name 'cmake-build-*' \
            -o -name _deps \
            -o -name third_party \
            -o -name vendor \
            -o -name external \
            -o -path '*/tests/last-output' \
        \) -prune \) -o \
        \( -type f \( \
            -name '*.c' \
            -o -name '*.cc' \
            -o -name '*.cpp' \
            -o -name '*.cxx' \
            -o -name '*.h' \
            -o -name '*.hh' \
            -o -name '*.hpp' \
            -o -name '*.hxx' \
            -o -name '*.inc' \
            -o -name '*.ipp' \
            -o -name '*.tpp' \
        \) -print0 \)
}

FILES=()

if ((${#TARGET_ARGS[@]} == 0)); then
    if git -C "$PROJECT_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        while IFS= read -r -d '' rel; do
            path="$PROJECT_ROOT/$rel"

            if [[ -f "$path" ]] && is_cpp_file "$path"; then
                FILES+=("$path")
            fi
        done < <(git -C "$PROJECT_ROOT" ls-files -z)
    else
        while IFS= read -r -d '' path; do
            FILES+=("$path")
        done < <(collect_from_dir "$PROJECT_ROOT")
    fi
else
    for target in "${TARGET_ARGS[@]}"; do
        [[ "$target" = /* ]] || target="$PROJECT_ROOT/$target"

        if [[ -d "$target" ]]; then
            while IFS= read -r -d '' path; do
                FILES+=("$path")
            done < <(collect_from_dir "$target")
        elif [[ -f "$target" ]] && is_cpp_file "$target"; then
            FILES+=("$target")
        else
            echo "Skipping non-C/C++ or missing target: $target" >&2
        fi
    done
fi

if ((${#FILES[@]} == 0)); then
    echo "No C/C++ files found."
    exit 0
fi

mapfile -d '' FILES < <(printf '%s\0' "${FILES[@]}" | sort -zu)

TIDY_FILES=()

for file in "${FILES[@]}"; do
    if is_translation_unit "$file"; then
        TIDY_FILES+=("$file")
    fi
done

echo "Files: ${#FILES[@]} (${#TIDY_FILES[@]} translation unit(s))"

run_token_tool() {
    local mode="$1"
    shift

    python3 - "$mode" "$@" <<'PY'
from __future__ import annotations

import re
import sys
from pathlib import Path

MODE = sys.argv[1]
PATHS = [Path(p) for p in sys.argv[2:]]
MAX_COLUMNS = 120


def raw_string_end(text: str, start: int) -> int | None:
    prefixes = ("u8R\"", "uR\"", "UR\"", "LR\"", "R\"")
    prefix = next((p for p in prefixes if text.startswith(p, start)), None)

    if prefix is None:
        return None

    delim_start = start + len(prefix)
    open_paren = text.find("(", delim_start, delim_start + 17)

    if open_paren == -1:
        return None

    delimiter = text[delim_start:open_paren]

    if any(ch in delimiter for ch in " \\\t\r\n()"):
        return None

    closing = ")" + delimiter + '"'
    end = text.find(closing, open_paren + 1)

    return len(text) if end == -1 else end + len(closing)


def transform(text: str, rewrite: bool) -> tuple[str, list[tuple[int, str]]]:
    out: list[str] = []
    violations: list[tuple[int, str]] = []
    i = 0
    line = 1

    def copy(fragment: str) -> None:
        nonlocal line
        out.append(fragment)
        line += fragment.count("\n")

    while i < len(text):
        raw_end = raw_string_end(text, i)

        if raw_end is not None:
            copy(text[i:raw_end])
            i = raw_end
            continue

        if text.startswith("//", i):
            end = text.find("\n", i)

            if end == -1:
                copy(text[i:])
                break

            copy(text[i:end + 1])
            i = end + 1
            continue

        if text.startswith("/*", i):
            end = text.find("*/", i + 2)

            if end == -1:
                copy(text[i:])
                break

            end += 2
            copy(text[i:end])
            i = end
            continue

        ch = text[i]

        if ch in {'"', "'"}:
            quote = ch
            start = i
            i += 1

            while i < len(text):
                if text[i] == "\\":
                    i += 2
                    continue

                if i < len(text) and text[i] == quote:
                    i += 1
                    break

                i += 1

            copy(text[start:i])
            continue

        if text.startswith("&&", i):
            if rewrite:
                out.append(" and ")
            else:
                out.append("&&")
                violations.append((line, "use 'and' instead of '&&'"))
            i += 2
            continue

        if text.startswith("||", i):
            if rewrite:
                out.append(" or ")
            else:
                out.append("||")
                violations.append((line, "use 'or' instead of '||'"))
            i += 2
            continue

        if ch == "!" and not text.startswith("!=", i):
            if rewrite:
                out.append("not ")
            else:
                out.append("!")
                violations.append((line, "use 'not' instead of '!'"))
            i += 1
            continue

        out.append(ch)

        if ch == "\n":
            line += 1

        i += 1

    return "".join(out), violations


def code_only(text: str) -> str:
    out: list[str] = []
    i = 0

    def blank(fragment: str) -> str:
        return "".join("\n" if c == "\n" else " " for c in fragment)

    while i < len(text):
        raw_end = raw_string_end(text, i)

        if raw_end is not None:
            out.append(blank(text[i:raw_end]))
            i = raw_end
            continue

        if text.startswith("//", i):
            end = text.find("\n", i)

            if end == -1:
                out.append(blank(text[i:]))
                break

            out.append(blank(text[i:end]))
            out.append("\n")
            i = end + 1
            continue

        if text.startswith("/*", i):
            end = text.find("*/", i + 2)

            if end == -1:
                out.append(blank(text[i:]))
                break

            end += 2
            out.append(blank(text[i:end]))
            i = end
            continue

        if text[i] in {'"', "'"}:
            quote = text[i]
            start = i
            i += 1

            while i < len(text):
                if text[i] == "\\":
                    i += 2
                    continue

                if i < len(text) and text[i] == quote:
                    i += 1
                    break

                i += 1

            out.append(blank(text[start:i]))
            continue

        out.append(text[i])
        i += 1

    return "".join(out)


split_function_re = re.compile(
    r"^[ \t]*(?:~?[A-Za-z_]\w*::)*~?[A-Za-z_]\w*\s*\("
)
control_prefixes = (
    "if",
    "for",
    "while",
    "switch",
    "catch",
    "return",
    "sizeof",
    "alignof",
)


def split_return_type_violations(text: str) -> list[tuple[int, str]]:
    lines = text.splitlines()
    result: list[tuple[int, str]] = []

    for idx in range(1, len(lines)):
        current = lines[idx]
        previous = lines[idx - 1]
        stripped = current.lstrip()

        if not split_function_re.match(current):
            continue
        if stripped.startswith(control_prefixes):
            continue
        if not previous.strip():
            continue
        if previous.rstrip().endswith(("{", "}", ";", ":", ",", "(", "[", "=")):
            continue
        if previous.lstrip().startswith(("//", "/*", "*", "#")):
            continue

        if re.search(r"(?:[A-Za-z_]\w*|[>*&]|const|volatile)\s*$", previous):
            result.append((idx + 1, stripped.split("(", 1)[0].strip()))

    return result


failed = False

for path in PATHS:
    text = path.read_text(encoding="utf-8")

    if MODE == "rewrite":
        new, _ = transform(text, True)
        if new != text:
            path.write_text(new, encoding="utf-8")
        continue

    if MODE == "check-rewrite":
        _, violations = transform(text, False)

        for line_no, message in violations:
            print(f"{path}:{line_no}: {message}", file=sys.stderr)
            failed = True

        continue

    if MODE == "verify":
        _, violations = transform(text, False)

        for line_no, message in violations:
            print(f"{path}:{line_no}: {message}", file=sys.stderr)
            failed = True

        for line_no, line in enumerate(text.splitlines(), 1):
            columns = len(line.expandtabs(4))

            if columns > MAX_COLUMNS:
                print(
                    f"{path}:{line_no}: line is {columns} columns; max is {MAX_COLUMNS}",
                    file=sys.stderr,
                )
                failed = True

        for line_no, name in split_return_type_violations(code_only(text)):
            print(
                f"{path}:{line_no}: function name '{name}' is split from its return type",
                file=sys.stderr,
            )
            failed = True

        continue

    raise SystemExit(f"unknown mode: {MODE}")

raise SystemExit(1 if failed else 0)
PY
}

run_signature_tool() {
    local mode="$1"
    shift

    python3 - "$mode" "$@" <<'PY'
from __future__ import annotations

import re
import sys
from pathlib import Path

MODE = sys.argv[1]
PATHS = [Path(p) for p in sys.argv[2:]]

CONTROL_PREFIXES = (
    "if ",
    "if(",
    "for ",
    "for(",
    "while ",
    "while(",
    "switch ",
    "switch(",
    "catch ",
    "catch(",
    "return ",
    "co_return ",
    "sizeof",
    "alignof",
    "static_assert",
)

SIGNATURE_START_RE = re.compile(
    r"^(?P<indent>[ \t]*)"
    r"(?P<body>.+?)"
    r"(?P<name>(?:operator\s*[^\s(]+|~?[A-Za-z_]\w*))"
    r"\s*\(\s*$"
)


def is_signature_start(line: str) -> tuple[bool, str]:
    match = SIGNATURE_START_RE.match(line)

    if not match:
        return False, ""

    stripped = line.strip()

    if stripped.startswith(CONTROL_PREFIXES):
        return False, ""

    body = match.group("body").strip()

    if not body:
        return False, ""

    if "=" in body or "." in body or "->" in body:
        return False, ""

    if stripped.startswith("#"):
        return False, ""

    return True, match.group("indent")


def find_matching_paren(text: str, open_pos: int) -> int | None:
    depth = 0
    i = open_pos
    state = "code"
    raw_delim = ""

    while i < len(text):
        if state == "line_comment":
            if text[i] == "\n":
                state = "code"
            i += 1
            continue

        if state == "block_comment":
            if text.startswith("*/", i):
                state = "code"
                i += 2
            else:
                i += 1
            continue

        if state in {"string", "char"}:
            quote = '"' if state == "string" else "'"

            if text[i] == "\\":
                i += 2
                continue

            if text[i] == quote:
                state = "code"

            i += 1
            continue

        if state == "raw":
            closing = ")" + raw_delim + '"'

            if text.startswith(closing, i):
                state = "code"
                i += len(closing)
            else:
                i += 1

            continue

        if text.startswith("//", i):
            state = "line_comment"
            i += 2
            continue

        if text.startswith("/*", i):
            state = "block_comment"
            i += 2
            continue

        raw_match = re.match(
            r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(',
            text[i:],
        )

        if raw_match:
            raw_delim = raw_match.group(1)
            state = "raw"
            i += raw_match.end()
            continue

        if text[i] == '"':
            state = "string"
            i += 1
            continue

        if text[i] == "'":
            state = "char"
            i += 1
            continue

        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1

            if depth == 0:
                return i

        i += 1

    return None


def signature_ranges(text: str):
    lines = text.splitlines(keepends=True)
    offset = 0

    for line_no, line_with_nl in enumerate(lines, 1):
        line = line_with_nl.rstrip("\r\n")
        is_signature, indent = is_signature_start(line)

        if is_signature:
            open_in_line = line.rfind("(")

            if open_in_line >= 0:
                open_pos = offset + open_in_line
                close_pos = find_matching_paren(text, open_pos)

                if close_pos is not None:
                    yield line_no, open_pos, close_pos, indent

        offset += len(line_with_nl)


def normalize(text: str) -> str:
    edits: list[tuple[int, int, str]] = []

    for _, open_pos, close_pos, indent in signature_ranges(text):
        if "\n" not in text[open_pos:close_pos]:
            continue

        line_start = text.rfind("\n", 0, close_pos) + 1
        before_close = text[line_start:close_pos]

        if before_close == indent:
            continue

        if before_close.strip() == "":
            edits.append((line_start, close_pos, indent))
            continue

        edits.append((close_pos, close_pos, "\n" + indent))

    for start, end, replacement in reversed(edits):
        text = text[:start] + replacement + text[end:]

    return text


def verify(text: str, path: Path) -> bool:
    ok = True

    for _, open_pos, close_pos, indent in signature_ranges(text):
        if "\n" not in text[open_pos:close_pos]:
            continue

        line_start = text.rfind("\n", 0, close_pos) + 1
        before_close = text[line_start:close_pos]

        if before_close != indent:
            close_line = text.count("\n", 0, close_pos) + 1
            print(
                f"{path}:{close_line}: closing ')' of multiline function signature "
                "must be on its own line at the function indentation",
                file=sys.stderr,
            )
            ok = False

    return ok


failed = False

for path in PATHS:
    text = path.read_text(encoding="utf-8")

    if MODE == "normalize":
        new_text = normalize(text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
    elif MODE == "verify":
        failed = not verify(text, path) or failed
    else:
        raise SystemExit(f"unknown signature mode: {MODE}")

raise SystemExit(1 if failed else 0)
PY
}

run_assignment_tool() {
    local mode="$1"
    shift

    python3 - "$mode" "$@" <<'PY'
from __future__ import annotations

import re
import sys
from pathlib import Path

MODE = sys.argv[1]
PATHS = [Path(p) for p in sys.argv[2:]]

EXTRA_INDENT = 8
ASSIGNMENT_RE = re.compile(r"^(?P<indent>[ \t]*)=(?!=)")


def raw_string_end(text: str, start: int) -> int | None:
    prefixes = ("u8R\"", "uR\"", "UR\"", "LR\"", "R\"")
    prefix = next((p for p in prefixes if text.startswith(p, start)), None)

    if prefix is None:
        return None

    delim_start = start + len(prefix)
    open_paren = text.find("(", delim_start, delim_start + 17)

    if open_paren == -1:
        return None

    delimiter = text[delim_start:open_paren]

    if any(ch in delimiter for ch in " \\\t\r\n()"):
        return None

    closing = ")" + delimiter + '"'
    end = text.find(closing, open_paren + 1)

    return len(text) if end == -1 else end + len(closing)


def code_only(text: str) -> str:
    out: list[str] = []
    i = 0

    def blank(fragment: str) -> str:
        return "".join("\n" if char == "\n" else " " for char in fragment)

    while i < len(text):
        raw_end = raw_string_end(text, i)

        if raw_end is not None:
            out.append(blank(text[i:raw_end]))
            i = raw_end
            continue

        if text.startswith("//", i):
            end = text.find("\n", i)

            if end == -1:
                out.append(blank(text[i:]))
                break

            out.append(blank(text[i:end]))
            out.append("\n")
            i = end + 1
            continue

        if text.startswith("/*", i):
            end = text.find("*/", i + 2)

            if end == -1:
                out.append(blank(text[i:]))
                break

            end += 2
            out.append(blank(text[i:end]))
            i = end
            continue

        if text[i] in {'"', "'"}:
            quote = text[i]
            start = i
            i += 1

            while i < len(text):
                if text[i] == "\\":
                    i += 2
                    continue

                if i < len(text) and text[i] == quote:
                    i += 1
                    break

                i += 1

            out.append(blank(text[start:i]))
            continue

        out.append(text[i])
        i += 1

    return "".join(out)


def line_ending(line: str) -> str:
    if line.endswith("\r\n"):
        return "\r\n"
    if line.endswith("\n"):
        return "\n"
    return ""


def leading_whitespace(line: str) -> str:
    return line[:len(line) - len(line.lstrip(" \t"))]


def is_alias_declaration(line: str) -> bool:
    return line.lstrip().startswith("using ")


def normalize(text: str) -> str:
    lines = text.splitlines(keepends=True)
    code_lines = code_only(text).splitlines(keepends=True)

    for index in range(1, min(len(lines), len(code_lines))):
        code_line = code_lines[index].rstrip("\r\n")

        if not ASSIGNMENT_RE.match(code_line):
            continue

        previous_code = code_lines[index - 1].rstrip("\r\n")

        if not previous_code.strip() or is_alias_declaration(previous_code):
            continue

        previous_line = lines[index - 1].rstrip("\r\n")
        current_line = lines[index].rstrip("\r\n")

        indent = leading_whitespace(previous_line) + " " * EXTRA_INDENT
        body = current_line.lstrip(" \t")
        lines[index] = indent + body + line_ending(lines[index])

    return "".join(lines)


def verify(text: str, path: Path) -> bool:
    lines = text.splitlines()
    code_lines = code_only(text).splitlines()
    ok = True

    for index in range(1, min(len(lines), len(code_lines))):
        if not ASSIGNMENT_RE.match(code_lines[index]):
            continue

        previous_code = code_lines[index - 1]

        if not previous_code.strip() or is_alias_declaration(previous_code):
            continue

        previous_indent = len(leading_whitespace(lines[index - 1]).expandtabs(4))
        actual_indent = len(leading_whitespace(lines[index]).expandtabs(4))
        expected_indent = previous_indent + EXTRA_INDENT

        if actual_indent != expected_indent:
            print(
                f"{path}:{index + 1}: wrapped assignment '=' must be "
                f"{expected_indent} columns indented, got {actual_indent}",
                file=sys.stderr,
            )
            ok = False

    return ok


failed = False

for path in PATHS:
    text = path.read_text(encoding="utf-8")

    if MODE == "normalize":
        new_text = normalize(text)
        if new_text != text:
            path.write_text(new_text, encoding="utf-8")
    elif MODE == "verify":
        failed = not verify(text, path) or failed
    else:
        raise SystemExit(f"unknown assignment mode: {MODE}")

raise SystemExit(1 if failed else 0)
PY
}

check_composite_format() {
    local failed=0
    local i=0
    local file
    local tmp

    mkdir -p -- "$RUNTIME_DIR/format-check"

    for file in "${FILES[@]}"; do
        i=$((i + 1))
        tmp="$RUNTIME_DIR/format-check/${i}-$(basename -- "$file")"

        cp -- "$file" "$tmp"

        "$CLANG_FORMAT_BIN" \
            -i \
            --style="file:$FORMAT_CONFIG" \
            "$tmp"

        run_assignment_tool normalize "$tmp"
        run_signature_tool normalize "$tmp"

        if ! cmp -s -- "$file" "$tmp"; then
            echo "Formatting differs: $file" >&2

            diff \
                -u \
                --label "$file" \
                --label "$file (formatted)" \
                "$file" \
                "$tmp" \
                >&2 \
                || true

            failed=1
        fi
    done

    return "$failed"
}

ensure_compile_db() {
    local requested="$BUILD_DIR"
    local candidate
    local db
    local auto_dir

    if [[ -n "$requested" ]]; then
        [[ "$requested" = /* ]] || requested="$PROJECT_ROOT/$requested"

        mkdir -p -- "$requested"
        requested="$(cd -- "$requested" && pwd)"

        if [[ -f "$requested/compile_commands.json" ]]; then
            BUILD_DIR="$requested"
            return 0
        fi

        candidate="$requested"
    else
        for candidate in \
            "$PROJECT_ROOT" \
            "$PROJECT_ROOT/build" \
            "$PROJECT_ROOT/cmake-build-debug" \
            "$PROJECT_ROOT/cmake-build-release"; do

            if [[ -f "$candidate/compile_commands.json" ]]; then
                BUILD_DIR="$candidate"
                return 0
            fi
        done

        db="$(
            find "$PROJECT_ROOT" \
                -maxdepth 3 \
                -type f \
                -name compile_commands.json \
                -not -path '*/.git/*' \
                -print \
                -quit \
                2>/dev/null \
                || true
        )"

        if [[ -n "$db" ]]; then
            BUILD_DIR="$(dirname -- "$db")"
            return 0
        fi

        auto_dir="$RUNTIME_DIR/cmake-build"
        mkdir -p -- "$auto_dir"
        candidate="$auto_dir"
    fi

    [[ -f "$PROJECT_ROOT/CMakeLists.txt" ]] || {
        echo "No compile_commands.json and no CMakeLists.txt at $PROJECT_ROOT" >&2
        return 1
    }

    [[ -n "$CMAKE" && -x "$CMAKE" ]] || {
        echo "No compile_commands.json and cmake is not available." >&2
        return 1
    }

    echo "No compile_commands.json found; generating a temporary one..."

    local cmake_path="$PATH"
    local tool_dir

    for tool_dir in \
        "$(dirname -- "$CLANG_FORMAT_BIN")" \
        "$(dirname -- "$CLANG_TIDY_BIN")"; do

        case ":$cmake_path:" in
            *":$tool_dir:"*)
                ;;
            *)
                cmake_path="$tool_dir:$cmake_path"
                ;;
        esac
    done

    if [[ -n "$LLVM_CONFIG_BIN" ]]; then
        tool_dir="$(dirname -- "$LLVM_CONFIG_BIN")"

        case ":$cmake_path:" in
            *":$tool_dir:"*)
                ;;
            *)
                cmake_path="$tool_dir:$cmake_path"
                ;;
        esac
    fi

    cmake_args=(
        -S "$PROJECT_ROOT"
        -B "$candidate"
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    )

    if [[ -n "$LLVM_CONFIG_BIN" ]] \
        && grep -q 'LLVM_CONFIG' "$PROJECT_ROOT/CMakeLists.txt"; then

        cmake_args+=("-DLLVM_CONFIG=$LLVM_CONFIG_BIN")
    fi

    if ! PATH="$cmake_path" "$CMAKE" "${cmake_args[@]}"; then
        echo "CMake could not generate compile_commands.json." >&2
        return 1
    fi

    [[ -f "$candidate/compile_commands.json" ]] || {
        echo "CMake succeeded but $candidate/compile_commands.json was not created." >&2
        return 1
    }

    BUILD_DIR="$candidate"
    echo "Compilation database: $BUILD_DIR/compile_commands.json"
}

if [[ "$STYLE_ONLY" -eq 0 ]]; then
    ((${#TIDY_FILES[@]} > 0)) || {
        echo "No translation units (.c/.cc/.cpp/.cxx) available for clang-tidy." >&2
        echo "Use --style-only when formatting header-only input." >&2
        exit 2
    }

    ensure_compile_db || exit 2
fi

run_tidy() {
    local config="$1"
    local fix="$2"

    shift 2

    local args=(
        --config-file="$config"
        -p "$BUILD_DIR"
    )

    if [[ "$fix" -eq 1 ]]; then
        args+=(
            --fix
            --format-style=none
        )
    else
        args+=(--warnings-as-errors='*')
    fi

    "$CLANG_TIDY_BIN" "${args[@]}" "$@"
}

if [[ "$CHECK_ONLY" -eq 1 ]]; then
    echo "Checking logical operators..."
    run_token_tool check-rewrite "${FILES[@]}"

    echo "Checking clang-format + project postprocessing..."
    check_composite_format

    if [[ "$STYLE_ONLY" -eq 0 ]]; then
        echo "Checking braces and operator representation with clang-tidy..."
        run_tidy "$TIDY_STYLE_CONFIG" 0 "${TIDY_FILES[@]}"

        echo "Checking const/const& opportunities with clang-tidy..."
        run_tidy "$TIDY_SEMANTIC_CONFIG" 0 "${TIDY_FILES[@]}"
    fi

    echo "Checking wrapped assignment indentation..."
    run_assignment_tool verify "${FILES[@]}"

    echo "Checking multiline function signature closing parens..."
    run_signature_tool verify "${FILES[@]}"

    echo "Checking hard invariants (120 columns and return-type/function-name)..."
    run_token_tool verify "${FILES[@]}"

    echo "All checks passed."
    exit 0
fi

echo "Rewriting !/&&/|| -> not/and/or..."
run_token_tool rewrite "${FILES[@]}"

if [[ "$STYLE_ONLY" -eq 0 ]]; then
    echo "Adding required braces with clang-tidy..."
    run_tidy "$TIDY_STYLE_CONFIG" 1 "${TIDY_FILES[@]}"

    echo "Applying safe clang-tidy const/const& fixes..."
    run_tidy "$TIDY_SEMANTIC_CONFIG" 1 "${TIDY_FILES[@]}"
fi

echo "Applying clang-format..."

"$CLANG_FORMAT_BIN" \
    -i \
    --style="file:$FORMAT_CONFIG" \
    "${FILES[@]}"

echo "Normalizing wrapped assignments..."
run_assignment_tool normalize "${FILES[@]}"

echo "Normalizing multiline function signatures..."
run_signature_tool normalize "${FILES[@]}"

if [[ "$STYLE_ONLY" -eq 0 ]]; then
    echo "Verifying braces and operator representation..."
    run_tidy "$TIDY_STYLE_CONFIG" 0 "${TIDY_FILES[@]}"
fi

echo "Verifying wrapped assignments..."
run_assignment_tool verify "${FILES[@]}"

echo "Verifying multiline function signatures..."
run_signature_tool verify "${FILES[@]}"

echo "Verifying formatter idempotence..."
check_composite_format

echo "Verifying hard invariants..."
run_token_tool verify "${FILES[@]}"

echo "Formatting completed successfully."
