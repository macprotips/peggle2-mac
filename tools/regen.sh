#!/bin/bash
# Regenerates jump tables, frameless function bounds, the recompiled C++ and the import stubs, then builds.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
mkdir -p out/analysis
build/xexdump game/Default.xex out/analysis/image.bin > out/analysis/xexinfo.txt
python3 -I tools/find_switches.py out/analysis/image.bin config/switch_tables.toml | grep -v '^   base'
python3 -I - <<'PY'
c='config/Peggle2.toml'; t=open(c).read(); open(c,'w').write(t.split('\n# Frameless functions containing jump tables')[0].rstrip()+'\n')
PY
python3 -I tools/fix_switch_bounds.py out/analysis/image.bin config/switch_tables.toml config/functions.toml
{ echo; echo '# Frameless functions containing jump tables (tools/fix_switch_bounds.py)'; cat config/functions.toml; } >> config/Peggle2.toml
rm config/functions.toml
rm -rf out/ppc && mkdir -p out/ppc
./build/third_party/XenonRecomp/XenonRecomp/XenonRecomp config/Peggle2.toml third_party/XenonRecomp/XenonUtils/ppc_context.h > out/recomp.log 2>&1
echo "switch errors: $(grep -c 'Switch case' out/recomp.log || true)  unrecognized: $(grep -c Unrecognized out/recomp.log || true)  functions: $(grep -c sub_ out/ppc/ppc_func_mapping.cpp)"
cp third_party/XenonRecomp/XenonUtils/ppc_context.h out/ppc/
python3 -I tools/gen_import_stubs.py out/ppc/ppc_recomp_shared.h runtime/kernel/import_stubs.cpp
cmake -S "$ROOT" -B "$ROOT/build" > /dev/null # picks up the generated sources
tools/build.sh
