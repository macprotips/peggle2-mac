// Texture cache: untiles guest textures into host Metal textures. Layout rules come from Xenia's
// texture_util and texture_address (BSD-3-Clause, see THIRD_PARTY_NOTICES.md).
#include "textures.h"
#include "gpu.h"
#include "../log.h"
#include "../memory.h"
#include "../thread.h"
#include "xenia/base/math.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#define XXH_INLINE_ALL
#include "xxhash.h"
#include <cstring>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rt::gpu
{
    std::chrono::steady_clock::duration g_settleWait; // time spent waiting for streaming textures to settle (PEGGLE2_PERF)
    using namespace xe::gpu;
    using xenos::TextureFormat;

    static id<MTLDevice> g_device;

    struct Entry
    {
        id<MTLTexture> texture;
        uint64_t dataHash = 0;
        uint64_t checkedGeneration = 0;
    };
    static std::unordered_map<uint64_t, Entry> g_textures;
    static std::unordered_map<uint64_t, id<MTLSamplerState>> g_samplers;
    static uint64_t g_generation = 1;

    struct Resolved
    {
        id<MTLTexture> texture; // width x height texels at the render scale
        uint32_t width, height; // guest texels
        uint32_t bytes; // guest bytes covered
        std::unordered_map<uint32_t, id<MTLTexture>> views; // by swizzle and gamma
        uint64_t resolvedGeneration = 0; // frame of the latest resolve here
    };
    static std::unordered_map<uint32_t, Resolved> g_resolved; // by guest base address
    static std::function<void()> g_sync;
    static bool g_resolvePending; // a resolve was encoded since the last wait for the GPU

    // Entries not used (textures) or resolved (resolve targets) for this many frames are dropped; a
    // dropped resolve has long since landed in guest memory.
    constexpr uint64_t CacheLifetime = 600;

    void SetTextureSync(std::function<void()> sync)
    {
        g_sync = std::move(sync);
    }

    id<MTLTexture> GetResolveTexture(uint32_t base, uint32_t width, uint32_t height, uint32_t scale)
    {
        Resolved& r = g_resolved[base];
        if (!r.texture || r.width != width || r.height != height || r.texture.width != width * scale)
        {
            MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                            width:width * scale
                                                                                           height:height * scale
                                                                                        mipmapped:NO];
            desc.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
            desc.storageMode = MTLStorageModePrivate;
            r.texture = [g_device newTextureWithDescriptor:desc];
            r.views.clear();
            r.width = width;
            r.height = height;
            r.bytes = xe::align(width, 32u) * xe::align(height, 32u) * 4;
        }
        r.resolvedGeneration = g_generation;
        g_resolvePending = true;
        return r.texture;
    }
    static uint32_t g_uploads;

    uint32_t TakeTextureUploadCount()
    {
        uint32_t count = g_uploads;
        g_uploads = 0;
        return count;
    }

    void InitializeTextures(id<MTLDevice> device)
    {
        g_device = device;
    }

    void InvalidateTextures()
    {
        g_generation++;
        if (g_generation % 60 != 0)
            return;
        std::erase_if(g_textures, [](const auto& e) { return g_generation - e.second.checkedGeneration > CacheLifetime; });
        std::erase_if(g_resolved, [](const auto& e) { return g_generation - e.second.resolvedGeneration > CacheLifetime; });
    }

    static uint32_t SwapWord(uint32_t value, xenos::Endian endian)
    {
        switch (endian)
        {
        case xenos::Endian::k8in16: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
        case xenos::Endian::k8in32: return __builtin_bswap32(value);
        case xenos::Endian::k16in32: return (value >> 16) | (value << 16);
        default: return value;
        }
    }

    // How a guest format becomes a host texture: copied block for block, or expanded to RGBA8.
    struct HostFormat
    {
        MTLPixelFormat pixel = MTLPixelFormatInvalid;
        void (*expand)(uint32_t value, uint8_t* rgba) = nullptr; // 16-bit formats
    };

    static uint8_t Expand(uint32_t v, uint32_t bits)
    {
        uint32_t max = (1u << bits) - 1;
        return uint8_t((v * 255 + max / 2) / max);
    }

    static HostFormat GetHostFormat(TextureFormat format, bool gamma)
    {
        switch (format)
        {
        case TextureFormat::k_8: return { MTLPixelFormatR8Unorm };
        case TextureFormat::k_8_8: return { MTLPixelFormatRG8Unorm };
        case TextureFormat::k_8_8_8_8:
        case TextureFormat::k_8_8_8_8_A:
            return { gamma ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm };
        case TextureFormat::k_2_10_10_10: return { MTLPixelFormatRGB10A2Unorm };
        case TextureFormat::k_DXT1: return { gamma ? MTLPixelFormatBC1_RGBA_sRGB : MTLPixelFormatBC1_RGBA };
        case TextureFormat::k_DXT2_3: return { gamma ? MTLPixelFormatBC2_RGBA_sRGB : MTLPixelFormatBC2_RGBA };
        case TextureFormat::k_DXT4_5: return { gamma ? MTLPixelFormatBC3_RGBA_sRGB : MTLPixelFormatBC3_RGBA };
        case TextureFormat::k_16_FLOAT: return { MTLPixelFormatR16Float };
        case TextureFormat::k_16_16_FLOAT: return { MTLPixelFormatRG16Float };
        case TextureFormat::k_16_16_16_16_FLOAT: return { MTLPixelFormatRGBA16Float };
        case TextureFormat::k_32_FLOAT: return { MTLPixelFormatR32Float };
        case TextureFormat::k_32_32_FLOAT: return { MTLPixelFormatRG32Float };
        case TextureFormat::k_32_32_32_32_FLOAT: return { MTLPixelFormatRGBA32Float };
        // Xenos packs the first component in the low bits.
        case TextureFormat::k_5_6_5:
            return { MTLPixelFormatRGBA8Unorm, [](uint32_t v, uint8_t* o) {
                o[0] = Expand(v & 31, 5); o[1] = Expand((v >> 5) & 63, 6); o[2] = Expand(v >> 11, 5); o[3] = 255; } };
        case TextureFormat::k_1_5_5_5:
            return { MTLPixelFormatRGBA8Unorm, [](uint32_t v, uint8_t* o) {
                o[0] = Expand(v & 31, 5); o[1] = Expand((v >> 5) & 31, 5); o[2] = Expand((v >> 10) & 31, 5); o[3] = (v >> 15) ? 255 : 0; } };
        case TextureFormat::k_4_4_4_4:
            return { MTLPixelFormatRGBA8Unorm, [](uint32_t v, uint8_t* o) {
                for (int i = 0; i < 4; i++) o[i] = Expand((v >> (i * 4)) & 15, 4); } };
        default:
            return {};
        }
    }

    static MTLTextureSwizzle Swizzle(uint32_t code)
    {
        switch (code)
        {
        case 0: return MTLTextureSwizzleRed;
        case 1: return MTLTextureSwizzleGreen;
        case 2: return MTLTextureSwizzleBlue;
        case 3: return MTLTextureSwizzleAlpha;
        case 4: return MTLTextureSwizzleZero;
        default: return MTLTextureSwizzleOne;
        }
    }

    static void ReportOnce(const std::string& message)
    {
        static std::unordered_map<std::string, bool> reported;
        if (!reported[message])
        {
            reported[message] = true;
            Log("Textures: {}", message);
        }
    }

    // Reads the base level as host-order blocks, row by row. Returns false when the guest data still
    // hashes to `hash`, updating it otherwise.
    static bool Untile(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t width, uint32_t height,
                       uint32_t basePage, std::vector<uint8_t>& blocks, uint32_t& rowBytes, uint64_t& hash)
    {
        TextureFormat format = fetch.format;
        const FormatInfo* info = FormatInfo::Get(uint32_t(format));
        const uint32_t bpb = info->bytes_per_block();
        const uint32_t bpbLog2 = xe::log2_floor(bpb);
        const uint32_t blocksX = (width + info->block_width - 1) / info->block_width;
        const uint32_t blocksY = (height + info->block_height - 1) / info->block_height;

        texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
            xenos::DataDimension::k2DOrStacked, fetch.pitch, width, height, 1, fetch.tiled, format,
            fetch.packed_mips, true, 0);
        uint32_t offsetX = 0, offsetY = 0, offsetZ = 0;
        if (fetch.packed_mips && layout.packed_level == 0)
            texture_util::GetPackedMipOffset(width, height, 1, format, 0, offsetX, offsetY, offsetZ);

        const uint8_t* base = TranslatePhysical(basePage << 12);
        const uint32_t extent = layout.base.level_data_extent_bytes;
        // Unwritten since the last check: nothing to hash. Otherwise watch first, so a write racing with
        // the hash below marks the pages dirty again.
        if (hash && !IsPhysicalDirty(basePage << 12, extent))
            return false;
        // Streaming textures: rewritten within the last few frames, such as Bink video frames. The GPU reads
        // them as of the kick (see MarkGpuRead).
        static std::unordered_map<uint32_t, uint64_t> lastChange; // base page -> frame of its last change
        const uint64_t frame = FrameCount();
        auto recent = lastChange.find(basePage);
        const bool streaming = recent != lastChange.end() && frame - recent->second <= 10;
        WatchPhysical(basePage << 12, extent, streaming);
        uint64_t newHash = XXH3_64bits(base, extent);
        // A streaming texture may also be drawn while its writer is still filling it: Bink's async decode
        // thread writes a frame in bursts and the title draws it without waiting. A read mid-decode
        // uploads rows just zeroed (dcbz) but not yet filled. So wait until the thread that last wrote the
        // texture is blocked in a kernel wait and the memory has been quiet for 3 ms (the decoder also
        // blocks briefly mid-frame, and pauses between bursts), at most 50 ms; then read again.
        const uint32_t lastWriter = PhysicalWriter(basePage << 12, extent);
        if (newHash != hash && (streaming || IsPhysicalDirty(basePage << 12, extent) ||
                                (lastWriter && IsThreadRunning(lastWriter))))
        {
            using Clock = std::chrono::steady_clock;
            // The texture was written since the last read, so its writer must finish: stop holding writers (see
            // MarkGpuRead) meanwhile. (One held at its first write left the texture clean, read above.)
            g_gpuAwaitingWriters = true;
            const auto start = Clock::now();
            auto quietSince = start;
            while (Clock::now() - start < std::chrono::milliseconds(50))
            {
                if (IsPhysicalDirty(basePage << 12, extent))
                {
                    WatchPhysical(basePage << 12, extent, streaming);
                    quietSince = Clock::now();
                }
                const uint32_t writer = PhysicalWriter(basePage << 12, extent);
                if (Clock::now() - quietSince >= std::chrono::milliseconds(3) && !(writer && IsThreadRunning(writer)))
                    break;
                std::this_thread::sleep_for(std::chrono::microseconds(250));
            }
            g_gpuAwaitingWriters = false;
            g_settleWait += Clock::now() - start;
            newHash = XXH3_64bits(base, extent);
        }
        if (newHash != hash)
            lastChange[basePage] = frame;
        if (streaming || (hash && newHash != hash))
            MarkGpuRead(basePage << 12, extent);
        if (newHash == hash)
            return false;
        hash = newHash;

        rowBytes = blocksX * bpb;
        blocks.resize(size_t(rowBytes) * blocksY);
        const uint32_t pitchBlocks = layout.base.row_pitch_bytes >> bpbLog2;
        for (uint32_t y = 0; y < blocksY; y++)
        {
            for (uint32_t x = 0; x < blocksX; x++)
            {
                uint32_t sx = x + offsetX, sy = y + offsetY;
                uint32_t offset = fetch.tiled ? uint32_t(texture_address::Tiled2D(int32_t(sx), int32_t(sy), pitchBlocks, bpbLog2))
                                              : sy * layout.base.row_pitch_bytes + (sx << bpbLog2);
                if (offset + bpb > extent)
                    continue;
                uint8_t* out = &blocks[size_t(y) * rowBytes + size_t(x) * bpb];
                if (bpb >= 4)
                {
                    for (uint32_t i = 0; i < bpb; i += 4)
                    {
                        uint32_t word;
                        memcpy(&word, base + offset + i, 4);
                        word = SwapWord(word, fetch.endianness);
                        memcpy(out + i, &word, 4);
                    }
                }
                else
                {
                    // Swapping applies to aligned 32-bit words of memory.
                    uint32_t word;
                    memcpy(&word, base + (offset & ~3u), 4);
                    word = SwapWord(word, fetch.endianness) >> ((offset & 3) * 8);
                    memcpy(out, &word, bpb);
                }
            }
        }
        return true;
    }

    static MTLTextureSwizzleChannels Swizzles(uint32_t swizzle)
    {
        return MTLTextureSwizzleChannelsMake(Swizzle(swizzle & 7), Swizzle((swizzle >> 3) & 7), Swizzle((swizzle >> 6) & 7),
                                             Swizzle((swizzle >> 9) & 7));
    }

    // A fetch of resolved memory: the resolve's GPU copy when it is the same image, otherwise wait
    // for the resolve to land in guest memory. Returns nil to continue with guest memory.
    static id<MTLTexture> FromResolve(const xenos::xe_gpu_texture_fetch_t& fetch)
    {
        const uint32_t base = fetch.base_address << 12;
        const uint32_t bytes = xe::align(uint32_t(fetch.size_2d.width) + 1, 32u) * xe::align(uint32_t(fetch.size_2d.height) + 1, 32u) *
                               FormatInfo::Get(uint32_t(fetch.format))->bits_per_pixel / 8;
        for (auto& [address, r] : g_resolved)
        {
            if (base >= address + r.bytes || address >= base + bytes)
                continue;
            const bool same = address == base && fetch.tiled && fetch.dimension == xenos::DataDimension::k2DOrStacked &&
                              (fetch.format == TextureFormat::k_8_8_8_8 || fetch.format == TextureFormat::k_8_8_8_8_A) &&
                              fetch.size_2d.width + 1 == r.width && fetch.size_2d.height + 1 == r.height;
            if (!same)
            {
                // Read from guest memory, once the GPU has written the resolve there.
                if (g_resolvePending && g_sync)
                    g_sync();
                g_resolvePending = false;
                return nil;
            }
            const bool gamma = fetch.sign_x == xenos::TextureSign::kGamma;
            __strong id<MTLTexture>& view = r.views[fetch.swizzle | (gamma ? 0x10000 : 0)];
            if (!view)
                view = [r.texture newTextureViewWithPixelFormat:gamma ? MTLPixelFormatRGBA8Unorm_sRGB : MTLPixelFormatRGBA8Unorm
                                                    textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)
                                                        swizzle:Swizzles(fetch.swizzle)];
            return view;
        }
        return nil;
    }

    id<MTLTexture> GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch)
    {
        if (id<MTLTexture> resolved = FromResolve(fetch))
            return resolved;

        uint64_t key = XXH3_64bits(&fetch, sizeof(fetch));
        Entry& entry = g_textures[key];
        if (entry.texture && entry.checkedGeneration == g_generation)
            return entry.texture;
        entry.checkedGeneration = g_generation;

        if (fetch.type != xenos::FetchConstantType::kTexture || fetch.dimension != xenos::DataDimension::k2DOrStacked || fetch.stacked)
        {
            ReportOnce(fmt::format("unsupported fetch constant type {} dimension {} stacked {}", uint32_t(fetch.type),
                                   uint32_t(fetch.dimension), uint32_t(fetch.stacked)));
            return nil;
        }
        const bool gamma = fetch.sign_x == xenos::TextureSign::kGamma;
        if ((fetch.sign_x != xenos::TextureSign::kUnsigned && !gamma) || fetch.exp_adjust != 0)
            ReportOnce(fmt::format("format {} uses sign {} / exp adjust {} (ignored)", uint32_t(fetch.format), uint32_t(fetch.sign_x), int(fetch.exp_adjust)));
        HostFormat host = GetHostFormat(fetch.format, gamma);
        if (host.pixel == MTLPixelFormatInvalid)
        {
            ReportOnce(fmt::format("unsupported format {}", FormatInfo::Get(uint32_t(fetch.format))->name));
            return nil;
        }

        uint32_t widthMinus1, heightMinus1, depthMinus1, basePage, mipPage, mipMin, mipMax;
        texture_util::GetSubresourcesFromFetchConstant(fetch, &widthMinus1, &heightMinus1, &depthMinus1, &basePage,
                                                       &mipPage, &mipMin, &mipMax);
        if (!basePage)
            return nil;
        if (mipMax > mipMin && fetch.mip_filter != xenos::TextureFilter::kBaseMap)
            ReportOnce(fmt::format("textures with mip levels are used (format {}, {}x{}, levels {}-{}); only the base level is uploaded",
                                   FormatInfo::Get(uint32_t(fetch.format))->name, widthMinus1 + 1, heightMinus1 + 1, mipMin, mipMax));
        const uint32_t width = widthMinus1 + 1, height = heightMinus1 + 1;

        std::vector<uint8_t> blocks;
        uint32_t rowBytes;
        if (!Untile(fetch, width, height, basePage, blocks, rowBytes, entry.dataHash) && entry.texture)
            return entry.texture;

        if (host.expand)
        {
            // 16-bit packed formats become RGBA8.
            std::vector<uint8_t> rgba(size_t(width) * height * 4);
            for (size_t i = 0; i < size_t(width) * height; i++)
            {
                uint16_t v;
                memcpy(&v, &blocks[i * 2], 2);
                host.expand(v, &rgba[i * 4]);
            }
            blocks.swap(rgba);
            rowBytes = width * 4;
        }

        // New contents get a new texture: draws already encoded (not yet run by the GPU) still sample the old
        // one, which their command buffers keep alive. Rewriting it in place tore videos (Bink rewrites its
        // frames every frame) into rows of old and new data.
        // Note: one allocation per changed texture; pool them if allocation shows up in profiles.
        {
            MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:host.pixel width:width height:height mipmapped:NO];
            desc.usage = MTLTextureUsageShaderRead;
            desc.storageMode = MTLStorageModeShared;
            desc.swizzle = Swizzles(fetch.swizzle);
            entry.texture = [g_device newTextureWithDescriptor:desc];
        }
        // Note: base level only, mips are dropped; upload them if a title relies on minification.
        g_uploads++;
        [entry.texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:blocks.data() bytesPerRow:rowBytes];
        return entry.texture;
    }

    static MTLSamplerAddressMode AddressMode(xenos::ClampMode mode)
    {
        switch (mode)
        {
        case xenos::ClampMode::kRepeat: return MTLSamplerAddressModeRepeat;
        case xenos::ClampMode::kMirroredRepeat: return MTLSamplerAddressModeMirrorRepeat;
        case xenos::ClampMode::kClampToBorder: return MTLSamplerAddressModeClampToBorderColor;
        case xenos::ClampMode::kMirrorClampToEdge:
        case xenos::ClampMode::kMirrorClampToHalfway:
        case xenos::ClampMode::kMirrorClampToBorder:
            return MTLSamplerAddressModeMirrorClampToEdge;
        default: return MTLSamplerAddressModeClampToEdge;
        }
    }

    id<MTLSamplerState> GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch)
    {
        uint64_t key = uint64_t(fetch.clamp_x) | uint64_t(fetch.clamp_y) << 3 | uint64_t(fetch.clamp_z) << 6 |
                       uint64_t(fetch.mag_filter) << 9 | uint64_t(fetch.min_filter) << 11 | uint64_t(fetch.border_color) << 13;
        __strong id<MTLSamplerState>& sampler = g_samplers[key];
        if (!sampler)
        {
            MTLSamplerDescriptor* desc = [MTLSamplerDescriptor new];
            desc.sAddressMode = AddressMode(fetch.clamp_x);
            desc.tAddressMode = AddressMode(fetch.clamp_y);
            desc.rAddressMode = AddressMode(fetch.clamp_z);
            desc.magFilter = fetch.mag_filter == xenos::TextureFilter::kPoint ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
            desc.minFilter = fetch.min_filter == xenos::TextureFilter::kPoint ? MTLSamplerMinMagFilterNearest : MTLSamplerMinMagFilterLinear;
            desc.borderColor = fetch.border_color == xenos::BorderColor::k_ABGR_White ? MTLSamplerBorderColorOpaqueWhite
                               : fetch.border_color == xenos::BorderColor::k_ABGR_Black ? MTLSamplerBorderColorOpaqueBlack
                                                                                        : MTLSamplerBorderColorTransparentBlack;
            sampler = [g_device newSamplerStateWithDescriptor:desc];
        }
        return sampler;
    }
}
