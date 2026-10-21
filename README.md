# Peggle 2 for Mac

A native macOS port of the Xbox 360 (Xbox Live Arcade) release of Peggle 2, for Apple silicon Macs.

**Beta.** It plays through on the Macs it was developed on, but it needs testing on more Macs.
If something goes wrong, please open an issue with your Mac model and macOS version.

## How it works

The Xbox 360's CPU is not emulated. The game's PowerPC executable is statically recompiled ahead of time
to C++ with [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), then compiled to native ARM64 code
and linked with a runtime that stands in for the rest of the console:

- **Kernel and system libraries:** the Xbox 360 kernel and XAM functions the game imports, reimplemented
  on macOS (threads, synchronisation, files, saves, achievements).
- **GPU:** the game's Xenos command stream is processed and drawn with Metal; its shader microcode is
  translated to Metal Shading Language.
- **Audio:** XMA is decoded with FFmpeg and played through Core Audio.
- **Input:** controllers, keyboard and mouse, including mouse aiming.

It can also run at 60 fps (every one of the game's 60 logic ticks per second is drawn) and render at up
to 4K.

## You need your own copy of the game

No game code or assets are included. You need the Xbox Live Arcade release of Peggle 2 from your own
Xbox 360. Copy it off the console's storage, for example with a USB drive formatted by the console and a
tool such as Horizon or Velocity, or over FTP from a modded console. You want the single file in
`Content/0000000000000000/58411436/000D0000/`.

## Install

Requires an Apple silicon Mac with macOS 13 or later.

1. Download **Peggle 2 Installer** from [Releases](../../releases) and unzip it.
2. Open it. The first time, macOS may say it can't verify the app: open System Settings > Privacy &
   Security and click Open Anyway.
3. If asked, install Apple's Command Line Tools, then open the installer again.
4. Drop your game file into the window.

The installer builds Peggle 2 on your Mac from your copy of the game, which takes a few minutes once,
puts it in Applications and opens it.

## Controls

| | Mouse | Keyboard | Controller |
|---|---|---|---|
| Aim | point | arrow keys | left stick |
| Shoot / select | left click | Space | A |
| Back | right click | Esc | B |
| Fine aim | scroll wheel | Q, E | LB, RB |

Frame rate, resolution and aiming options are in the Settings menu.

## Building from source

Requires Xcode command-line tools, CMake, Ninja and Python 3.

```
./setup.sh
build/install_game /path/to/your/game/file game
tools/regen.sh
```

This builds `build/Peggle2.app` and `build/Peggle 2 Installer.app`.

## Credits

Built on XenonRecomp, GPU code from [Xenia](https://github.com/xenia-project/xenia), and FFmpeg.
See `THIRD_PARTY_NOTICES.md`.
