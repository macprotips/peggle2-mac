// Stand-in for Xenia's gpu/gpu_flags.h, with the defaults of Xenia's gpu_flags.cc.
#pragma once
#include <filesystem>
namespace cvars
{
    inline std::filesystem::path dump_shaders;
    inline bool vsync = true;
    inline bool gpu_allow_invalid_fetch_constants = false;
    inline bool non_seamless_cube_map = true;
    inline bool half_pixel_offset = true;
    inline int32_t query_occlusion_fake_sample_count = 1000;
}
