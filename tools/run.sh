#!/bin/bash
# Runs the game with a time limit, logging to out/run.log.
# usage: [RUN_ENV='PEGGLE2_X=1 ...'] tools/run.sh [seconds]
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIMIT=${1:-20}
env PEGGLE2_BACKGROUND=1 $RUN_ENV "$ROOT/build/Peggle2.app/Contents/MacOS/Peggle2" "$ROOT/game" > "$ROOT/out/run.log" 2>&1 &
PID=$!
for ((i = 0; i < LIMIT * 10; i++)); do
    kill -0 $PID 2>/dev/null || break
    sleep 0.1
done
if kill -0 $PID 2>/dev/null; then
    kill -9 $PID 2>/dev/null
    echo "(killed after ${LIMIT}s)" >> "$ROOT/out/run.log"
fi
wait $PID 2>/dev/null
echo "exit=$?" >> "$ROOT/out/run.log"
