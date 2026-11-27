#!/bin/bash
# Builds and runs submission_marks_test.cpp (no GPU, no game) with AddressSanitizer; exit 0 when every check passes.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
out="${TMPDIR:-/tmp}/submission_marks_test.$$"
trap 'rm -f "$out"' EXIT
clang++ -std=c++20 -O1 -g -fsanitize=address -Wall -Wextra -I"$here/../src" "$here/submission_marks_test.cpp" -o "$out"
"$out"
