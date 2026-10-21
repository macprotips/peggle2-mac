# Third-party notices

The port's own code is MIT licensed (`LICENSE`). It builds on the projects below, under their own
licenses.

## Xenia

Xenia source files are included unmodified in `runtime/gpu/xenos` (GPU register, packet, texture and
shader microcode definitions, the shader analyzer, and the XMA decoder context). Other parts of the
runtime follow Xenia's behavior and in places closely paraphrase it: kernel structure layouts, video
(Vd*) imports, the GPU command processor and draw state, and texture layouts.
https://github.com/xenia-project/xenia

    Copyright (c) 2015, Ben Vanik.
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice, this
      list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.

    * Neither the name of the project nor the names of its
      contributors may be used to endorse or promote products derived from
      this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
    FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
    DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
    CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
    OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## XenonRecomp

The static recompiler (built from third_party/XenonRecomp with patches/XenonRecomp applied) and its
XenonUtils library, compiled into the app, are by hedge-dev and contributors, MIT licensed.
https://github.com/hedge-dev/XenonRecomp. Licence: `packaging/licenses/XenonRecomp.txt`.

XenonUtils brings these libraries into the app (license texts in `packaging/licenses/`):

- libmspack (LZX decompression of the game executable), (C) 2003-2023 Stuart Caie, GNU Lesser General
  Public License 2.1 (`LGPL-2.1.txt`). Linked statically; the app is built from source by its user, who
  can rebuild it with a modified libmspack.
- tiny-AES-c (executable decryption), public domain (`tiny-AES-c.txt`).
- TinySHA1, Copyright (c) 2012-22 Saurav Mohapatra, ISC-style license (`TinySHA1.txt`).
- xxHash, Copyright (c) 2012-2021 Yann Collet, BSD 2-clause (`xxHash.txt`).
- {fmt}, Copyright (c) 2012-present Victor Zverovich and contributors, MIT (`fmt.txt`).
- SIMDe, Copyright (c) 2017 Evan Nemerson, MIT (`simde.txt`).

The installer also contains the XenonRecomp tool itself, which recompiles the player's copy of the game.
Besides the libraries above it includes toml++ (Copyright (c) Mark Gillard, MIT, `tomlplusplus.txt`)
and a PowerPC disassembler from GNU binutils (Copyright (C) 1994-2007 Free Software Foundation, Inc.),
which is under the GNU General Public License version 2 or later (`GPL-2.0.txt`). The tool as shipped is
therefore distributed under that license. Its complete source is XenonRecomp at commit
ddd128bcca99fe8bfbb99bea583c972351fa6ace with the patches in `patches/XenonRecomp`, which `setup.sh`
fetches and applies. The disassembler is not part of the game app.

## FFmpeg (Xenia fork)

XMA audio is decoded by libavcodec and libavutil from Xenia's FFmpeg fork
(https://github.com/xenia-project/FFmpeg, commit 15ece0882e8d5875051ff5b73c5a8326f7cee9f5), licensed
under the GNU Lesser General Public License version 2.1 or later. `setup.sh` builds them from that source
as shared libraries, which the app bundles unmodified in `Contents/Frameworks`; they can be replaced with
any compatible build. The LGPL text is in `packaging/licenses/LGPL-2.1.txt`.

The built app carries this file and `packaging/licenses/` in `Contents/Resources`.
