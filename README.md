# Peggle 2 for Mac

A native macOS port of the Xbox 360 version of Peggle 2, for Apple silicon Macs.

**Beta.** It plays start to finish, but it needs testing on more Macs. If something goes wrong, open an
issue with your Mac model, macOS version and the log from `~/Library/Logs/Peggle 2`.

## How it works

The game's PowerPC code is statically recompiled to C++ with
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) and compiled for ARM64. A runtime replaces the rest
of the console: the kernel and system calls the game makes, the GPU (drawn with Metal, its shaders
translated to Metal Shading Language), XMA audio (decoded with FFmpeg) and input.

It runs at 60 fps and can render at up to 4K. The original 30 fps is a setting.

## You need your own copy of the game

No game files are included. You need Peggle 2 (Xbox Live Arcade) from your own Xbox 360: the file in
`Content/0000000000000000/58411436/000D0000/`. Copy it off with a console-formatted USB drive and a tool
like Horizon or Velocity, or over FTP from a modded console.

## Install

Requires an Apple silicon Mac with macOS 13 or later.

1. Download **Peggle 2 Installer** from [Releases](../../releases) and unzip it.
2. Open it. The first time, macOS may say it can't verify the app: go to System Settings > Privacy &
   Security and click Open Anyway.
3. If asked, install Apple's Command Line Tools, then open the installer again.
4. Drop your game file into the window.

The installer builds Peggle 2 from your copy (a few minutes, once), puts it in Applications and opens it.

## Updating

Open the latest installer from [Releases](../../releases). It updates Peggle 2 in place, without needing
the game file again. Saves, progress and settings are kept.

## Controls

| | Mouse | Keyboard | Controller |
|---|---|---|---|
| Aim | point | arrow keys | left stick |
| Shoot / select | left click | Space (Return in menus) | A |
| Back | right click (in menus) | Esc (in menus) | B |
| Pause | | Esc (in a level) | Start |
| Speed up | hold right click (in a level) | hold X | hold X |
| Fine aim | scroll wheel | Q, E | LB, RB |

Frame rate, resolution, mouse aiming and skipping the intro videos are in Peggle 2 > Settings.

## Building from source

Requires the Xcode Command Line Tools, CMake, Ninja and Python 3.

```
./setup.sh
build/install_game /path/to/your/game/file game
tools/regen.sh
cmake --build build --target Peggle2Installer
```

This builds `build/Peggle2.app` and `build/Peggle 2 Installer.app`. The tools and diagnostics are
described in `tools/README.md`.

## Credits and license

Built on XenonRecomp, [Xenia](https://github.com/xenia-project/xenia) (GPU code) and FFmpeg. The port's
code is MIT licensed; third-party licenses are in `THIRD_PARTY_NOTICES.md`.
