# Tools

## Building

- `regen.sh`: recompiles the game in `game/` (jump tables, function bounds, XenonRecomp, import stubs), then
  builds the app. Run it after changing `config/` or the recompiler.
- `build.sh`: builds `build/Peggle2.app`.
- `install_game`: unpacks the game file into a folder (`build/install_game <file> game`), with the
  installer's checks.
- `find_switches.py`, `fix_switch_bounds.py`, `gen_import_stubs.py`: steps of `regen.sh`.
- `make_icon.sh`, `sign_bundle.sh`: app bundle steps, also used by the installer.
- `installer/`: the Peggle 2 Installer app and the scripts it runs on the player's Mac.

## Running and debugging

- `run.sh`, `crash_bt.sh`, `hang_bt.sh`: run the game for a while, capturing its log, a crash backtrace, or
  every thread's stack. `RUN_ENV` passes `PEGGLE2_*` diagnostic settings.

## Reverse engineering (on `out/analysis/image.bin`, written by `regen.sh` via `xexdump`)

- `xexdump`: writes a XEX's mapped image and a summary of its sections and imports.
- `disas`: disassembles around addresses.
- `fnanalyze`: shows how XenonAnalyse splits a function into blocks.
- `rtti.py`: names vtables from MSVC RTTI.
- `inventory.py`: lists functions, save/restore helpers and middleware strings.
- `xenos_disasm`, `msl_check`: disassemble shaders dumped with `PEGGLE2_DUMP_SHADERS`, and check that they
  translate to Metal.

## Diagnostics

Environment variables for diagnostics and testing (with `tools/run.sh`, pass them in `RUN_ENV="..."`):

- Logging: `PEGGLE2_TRACE=1` logs kernel import calls (`all` includes lock and TLS traffic), with
  `PEGGLE2_BACKTRACE=<import>` printing the guest stack at that import. `PEGGLE2_GAME_LOG=1` logs the game's
  own debug messages (those containing "Error" always are). `PEGGLE2_COUNT_UNIMPLEMENTED=1` repeats the
  report for unimplemented imports every 50 calls. `PEGGLE2_MEMLOG=1` logs memory allocations.
- Performance: `PEGGLE2_PERF=1` prints, per second, the longest game tick and draw, the most command
  processor time a frame took apart from its waits, and texture settle waits. `PEGGLE2_GPU_STATS=1` and
  `PEGGLE2_AUDIO_STATS=1` print per-second renderer and audio figures.
- GPU: `PEGGLE2_GPU_TRACE=1` logs every PM4 packet; `PEGGLE2_GPU_STATUS=1` logs the ring pointers every 2 s.
  `PEGGLE2_TRACE_FRAME=<n>` logs every draw of one frame and dumps its textures to `out/trace`.
  `PEGGLE2_DUMP_SHADERS=<dir>` saves each shader's microcode, for `xenos_disasm` and `msl_check`.
- Captures: `PEGGLE2_DUMP_FRAMES=<dir>` saves the front buffer as PNG every `PEGGLE2_DUMP_EVERY` frames
  (default 300) from frame `PEGGLE2_DUMP_FROM`. `PEGGLE2_AUDIO_DUMP=<file>` records the output as raw stereo
  float32 at 48 kHz. `PEGGLE2_UI_SNAPSHOT=<dir>` saves achievement toasts and the achievements window as PNG;
  `PEGGLE2_TEST_TOAST=<id>` shows one achievement's toast. `PEGGLE2_MEMSNAP=<frames>` writes heap snapshots
  to `out/snap`.
- Test runs: `PEGGLE2_INPUT_SCRIPT=1000@300-310,L-1@400-460,M640:600@500,K@510,R@520-600,2:1000@700` plays
  buttons, stick, mouse moves and left (`K`) or right (`R`) mouse buttons over frame ranges (`2:` for player
  2). `PEGGLE2_DATA_DIR=<dir>` keeps saves away from the real profile. `PEGGLE2_CONSOLE="<frame>:<command>"`
  sends the game's developer-console commands (for example `/profile.unlock`). `PEGGLE2_BACKGROUND=1` opens
  the window behind others, and `PEGGLE2_NO_AUDIO_DEVICE=1` plays audio to nowhere in real time.
- Settings: `PEGGLE2_FPS=<30|60>`, `PEGGLE2_SCALE=<1-3>`, `PEGGLE2_AIM_IN_PLAY=1` and `PEGGLE2_SKIP_INTRO=1`
  override the saved settings. `PEGGLE2_AIM_K` and `PEGGLE2_AIM_MUZZLE` tune the mouse aim's ballistic model.
