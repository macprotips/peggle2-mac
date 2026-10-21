#!/bin/bash
# Starts the game, waits N seconds, then dumps every thread's stack (guest frames are sub_XXXXXXXX).
# usage: [RUN_ENV='PEGGLE2_X=1 ...'] tools/hang_bt.sh [seconds]
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WAIT=${1:-15}
env $RUN_ENV "$ROOT/build/Peggle2.app/Contents/MacOS/Peggle2" "$ROOT/game" > "$ROOT/out/run.log" 2>&1 &
PID=$!
sleep "$WAIT"
lldb -p $PID -b -o "thread backtrace all -c 14" -o "process kill" > "$ROOT/out/hang.log" 2>&1
kill -9 $PID 2>/dev/null
grep -E '^\* thread|^  thread|frame #' "$ROOT/out/hang.log" | sed -E 's/0x[0-9a-f]+ //; s/ \+ [0-9]+$//; s/\(PPCContext&, unsigned char\*\)//' | awk '{$1=$1};1'
