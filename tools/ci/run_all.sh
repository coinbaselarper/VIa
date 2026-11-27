#!/bin/bash
# usage: tools/ci/run_all.sh [--arch arm64|x86_64|both] [--units a,b] [--test-timeout S] [--no-build] [--build-only] [--out DIR]
# Fresh configure+build of port/build/ci-{arm64,x86_64} (plus ci-<unit>-<arch> for standalone modules), CTest per tree,
# JSON summary + table under port/build/ci-results/<stamp>/. Never runs the gta6rt host. Exit 0 only if everything passed.
exec python3 -I "$(cd "$(dirname "$0")" && pwd)/ci_run.py" "$@"
