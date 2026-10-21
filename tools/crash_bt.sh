#!/bin/bash
# Runs the game under lldb for up to N seconds; on a crash prints the stop reason and backtrace.
# usage: [RUN_ENV='PEGGLE2_X=1 ...'] tools/crash_bt.sh [seconds] [frames]
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LIMIT=${1:-15}
FRAMES=${2:-12}
CMDS=$(mktemp)
printf 'run\nthread backtrace -c %s\nregister read x30\nquit\n' "$FRAMES" > "$CMDS"
env PEGGLE2_BACKGROUND=1 $RUN_ENV lldb -b -s "$CMDS" -- "$ROOT/build/Peggle2.app/Contents/MacOS/Peggle2" "$ROOT/game" > "$ROOT/out/lldb.log" 2>&1 &
PID=$!
for ((i = 0; i < LIMIT * 10; i++)); do
    kill -0 $PID 2>/dev/null || break
    sleep 0.1
done
if kill -0 $PID 2>/dev/null; then
    pkill -9 -f "build/Peggle2.app/Contents/MacOS/Peggle2 $ROOT/game" 2>/dev/null
    kill -9 $PID 2>/dev/null
    echo "no crash in ${LIMIT}s"
else
    grep -E 'stop reason|frame #|x30 =' "$ROOT/out/lldb.log" | grep -v '^\[' | head -$((FRAMES + 4))
fi
rm -f "$CMDS"
