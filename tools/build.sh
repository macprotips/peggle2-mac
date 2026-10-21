#!/bin/bash
# Builds build/Peggle2.app; prints only errors, and fails if the build fails.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$(cmake --build "$ROOT/build" --target Peggle2 2>&1)
STATUS=$?
echo "$OUT" | grep -E ' error|FAILED' | head -20
exit $STATUS
