Files under `xenia/gpu/`, `xenia/ui/` and `xenia/apu/xma_*`, and `xenia/base/bit_stream.*` and
`xenia/base/ring_buffer.*`, are taken unmodified from Xenia (commit in `XENIA_COMMIT`),
BSD-3-Clause, see the license in each file and THIRD_PARTY_NOTICES.md, except these stand-ins:
`xenia/gpu/gpu_flags.h`, `xenia/gpu/trace_writer.h`, `xenia/gpu/texture_cache.h`, `xenia/apu/xma_decoder.h`,
`xenia/xbox.h`, `xenia/base/profiling.h`. `xenia/base/`,
`xenia/memory.h` and `third_party/` hold small stand-ins for the parts of Xenia's base library,
memory system and include layout those files use.
