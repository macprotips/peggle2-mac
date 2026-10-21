// Host copies of guest textures, keyed by fetch constant and refreshed when guest memory changes.
#pragma once
#import <Metal/Metal.h>
#include "xenia/gpu/xenos.h"
#include <functional>

namespace rt::gpu
{
    void InitializeTextures(id<MTLDevice> device);

    // nil when the texture's dimension or format is unsupported (logged once).
    id<MTLTexture> GetTexture(const xe::gpu::xenos::xe_gpu_texture_fetch_t& fetch);
    id<MTLSamplerState> GetSampler(const xe::gpu::xenos::xe_gpu_texture_fetch_t& fetch);

    // Guest memory may have changed (a new frame or a resolve): recheck contents on next use.
    void InvalidateTextures();

    // Resolves keep a GPU copy of the k_8_8_8_8 data they write to guest memory at `base` (width x height
    // texels, unswizzled), at the render scale. GetTexture serves fetches of that image, or of its top-left part, from it.
    // Note: CPU writes into resolved memory are not noticed; add write watches if a title does that.
    id<MTLTexture> GetResolveTexture(uint32_t base, uint32_t width, uint32_t height, uint32_t scale);

    // Finishes submitted GPU work, for reading guest memory a resolve may still be writing.
    void SetTextureSync(std::function<void()> sync);
    // Encodes a GPU copy of the source's top-left part, the destination's size, into the destination.
    void SetTextureCopy(std::function<void(id<MTLTexture>, id<MTLTexture>)> copy);

    // Uploads since the last call (statistics).
    uint32_t TakeTextureUploadCount();
}
