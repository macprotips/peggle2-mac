// Metal backend for the emulated GPU. EDRAM render targets are host textures keyed by EDRAM base,
// pitch and format; resolves write tiled data back to guest memory, so textures, the front buffer and
// CPU readers all see what the hardware would. Draw state follows Xenia (BSD-3-Clause).
#include "renderer.h"
#include "../settings.h"
#include "gpu.h"
#include "msl_prelude.h"
#include "msl_translator.h"
#include "textures.h"
#include "../log.h"
#include "../memory.h"
#include "../platform/app.h"
#include "xenia/base/math.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/ui/graphics_util.h"
#define XXH_INLINE_ALL
#include "xxhash.h"
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <chrono>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rt::gpu
{
    using namespace xe::gpu;

    // Mirrors XeDraw in msl_prelude.h.
    struct XeDraw
    {
        float ndcScale[4];
        float ndcOffset[4];
        uint32_t indexed, indexBase, indexFormat, indexEndian;
        int32_t indexOffset;
        uint32_t flags, primitive, alphaFunc;
        float alphaRef;
        float pixelScale;
        uint32_t pad[2];
    };
    static_assert(sizeof(XeDraw) == 80);

    struct CompiledShader
    {
        ShaderInfo info;
        id<MTLLibrary> library;
        std::unordered_map<uint32_t, id<MTLFunction>> functions; // by interpolants | alpha function << 16
    };

    // The shader's entry point specialized for a pipeline (see the function constants in msl_prelude.h).
    static id<MTLFunction> Specialize(CompiledShader* shader, uint32_t interpolants, uint32_t alphaFunc)
    {
        __strong id<MTLFunction>& function = shader->functions[interpolants | alphaFunc << 16];
        if (!function)
        {
            MTLFunctionConstantValues* values = [MTLFunctionConstantValues new];
            [values setConstantValue:&interpolants type:MTLDataTypeUInt atIndex:0];
            [values setConstantValue:&alphaFunc type:MTLDataTypeUInt atIndex:1];
            NSError* error = nil;
            function = [shader->library newFunctionWithName:shader->info.isPixel ? @"xe_fragment" : @"xe_vertex" constantValues:values error:&error];
            if (!function)
                Log("Renderer: specializing a shader failed: {}", error.localizedDescription.UTF8String);
        }
        return function;
    }

    static id<MTLDevice> g_device;
    static id<MTLCommandQueue> g_queue;
    static id<MTLBuffer> g_memory; // all of guest physical memory
    static id<MTLCommandBuffer> g_commands;
    static id<MTLRenderCommandEncoder> g_encoder;
    static id<MTLTexture> g_passColors[4];
    static id<MTLTexture> g_passDepth;
    static uint32_t g_passWidth, g_passHeight;
    static id<MTLTexture> g_dummyTexture;
    static id<MTLRenderPipelineState> g_presentPipeline;

    static std::unordered_map<uint64_t, CompiledShader> g_shaders;
    static CompiledShader* g_activeShaders[2]; // vertex, pixel
    static std::unordered_map<uint64_t, id<MTLTexture>> g_targets;
    // Render targets hold this many host pixels per guest pixel in each direction (RenderScale, applied
    // between frames). Resolves still write guest memory at the console's resolution.
    static uint32_t g_scale = 1;
    // Depth clears wait for the next pass over the target, whose render area covers their rectangle.
    struct DepthClear
    {
        uint32_t value; // D24S8
        uint32_t right, bottom;
    };
    static std::unordered_map<void*, DepthClear> g_pendingDepthClears;
    // The area each target has been drawn in. Targets span the rest of EDRAM, far taller than what the
    // title draws, and a pass loads and stores its whole render area.
    static std::unordered_map<void*, std::pair<uint32_t, uint32_t>> g_drawnExtents;
    static std::unordered_map<uint64_t, id<MTLRenderPipelineState>> g_pipelines;
    static std::unordered_map<uint64_t, id<MTLDepthStencilState>> g_depthStates;

    // PEGGLE2_GPU_STATS: per-second counts of what the renderer did with the guest's work.
    static const bool g_stats = getenv("PEGGLE2_GPU_STATS") != nullptr;
    static struct
    {
        uint32_t draws, drawn, noShader, noTarget, scissored, resolves, copies, swaps, textures, missingTextures, passes, buffers, waits;
    } g_count;
    static std::atomic<uint64_t> g_gpuNanos; // GPU execution time of finished command buffers

    // PEGGLE2_TRACE_FRAME=n: logs every draw of frame n and dumps its textures to out/trace.
    static const uint64_t g_traceFrame = getenv("PEGGLE2_TRACE_FRAME") ? strtoull(getenv("PEGGLE2_TRACE_FRAME"), nullptr, 0) : ~0ull;
    static bool Tracing() { return FrameCount() == g_traceFrame; }
    static void WritePng(id<MTLTexture> texture, const std::string& path);

    static void ReportOnce(const std::string& message)
    {
        static std::unordered_map<std::string, bool> reported;
        if (!reported[message])
        {
            reported[message] = true;
            Log("Renderer: {}", message);
        }
    }

    // --- Command submission --------------------------------------------------------------------

    static id<MTLCommandBuffer> Commands()
    {
        if (!g_commands)
            g_commands = [g_queue commandBuffer];
        return g_commands;
    }

    static void EndPass()
    {
        if (g_encoder)
        {
            [g_encoder endEncoding];
            g_encoder = nil;
        }
    }

    static void Submit(bool wait)
    {
        EndPass();
        if (!g_commands)
            return;
        if (g_stats)
            [g_commands addCompletedHandler:^(id<MTLCommandBuffer> b) { g_gpuNanos += uint64_t((b.GPUEndTime - b.GPUStartTime) * 1e9); }];
        g_count.buffers++;
        g_count.waits += wait;
        [g_commands commit];
        if (wait)
            [g_commands waitUntilCompleted];
        g_commands = nil;
    }

    // Before the guest can observe GPU progress (fences, interrupts), the work must really be done:
    // the title reuses vertex and constant memory once a fence passes.
    static void Sync()
    {
        @autoreleasepool
        {
            Submit(true);
        }
    }

    // --- Shaders -------------------------------------------------------------------------------

    static void LoadShader(const ShaderLoad& load)
    {
        const bool pixel = load.kind == 1;
        uint64_t hash = XXH3_64bits(load.dwords, load.count * 4) + load.kind;
        auto [it, inserted] = g_shaders.try_emplace(hash);
        CompiledShader& compiled = it->second;
        g_activeShaders[pixel] = &compiled;
        if (!inserted)
            return;

        @autoreleasepool
        {
            xe::gpu::Shader shader(pixel ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, hash, load.dwords, load.count);
            xe::StringBuffer disassembly;
            shader.AnalyzeUcode(disassembly);
            MslTranslator translator;
            std::string body = translator.Translate(shader, compiled.info);
            if (!compiled.info.errors.empty())
                Log("Renderer: shader {:016X} translation errors:\n{}", hash, compiled.info.errors);
            std::string source = std::string(kMslPrelude) + body + (pixel ? FragmentEntry(compiled.info) : kMslVertexEntry);

            MTLCompileOptions* options = [MTLCompileOptions new];
            // IEEE-conformant maths, as the console's shader units compute.
            if (@available(macOS 15.0, *))
                options.mathMode = MTLMathModeSafe;
            else
            {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                options.fastMathEnabled = NO;
#pragma clang diagnostic pop
            }
            NSError* error = nil;
            id<MTLLibrary> library = [g_device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
            if (!library)
            {
                Log("Renderer: shader {:016X} failed to compile: {}", hash, error.localizedDescription.UTF8String);
                return;
            }
            compiled.library = library;
        }
    }

    // --- Render targets ------------------------------------------------------------------------

    static MTLPixelFormat ColorFormat(xenos::ColorRenderTargetFormat format)
    {
        switch (format)
        {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8: return MTLPixelFormatRGBA8Unorm;
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: return MTLPixelFormatRGBA8Unorm_sRGB;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
            return MTLPixelFormatRGB10A2Unorm;
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: return MTLPixelFormatRG16Float;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
            return MTLPixelFormatRGBA16Float;
        case xenos::ColorRenderTargetFormat::k_32_FLOAT: return MTLPixelFormatR32Float;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: return MTLPixelFormatRG32Float;
        default:
            ReportOnce(fmt::format("color render target format {} approximated as RGBA16Float", uint32_t(format)));
            return MTLPixelFormatRGBA16Float;
        }
    }

    // The host texture standing in for an EDRAM surface. It spans from its base to the end of EDRAM.
    static id<MTLTexture> GetTarget(uint32_t baseTiles, uint32_t pitch, uint32_t format, bool depth)
    {
        uint64_t key = uint64_t(baseTiles) | uint64_t(pitch) << 12 | uint64_t(format) << 26 | uint64_t(depth) << 40;
        __strong id<MTLTexture>& texture = g_targets[key];
        if (!texture)
        {
            uint32_t tilesPerRow = std::max(1u, (pitch + xenos::kEdramTileWidthSamples - 1) / xenos::kEdramTileWidthSamples);
            uint32_t rows = (xenos::kEdramTileCount - std::min(baseTiles, xenos::kEdramTileCount - 1)) / tilesPerRow;
            uint32_t height = std::clamp(rows * xenos::kEdramTileHeightSamples, 1u, 2048u) * g_scale;
            uint32_t width = std::max(pitch, 1u) * g_scale;
            MTLPixelFormat pixel = depth ? MTLPixelFormatDepth32Float_Stencil8 : ColorFormat(xenos::ColorRenderTargetFormat(format));
            MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pixel width:width height:height mipmapped:NO];
            desc.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead | (depth ? 0 : MTLTextureUsageShaderWrite);
            desc.storageMode = MTLStorageModePrivate; // dumps stage through a shared copy (WritePng)
            texture = [g_device newTextureWithDescriptor:desc];
            Log("Renderer: {} target at tile {:X}, pitch {}, format {} ({}x{})", depth ? "depth" : "color", baseTiles, pitch, format, width, height);
        }
        return texture;
    }

    // A pass over these targets whose render area reaches (right, bottom).
    static id<MTLRenderCommandEncoder> BeginPass(id<MTLTexture> const colors[4], id<MTLTexture> depth, uint32_t right, uint32_t bottom)
    {
        bool same = g_encoder && depth == g_passDepth && right <= g_passWidth && bottom <= g_passHeight;
        for (int i = 0; i < 4 && same; i++)
            same = colors[i] == g_passColors[i];
        if (same && !(depth && g_pendingDepthClears.count((__bridge void*)depth)))
            return g_encoder;
        EndPass();

        // Start with everything drawn in these targets before, so the area rarely has to grow mid-frame.
        uint32_t maxWidth = UINT32_MAX, maxHeight = UINT32_MAX;
        auto include = [&](id<MTLTexture> texture) {
            auto [w, h] = g_drawnExtents[(__bridge void*)texture];
            right = std::max(right, w);
            bottom = std::max(bottom, h);
            maxWidth = std::min(maxWidth, uint32_t(texture.width));
            maxHeight = std::min(maxHeight, uint32_t(texture.height));
        };
        for (int i = 0; i < 4; i++)
            if (colors[i])
                include(colors[i]);
        if (depth)
        {
            include(depth);
            if (auto pending = g_pendingDepthClears.find((__bridge void*)depth); pending != g_pendingDepthClears.end())
            {
                right = std::max(right, pending->second.right);
                bottom = std::max(bottom, pending->second.bottom);
            }
        }
        right = std::min(right, maxWidth);
        bottom = std::min(bottom, maxHeight);
        for (int i = 0; i < 4; i++)
            if (colors[i])
                g_drawnExtents[(__bridge void*)colors[i]] = { right, bottom };
        if (depth)
            g_drawnExtents[(__bridge void*)depth] = { right, bottom };
        g_passWidth = right;
        g_passHeight = bottom;

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.renderTargetWidth = right;
        pass.renderTargetHeight = bottom;
        for (int i = 0; i < 4; i++)
        {
            g_passColors[i] = colors[i];
            if (!colors[i])
                continue;
            pass.colorAttachments[i].texture = colors[i];
            pass.colorAttachments[i].loadAction = MTLLoadActionLoad;
            pass.colorAttachments[i].storeAction = MTLStoreActionStore;
        }
        g_passDepth = depth;
        if (depth)
        {
            auto pending = g_pendingDepthClears.find((__bridge void*)depth);
            bool clear = pending != g_pendingDepthClears.end();
            uint32_t value = clear ? pending->second.value : 0;
            if (clear)
                g_pendingDepthClears.erase(pending);
            pass.depthAttachment.texture = depth;
            pass.depthAttachment.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.depthAttachment.storeAction = MTLStoreActionStore;
            pass.depthAttachment.clearDepth = double(value >> 8) / double(0xFFFFFF);
            pass.stencilAttachment.texture = depth;
            pass.stencilAttachment.loadAction = clear ? MTLLoadActionClear : MTLLoadActionLoad;
            pass.stencilAttachment.storeAction = MTLStoreActionStore;
            pass.stencilAttachment.clearStencil = value & 0xFF;
        }
        g_count.passes++;
        g_encoder = [Commands() renderCommandEncoderWithDescriptor:pass];
        [g_encoder setVertexBuffer:g_memory offset:0 atIndex:4];
        return g_encoder;
    }

    static uint32_t SwapWord(uint32_t value, uint32_t endian)
    {
        switch (endian & 3)
        {
        case 1: return ((value << 8) & 0xFF00FF00) | ((value >> 8) & 0x00FF00FF);
        case 2: return __builtin_bswap32(value);
        case 3: return (value >> 16) | (value << 16);
        default: return value;
        }
    }

    // Host pixels a draw can touch, when its vertex shader passes screen-space positions through
    // unchanged: the bounds of its vertices. False if they can't be read.
    static bool VertexBounds(const RegisterFile& regs, const ShaderInfo& vs, const DrawRequest& draw, const draw_util::ViewportInfo& viewport,
                             float& x0, float& y0, float& x1, float& y1)
    {
        auto fetch = regs.GetVertexFetch(vs.positionSlot);
        const uint32_t n = draw.indexCount;
        if (fetch.type != xenos::FetchConstantType::kVertex || draw.indexed || n == 0 || n > 64)
            return false;
        const uint32_t* memory = reinterpret_cast<const uint32_t*>(TranslatePhysical(fetch.address * 4));
        const int32_t indexOffset = int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]);
        std::vector<std::pair<float, float>> points; // NDC
        for (uint32_t i = 0; i < n; i++)
        {
            const int64_t element = int64_t(i) + indexOffset;
            const int64_t dword = element * vs.positionStride + vs.positionOffset;
            if (element < 0 || dword + 2 > int64_t(uint32_t(fetch.size)))
                return false;
            float ndc[2];
            for (int c = 0; c < 2; c++)
            {
                const float v = xe::memory::Reinterpret<float>(SwapWord(memory[dword + c], uint32_t(fetch.endian)));
                if (!std::isfinite(v))
                    return false;
                // As the vertex entry computes it with XY already divided by W.
                ndc[c] = v * viewport.ndc_scale[c] + viewport.ndc_offset[c];
            }
            points.emplace_back(ndc[0], ndc[1]);
        }
        // A rectangle's fourth corner, chosen as the vertex entry does: the vertex opposite the longest edge,
        // mirrored across it.
        if (draw.primitiveType == uint32_t(xenos::PrimitiveType::kRectangleList))
        {
            auto d2 = [](std::pair<float, float> a, std::pair<float, float> b) {
                return (a.first - b.first) * (a.first - b.first) + (a.second - b.second) * (a.second - b.second);
            };
            for (size_t i = 0; i + 2 < n; i += 3)
            {
                const auto p0 = points[i], p1 = points[i + 1], p2 = points[i + 2];
                const float e12 = d2(p1, p2), e20 = d2(p2, p0), e01 = d2(p0, p1);
                const auto [s, a, b] = (e12 > e20 && e12 > e01) ? std::tuple(p0, p1, p2) : e20 > e01 ? std::tuple(p1, p2, p0) : std::tuple(p2, p0, p1);
                points.emplace_back(a.first + b.first - s.first, a.second + b.second - s.second);
            }
        }
        // To host pixels (Metal's NDC is +Y up).
        for (auto& [x, y] : points)
        {
            x = viewport.xy_offset[0] + (x * 0.5f + 0.5f) * viewport.xy_extent[0];
            y = viewport.xy_offset[1] + (0.5f - y * 0.5f) * viewport.xy_extent[1];
        }
        x0 = y0 = INFINITY;
        x1 = y1 = -INFINITY;
        for (auto [x, y] : points)
        {
            x0 = std::min(x0, x);
            y0 = std::min(y0, y);
            x1 = std::max(x1, x);
            y1 = std::max(y1, y);
        }
        return true;
    }

    // --- Pipeline state ------------------------------------------------------------------------

    static MTLBlendFactor BlendFactor(xenos::BlendFactor factor)
    {
        switch (factor)
        {
        case xenos::BlendFactor::kZero: return MTLBlendFactorZero;
        case xenos::BlendFactor::kOne: return MTLBlendFactorOne;
        case xenos::BlendFactor::kSrcColor: return MTLBlendFactorSourceColor;
        case xenos::BlendFactor::kOneMinusSrcColor: return MTLBlendFactorOneMinusSourceColor;
        case xenos::BlendFactor::kSrcAlpha: return MTLBlendFactorSourceAlpha;
        case xenos::BlendFactor::kOneMinusSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
        case xenos::BlendFactor::kDstColor: return MTLBlendFactorDestinationColor;
        case xenos::BlendFactor::kOneMinusDstColor: return MTLBlendFactorOneMinusDestinationColor;
        case xenos::BlendFactor::kDstAlpha: return MTLBlendFactorDestinationAlpha;
        case xenos::BlendFactor::kOneMinusDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
        case xenos::BlendFactor::kConstantColor: return MTLBlendFactorBlendColor;
        case xenos::BlendFactor::kOneMinusConstantColor: return MTLBlendFactorOneMinusBlendColor;
        case xenos::BlendFactor::kConstantAlpha: return MTLBlendFactorBlendAlpha;
        case xenos::BlendFactor::kOneMinusConstantAlpha: return MTLBlendFactorOneMinusBlendAlpha;
        case xenos::BlendFactor::kSrcAlphaSaturate: return MTLBlendFactorSourceAlphaSaturated;
        default: return MTLBlendFactorZero;
        }
    }

    static MTLBlendOperation BlendOp(xenos::BlendOp op)
    {
        switch (op)
        {
        case xenos::BlendOp::kSubtract: return MTLBlendOperationSubtract;
        case xenos::BlendOp::kMin: return MTLBlendOperationMin;
        case xenos::BlendOp::kMax: return MTLBlendOperationMax;
        case xenos::BlendOp::kRevSubtract: return MTLBlendOperationReverseSubtract;
        default: return MTLBlendOperationAdd;
        }
    }

    static id<MTLRenderPipelineState> GetPipeline(CompiledShader* vs, CompiledShader* ps, id<MTLTexture> const colors[4],
                                                  id<MTLTexture> depth, const uint32_t blend[4], uint32_t colorMask, uint32_t alphaFunc)
    {
        struct
        {
            void* vs;
            void* ps;
            uint32_t formats[5];
            uint32_t blend[4];
            uint32_t colorMask;
            uint32_t alphaFunc;
        } key{ vs, ps, {}, {}, colorMask, alphaFunc };
        for (int i = 0; i < 4; i++)
        {
            key.formats[i] = colors[i] ? uint32_t(colors[i].pixelFormat) : 0;
            key.blend[i] = colors[i] ? blend[i] : 0;
        }
        key.formats[4] = depth ? uint32_t(depth.pixelFormat) : 0;
        uint64_t hash = XXH3_64bits(&key, sizeof(key));
        __strong id<MTLRenderPipelineState>& pipeline = g_pipelines[hash];
        static std::unordered_set<uint64_t> failed; // reported once, not retried every draw
        if (pipeline || failed.count(hash))
            return pipeline;

        MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
        const uint32_t interpolants = vs->info.interpolators & ps->info.inputsRead;
        desc.vertexFunction = Specialize(vs, interpolants, alphaFunc);
        desc.fragmentFunction = Specialize(ps, interpolants, alphaFunc);
        if (!desc.vertexFunction || !desc.fragmentFunction)
        {
            failed.insert(hash);
            return nil;
        }
        for (int i = 0; i < 4; i++)
        {
            if (!colors[i])
                continue;
            auto* attachment = desc.colorAttachments[i];
            attachment.pixelFormat = colors[i].pixelFormat;
            attachment.writeMask = ((colorMask >> (i * 4)) & 1 ? MTLColorWriteMaskRed : 0) | ((colorMask >> (i * 4)) & 2 ? MTLColorWriteMaskGreen : 0) |
                                   ((colorMask >> (i * 4)) & 4 ? MTLColorWriteMaskBlue : 0) | ((colorMask >> (i * 4)) & 8 ? MTLColorWriteMaskAlpha : 0);
            reg::RB_BLENDCONTROL b;
            b.value = blend[i];
            bool opaque = b.color_srcblend == xenos::BlendFactor::kOne && b.color_destblend == xenos::BlendFactor::kZero &&
                          b.color_comb_fcn == xenos::BlendOp::kAdd && b.alpha_srcblend == xenos::BlendFactor::kOne &&
                          b.alpha_destblend == xenos::BlendFactor::kZero && b.alpha_comb_fcn == xenos::BlendOp::kAdd;
            attachment.blendingEnabled = !opaque;
            attachment.sourceRGBBlendFactor = BlendFactor(b.color_srcblend);
            attachment.destinationRGBBlendFactor = BlendFactor(b.color_destblend);
            attachment.rgbBlendOperation = BlendOp(b.color_comb_fcn);
            attachment.sourceAlphaBlendFactor = BlendFactor(b.alpha_srcblend);
            attachment.destinationAlphaBlendFactor = BlendFactor(b.alpha_destblend);
            attachment.alphaBlendOperation = BlendOp(b.alpha_comb_fcn);
        }
        if (depth)
        {
            desc.depthAttachmentPixelFormat = depth.pixelFormat;
            desc.stencilAttachmentPixelFormat = depth.pixelFormat;
        }
        NSError* error = nil;
        pipeline = [g_device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!pipeline)
        {
            failed.insert(hash);
            Log("Renderer: pipeline creation failed: {}", error.localizedDescription.UTF8String);
        }
        return pipeline;
    }

    static id<MTLDepthStencilState> GetDepthState(reg::RB_DEPTHCONTROL control, uint32_t refMask, uint32_t refMaskBack)
    {
        // Stencil masks (bits 8-23 of each ref/mask register) do not fit beside the control word in 64 bits.
        const uint32_t parts[3] = { control.value, refMask & 0xFFFF00, refMaskBack & 0xFFFF00 };
        const uint64_t key = XXH3_64bits(parts, sizeof(parts));
        __strong id<MTLDepthStencilState>& state = g_depthStates[key];
        if (state)
            return state;
        MTLDepthStencilDescriptor* desc = [MTLDepthStencilDescriptor new];
        if (control.z_enable)
        {
            desc.depthCompareFunction = MTLCompareFunction(control.zfunc);
            desc.depthWriteEnabled = control.z_write_enable;
        }
        if (control.stencil_enable)
        {
            reg::RB_STENCILREFMASK front, back;
            front.value = refMask;
            back.value = control.backface_enable ? refMaskBack : refMask;
            MTLStencilDescriptor* f = [MTLStencilDescriptor new];
            f.stencilCompareFunction = MTLCompareFunction(control.stencilfunc);
            f.stencilFailureOperation = MTLStencilOperation(control.stencilfail);
            f.depthFailureOperation = MTLStencilOperation(control.stencilzfail);
            f.depthStencilPassOperation = MTLStencilOperation(control.stencilzpass);
            f.readMask = front.stencilmask;
            f.writeMask = front.stencilwritemask;
            MTLStencilDescriptor* b = [MTLStencilDescriptor new];
            b.stencilCompareFunction = MTLCompareFunction(control.backface_enable ? control.stencilfunc_bf : control.stencilfunc);
            b.stencilFailureOperation = MTLStencilOperation(control.backface_enable ? control.stencilfail_bf : control.stencilfail);
            b.depthFailureOperation = MTLStencilOperation(control.backface_enable ? control.stencilzfail_bf : control.stencilzfail);
            b.depthStencilPassOperation = MTLStencilOperation(control.backface_enable ? control.stencilzpass_bf : control.stencilzpass);
            b.readMask = back.stencilmask;
            b.writeMask = back.stencilwritemask;
            desc.frontFaceStencil = f;
            desc.backFaceStencil = b;
        }
        state = [g_device newDepthStencilStateWithDescriptor:desc];
        return state;
    }

    // --- Draws ---------------------------------------------------------------------------------

    static void Resolve();

    static void Draw(const DrawRequest& draw)
    {
        const RegisterFile& regs = Registers();
        auto mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
        if (mode == xenos::EdramMode::kCopy)
        {
            Resolve();
            return;
        }
        // Depth-only draws (the title clears depth and stencil with one every frame) write no color; as
        // in Xenia, the pixel shader's color outputs are ignored.
        const bool depthOnly = mode == xenos::EdramMode::kDepthOnly;
        if (mode != xenos::EdramMode::kColorDepth && !depthOnly)
        {
            ReportOnce(fmt::format("skipping draws in EDRAM mode {}", uint32_t(mode)));
            return;
        }
        CompiledShader* vs = g_activeShaders[0];
        CompiledShader* ps = g_activeShaders[1];
        g_count.draws++;
        if (!vs || !vs->library || !ps || !ps->library)
        {
            g_count.noShader++;
            return;
        }

        MTLPrimitiveType type;
        uint32_t count = draw.indexCount;
        switch (xenos::PrimitiveType(draw.primitiveType))
        {
        case xenos::PrimitiveType::kPointList: type = MTLPrimitiveTypePoint; break;
        case xenos::PrimitiveType::kLineList: type = MTLPrimitiveTypeLine; break;
        case xenos::PrimitiveType::kLineStrip: type = MTLPrimitiveTypeLineStrip; break;
        case xenos::PrimitiveType::kTriangleList: type = MTLPrimitiveTypeTriangle; break;
        case xenos::PrimitiveType::kTriangleStrip: type = MTLPrimitiveTypeTriangleStrip; break;
        case xenos::PrimitiveType::kTriangleFan: type = MTLPrimitiveTypeTriangle; count = count >= 3 ? (count - 2) * 3 : 0; break;
        case xenos::PrimitiveType::kRectangleList: type = MTLPrimitiveTypeTriangle; count = count / 3 * 6; break;
        case xenos::PrimitiveType::kQuadList: type = MTLPrimitiveTypeTriangle; count = count / 4 * 6; break;
        default:
            ReportOnce(fmt::format("unsupported primitive type {}", draw.primitiveType));
            return;
        }
        if (!count)
            return;

        auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
        const bool polygonal = type == MTLPrimitiveTypeTriangle || type == MTLPrimitiveTypeTriangleStrip;
        if (polygonal && sc.cull_front && sc.cull_back)
            return;
        if (sc.multi_prim_ib_ena && draw.indexed && type != MTLPrimitiveTypeTriangle && type != MTLPrimitiveTypePoint && type != MTLPrimitiveTypeLine)
            ReportOnce("primitive reset in strips is not supported");

        auto surface = regs.Get<reg::RB_SURFACE_INFO>();
        if (surface.msaa_samples != xenos::MsaaSamples::k1X)
            ReportOnce("MSAA render targets are drawn without multisampling");
        const uint32_t pitch = surface.surface_pitch;
        const uint32_t colorMask = regs[XE_GPU_REG_RB_COLOR_MASK];
        id<MTLTexture> colors[4] = {};
        uint32_t blend[4] = {};
        id<MTLTexture> size = nil;
        for (uint32_t i = 0; i < 4; i++)
        {
            if (depthOnly || !(ps->info.colorTargets & (1u << i)) || !((colorMask >> (i * 4)) & 0xF))
                continue;
            auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
            colors[i] = GetTarget(info.color_base, pitch, uint32_t(info.color_format), false);
            blend[i] = regs[reg::RB_BLENDCONTROL::rt_register_indices[i]];
            size = size ? size : colors[i];
        }
        reg::RB_DEPTHCONTROL depthControl = draw_util::GetNormalizedDepthControl(regs);
        id<MTLTexture> depth = nil;
        if (depthControl.z_enable || depthControl.stencil_enable)
        {
            auto info = regs.Get<reg::RB_DEPTH_INFO>();
            depth = GetTarget(info.depth_base, pitch, uint32_t(info.depth_format), true);
            size = size ? size : depth;
        }
        if (!size)
        {
            g_count.noTarget++;
            return;
        }
        const uint32_t width = uint32_t(size.width), height = uint32_t(size.height);

        draw_util::Scissor scissor;
        draw_util::GetScissor(regs, scissor);
        uint32_t sx = std::min(scissor.offset[0] * g_scale, width), sy = std::min(scissor.offset[1] * g_scale, height);
        uint32_t sw = std::min(scissor.extent[0] * g_scale, width - sx), sh = std::min(scissor.extent[1] * g_scale, height - sy);
        if (!sw || !sh)
        {
            g_count.scissored++;
            return;
        }

        draw_util::ViewportInfo viewport;
        // Metal NDC has +Y up, like Direct3D 12.
        draw_util::GetHostViewportInfo(regs, g_scale, g_scale, true, width, height, false, depthControl, false, false,
                                       ps->info.writesDepth, viewport);

        // Clear rectangles come with a scissor over all of EDRAM, and a pass loads and stores its whole area:
        // narrow the scissor to the vertices when they are known to be where the draw lands.
        float bx0, by0, bx1, by1;
        if (vs->info.positionStride && regs.Get<reg::PA_CL_VTE_CNTL>().vtx_xy_fmt && polygonal &&
            VertexBounds(regs, vs->info, draw, viewport, bx0, by0, bx1, by1))
        {
            // A pixel of margin for rounding and pixel centres.
            const int64_t left = std::max<int64_t>(sx, int64_t(std::floor(bx0)) - g_scale);
            const int64_t top = std::max<int64_t>(sy, int64_t(std::floor(by0)) - g_scale);
            const int64_t right = std::min<int64_t>(sx + sw, int64_t(std::ceil(bx1)) + g_scale);
            const int64_t bottom = std::min<int64_t>(sy + sh, int64_t(std::ceil(by1)) + g_scale);
            if (left >= right || top >= bottom)
            {
                g_count.scissored++;
                return;
            }
            sx = uint32_t(left);
            sy = uint32_t(top);
            sw = uint32_t(right - left);
            sh = uint32_t(bottom - top);
        }

        // Look textures up before encoding: one may wait for a resolve, which ends the current pass.
        id<MTLTexture> textures[32];
        id<MTLSamplerState> samplers[32];
        float textureScales[32];
        uint32_t textureCount = 0;
        for (uint32_t t = 0; t < 32; t++)
        {
            if (!(ps->info.textureSlots & (1u << t)))
                continue;
            auto fetch = regs.GetTextureFetch(t);
            id<MTLTexture> texture = GetTexture(fetch);
            g_count.textures++;
            g_count.missingTextures += !texture;
            if (Tracing())
            {
                Log("  t{}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} -> {}", t, fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3,
                    fetch.dword_4, fetch.dword_5, texture ? fmt::format("{}x{} format {}", texture.width, texture.height, uint32_t(texture.pixelFormat)) : "none");
                if (texture)
                    WritePng(texture, fmt::format("out/trace/draw{}_t{}.png", g_count.draws, t));
            }
            // A scaled resolve copy has more texels than the guest texture it stands for.
            const float scale = texture ? float(texture.width) / float(fetch.size_2d.width + 1) : 1.0f;
            textureScales[t] = scale >= 1.5f ? std::round(scale) : 1.0f;
            textures[textureCount] = texture ? texture : g_dummyTexture;
            samplers[textureCount] = GetSampler(fetch);
            textureCount++;
        }

        auto colorControl = regs.Get<reg::RB_COLORCONTROL>();
        const uint32_t alphaFunc = colorControl.alpha_test_enable ? uint32_t(colorControl.alpha_func) : 7u;
        id<MTLRenderPipelineState> pipeline = GetPipeline(vs, ps, colors, depth, blend, colorMask, alphaFunc);
        if (!pipeline)
            return;
        id<MTLRenderCommandEncoder> encoder = BeginPass(colors, depth, sx + sw, sy + sh);
        [encoder setRenderPipelineState:pipeline];
        [encoder setViewport:{ double(viewport.xy_offset[0]), double(viewport.xy_offset[1]), double(viewport.xy_extent[0]),
                               double(viewport.xy_extent[1]), double(viewport.z_min), double(viewport.z_max) }];
        [encoder setScissorRect:{ sx, sy, sw, sh }];
        [encoder setCullMode:!polygonal ? MTLCullModeNone : sc.cull_front ? MTLCullModeFront : sc.cull_back ? MTLCullModeBack : MTLCullModeNone];
        [encoder setFrontFacingWinding:sc.face ? MTLWindingClockwise : MTLWindingCounterClockwise];
        if (depth)
        {
            uint32_t refMask = regs[XE_GPU_REG_RB_STENCILREFMASK];
            uint32_t refMaskBack = regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
            [encoder setDepthStencilState:GetDepthState(depthControl, refMask, refMaskBack)];
            [encoder setStencilFrontReferenceValue:refMask & 0xFF
                                backReferenceValue:(depthControl.backface_enable ? refMaskBack : refMask) & 0xFF];
        }
        auto f = [&](uint32_t index) { return xe::memory::Reinterpret<float>(regs[index]); };
        [encoder setBlendColorRed:f(XE_GPU_REG_RB_BLEND_RED) green:f(XE_GPU_REG_RB_BLEND_GREEN) blue:f(XE_GPU_REG_RB_BLEND_BLUE)
                            alpha:f(XE_GPU_REG_RB_BLEND_ALPHA)];

        auto vte = regs.Get<reg::PA_CL_VTE_CNTL>();
        XeDraw d{};
        for (int i = 0; i < 3; i++)
        {
            d.ndcScale[i] = viewport.ndc_scale[i];
            d.ndcOffset[i] = viewport.ndc_offset[i];
        }
        d.indexed = draw.indexed;
        d.indexBase = draw.indexBase;
        d.indexFormat = draw.indexFormat;
        d.indexEndian = draw.indexEndian;
        d.indexOffset = int32_t(regs[XE_GPU_REG_VGT_INDX_OFFSET]);
        d.flags = (vte.vtx_xy_fmt ? 1u : 0u) | (vte.vtx_z_fmt ? 2u : 0u) | (vte.vtx_w0_fmt ? 4u : 0u);
        d.primitive = draw.primitiveType;
        d.alphaFunc = alphaFunc;
        d.alphaRef = f(XE_GPU_REG_RB_ALPHA_REF);
        d.pixelScale = float(g_scale);

        if (Tracing())
            Log("draw {}: vs {:X} ps {:X} prim {} count {} indexed {} target {}x{} viewport {},{} {}x{} ndc ({},{},{})+({},{},{}) flags {} scissor {},{} {}x{}",
                g_count.draws, (uint64_t)(uintptr_t)vs, (uint64_t)(uintptr_t)ps, draw.primitiveType, draw.indexCount, draw.indexed, width, height,
                viewport.xy_offset[0], viewport.xy_offset[1], viewport.xy_extent[0], viewport.xy_extent[1], viewport.ndc_scale[0],
                viewport.ndc_scale[1], viewport.ndc_scale[2], viewport.ndc_offset[0], viewport.ndc_offset[1], viewport.ndc_offset[2], d.flags,
                sx, sy, sw, sh);
        [encoder setVertexBytes:&d length:sizeof(d) atIndex:0];
        [encoder setVertexBytes:&regs[XE_GPU_REG_SHADER_CONSTANT_000_X] length:256 * 16 atIndex:1];
        [encoder setVertexBytes:&regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031] length:40 * 4 atIndex:2];
        [encoder setVertexBytes:&regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0] length:96 * 2 * 4 atIndex:3];
        [encoder setFragmentBytes:&d length:sizeof(d) atIndex:0];
        [encoder setFragmentBytes:&regs[XE_GPU_REG_SHADER_CONSTANT_256_X] length:256 * 16 atIndex:1];
        [encoder setFragmentBytes:&regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031] length:40 * 4 atIndex:2];
        [encoder setFragmentBytes:textureScales length:sizeof(textureScales) atIndex:3];
        for (uint32_t i = 0; i < textureCount; i++)
        {
            [encoder setFragmentTexture:textures[i] atIndex:i];
            [encoder setFragmentSamplerState:samplers[i] atIndex:i];
        }
        [encoder drawPrimitives:type vertexStart:0 vertexCount:count];
        g_count.drawn++;
    }

    // --- Resolves ------------------------------------------------------------------------------

    // The resolve rectangle, as Xenia's draw_util::GetResolveInfo computes it. False if empty.
    static bool ResolveRect(const RegisterFile& regs, int32_t& x0, int32_t& y0, int32_t& x1, int32_t& y1)
    {
        auto fetch = regs.GetVertexFetch(0);
        if (fetch.type != xenos::FetchConstantType::kVertex || fetch.size != 3 * 2)
        {
            ReportOnce("unsupported resolve vertex buffer");
            return false;
        }
        const uint32_t* raw = reinterpret_cast<const uint32_t*>(TranslatePhysical(fetch.address * 4));
        float half = regs.Get<reg::PA_SU_VTX_CNTL>().pix_center == xenos::PixelCenter::kD3DZero ? 0.5f : 0.0f;
        int32_t v[6];
        for (int i = 0; i < 6; i++)
            v[i] = xe::ui::FloatToD3D11Fixed16p8(xe::memory::Reinterpret<float>(SwapWord(raw[i], uint32_t(fetch.endian))) + half);
        x0 = (std::min({ v[0], v[2], v[4] }) + 127) >> 8;
        y0 = (std::min({ v[1], v[3], v[5] }) + 127) >> 8;
        x1 = (std::max({ v[0], v[2], v[4] }) + 127) >> 8;
        y1 = (std::max({ v[1], v[3], v[5] }) + 127) >> 8;
        if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable)
        {
            auto offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
            x0 += offset.window_x_offset;
            x1 += offset.window_x_offset;
            y0 += offset.window_y_offset;
            y1 += offset.window_y_offset;
        }
        draw_util::Scissor scissor;
        draw_util::GetScissor(regs, scissor, false);
        int32_t right = int32_t(scissor.offset[0] + scissor.extent[0]), bottom = int32_t(scissor.offset[1] + scissor.extent[1]);
        x0 = std::clamp(x0, int32_t(scissor.offset[0]), right);
        x1 = std::clamp(x1, int32_t(scissor.offset[0]), right);
        y0 = std::clamp(y0, int32_t(scissor.offset[1]), bottom);
        y1 = std::clamp(y1, int32_t(scissor.offset[1]), bottom);
        x0 &= ~7;
        y0 &= ~7;
        x1 = xe::align(x1, 8);
        y1 = xe::align(y1, 8);
        int32_t pitch = int32_t(regs.Get<reg::RB_SURFACE_INFO>().surface_pitch & ~7u);
        x0 = std::min(x0, pitch);
        x1 = std::min(x1, pitch);
        return x0 < x1 && y0 < y1;
    }

    // Mirrors ResolveParams in kResolveShaders.
    struct ResolveParams
    {
        uint32_t x0, y0, width, height;
        uint32_t pitchAligned, heightAligned;
        uint32_t destBase, endian, swap, clearValue;
        uint32_t scale;
    };

    static id<MTLComputePipelineState> g_resolvePipeline;
    static id<MTLComputePipelineState> g_clearPipeline;

    static void Dispatch(id<MTLComputePipelineState> pipeline, const ResolveParams& params, id<MTLTexture> source, id<MTLTexture> dest)
    {
        EndPass();
        id<MTLComputeCommandEncoder> encoder = [Commands() computeCommandEncoder];
        [encoder setComputePipelineState:pipeline];
        [encoder setTexture:source atIndex:0];
        [encoder setTexture:dest atIndex:1];
        [encoder setBuffer:g_memory offset:0 atIndex:0];
        [encoder setBytes:&params length:sizeof(params) atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(params.width, params.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [encoder endEncoding];
    }

    static void Resolve()
    {
        const RegisterFile& regs = Registers();
        auto copy = regs.Get<reg::RB_COPY_CONTROL>();
        if (copy.copy_command != xenos::CopyCommand::kRaw && copy.copy_command != xenos::CopyCommand::kConvert)
        {
            ReportOnce(fmt::format("unsupported resolve command {}", uint32_t(copy.copy_command)));
            return;
        }
        int32_t x0, y0, x1, y1;
        if (!ResolveRect(regs, x0, y0, x1, y1))
            return;
        const uint32_t pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
        const uint32_t destBase = regs[XE_GPU_REG_RB_COPY_DEST_BASE];
        if (Tracing())
        {
            Submit(true);
            Log("resolve {},{}-{},{} src {} dest {:08X} info {:08X} pitch {:08X} clear {:08X}", x0, y0, x1, y1, uint32_t(copy.copy_src_select),
                destBase, regs[XE_GPU_REG_RB_COPY_DEST_INFO], regs[XE_GPU_REG_RB_COPY_DEST_PITCH], regs[XE_GPU_REG_RB_COLOR_CLEAR]);
            if (copy.copy_src_select < 4)
            {
                auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[copy.copy_src_select]);
                WritePng(GetTarget(info.color_base, pitch, uint32_t(info.color_format), false), "out/trace/resolve_source.png");
            }
        }
        g_count.resolves++;
        g_count.copies += destBase != 0;

        if (copy.copy_src_select < 4)
        {
            auto info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[copy.copy_src_select]);
            id<MTLTexture> target = GetTarget(info.color_base, pitch, uint32_t(info.color_format), false);
            x1 = std::min(x1, int32_t(target.width / g_scale));
            y1 = std::min(y1, int32_t(target.height / g_scale));
            if (x0 >= x1 || y0 >= y1)
                return;
            const bool rgba8 = target.pixelFormat == MTLPixelFormatRGBA8Unorm || target.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB;
            auto dest = regs.Get<reg::RB_COPY_DEST_INFO>();
            auto destPitch = regs.Get<reg::RB_COPY_DEST_PITCH>();
            ResolveParams params{ uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0),
                                  xe::align(uint32_t(destPitch.copy_dest_pitch), 32u), xe::align(uint32_t(destPitch.copy_dest_height), 32u),
                                  destBase, uint32_t(dest.copy_dest_endian), dest.copy_dest_swap, regs[XE_GPU_REG_RB_COLOR_CLEAR], g_scale };

            if (destBase && (!rgba8 || dest.copy_dest_format != xenos::ColorFormat::k_8_8_8_8 || dest.copy_dest_exp_bias != 0))
                ReportOnce(fmt::format("unsupported resolve from {} to format {} (exp bias {})", uint32_t(target.pixelFormat),
                                       uint32_t(dest.copy_dest_format), int(dest.copy_dest_exp_bias)));
            else if (destBase && destPitch.copy_dest_pitch && destPitch.copy_dest_height)
            {
                // EDRAM k_8_8_8_8 and the k_8_8_8_8 texture format both hold the first component in the low byte.
                id<MTLTexture> copyTexture = GetResolveTexture(destBase, destPitch.copy_dest_pitch, destPitch.copy_dest_height, g_scale);
                // The GPU writes this memory; textures over it must be re-read.
                TouchPhysical(destBase, params.pitchAligned * params.heightAligned * 4);
                Dispatch(g_resolvePipeline, params, target, copyTexture);
            }
            if (copy.color_clear_enable)
            {
                if (target.pixelFormat == MTLPixelFormatRGBA8Unorm && x0 == 0 && y0 == 0)
                {
                    // A rectangle from the origin is a render area: clearing it with a pass costs only the store.
                    EndPass();
                    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                    pass.renderTargetWidth = uint32_t(x1) * g_scale;
                    pass.renderTargetHeight = uint32_t(y1) * g_scale;
                    pass.colorAttachments[0].texture = target;
                    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                    const uint32_t c = params.clearValue;
                    pass.colorAttachments[0].clearColor = MTLClearColorMake((c & 0xFF) / 255.0, (c >> 8 & 0xFF) / 255.0, (c >> 16 & 0xFF) / 255.0, (c >> 24) / 255.0);
                    [[Commands() renderCommandEncoderWithDescriptor:pass] endEncoding];
                }
                else if (rgba8)
                    Dispatch(g_clearPipeline, params, nil, target);
                else
                    ReportOnce("color clear of a non-RGBA8 target is not supported");
            }
        }
        else if (destBase)
            ReportOnce("depth resolves are not supported");

        if (copy.depth_clear_enable)
        {
            // Note: clears the next pass's whole render area, which covers the resolve rectangle.
            auto info = regs.Get<reg::RB_DEPTH_INFO>();
            id<MTLTexture> depth = GetTarget(info.depth_base, pitch, uint32_t(info.depth_format), true);
            g_pendingDepthClears[(__bridge void*)depth] = { regs[XE_GPU_REG_RB_DEPTH_CLEAR], uint32_t(x1) * g_scale, uint32_t(y1) * g_scale };
        }
    }

    // --- Presentation --------------------------------------------------------------------------

    // Writes an RGBA8 or R8 texture as PNG. getBytes returns unswizzled data, so RGBA8 applies the
    // texture's swizzle here; R8 is written as gray.
    static void WritePng(id<MTLTexture> texture, const std::string& path)
    {
        const size_t w = texture.width, h = texture.height;
        const MTLTextureSwizzleChannels swizzle = texture.swizzle;
        if (texture.storageMode == MTLStorageModePrivate)
        {
            // Stage through a CPU-visible copy.
            MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:texture.pixelFormat width:w height:h mipmapped:NO];
            desc.storageMode = MTLStorageModeShared;
            id<MTLTexture> staging = [g_device newTextureWithDescriptor:desc];
            EndPass();
            id<MTLBlitCommandEncoder> blit = [Commands() blitCommandEncoder];
            [blit copyFromTexture:texture toTexture:staging];
            [blit endEncoding];
            texture = staging;
        }
        Submit(true);
        std::vector<uint8_t> rgba(w * h * 4);
        if (texture.pixelFormat == MTLPixelFormatR8Unorm)
        {
            std::vector<uint8_t> gray(w * h);
            [texture getBytes:gray.data() bytesPerRow:w fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
            for (size_t i = 0; i < w * h; i++)
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = gray[i], rgba[i * 4 + 3] = 255;
        }
        else if (texture.pixelFormat == MTLPixelFormatRGBA8Unorm || texture.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB)
        {
            std::vector<uint8_t> pixels(w * h * 4);
            [texture getBytes:pixels.data() bytesPerRow:w * 4 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
            const MTLTextureSwizzle channels[4] = { swizzle.red, swizzle.green, swizzle.blue, swizzle.alpha };
            for (size_t i = 0; i < w * h; i++)
            {
                for (int c = 0; c < 4; c++)
                {
                    MTLTextureSwizzle s = channels[c];
                    rgba[i * 4 + c] = s == MTLTextureSwizzleZero ? 0 : s == MTLTextureSwizzleOne ? 255 : pixels[i * 4 + (s - MTLTextureSwizzleRed)];
                }
            }
        }
        else
        {
            Log("Renderer: cannot write {} (pixel format {})", path, uint32_t(texture.pixelFormat));
            return;
        }
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), error);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(rgba.data(), w, h, 8, w * 4, space, kCGImageAlphaPremultipliedLast);
        CGImageRef image = CGBitmapContextCreateImage(context);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, (const UInt8*)path.c_str(), path.size(), false);
        if (CGImageDestinationRef out = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr))
        {
            CGImageDestinationAddImage(out, image, nullptr);
            CGImageDestinationFinalize(out);
            CFRelease(out);
        }
        else
            Log("Renderer: cannot write {}", path);
        CFRelease(url);
        CGImageRelease(image);
        CGContextRelease(context);
        CGColorSpaceRelease(space);
    }

    static void Swap(uint32_t, uint32_t width, uint32_t height)
    {
        @autoreleasepool
        {
            // VdSwap put the front buffer's fetch constant in slot 0.
            auto fetch = Registers().GetTextureFetch(0);
            id<MTLTexture> frontbuffer = GetTexture(fetch);
            // PEGGLE2_DUMP_FRAMES=dir: saves the front buffer as PNG every PEGGLE2_DUMP_EVERY frames (300 by
            // default) from frame PEGGLE2_DUMP_FROM.
            static const char* frameDir = getenv("PEGGLE2_DUMP_FRAMES");
            static const uint64_t every = getenv("PEGGLE2_DUMP_EVERY") ? strtoull(getenv("PEGGLE2_DUMP_EVERY"), nullptr, 10) : 300;
            static const uint64_t from = getenv("PEGGLE2_DUMP_FROM") ? strtoull(getenv("PEGGLE2_DUMP_FROM"), nullptr, 10) : 0;
            if (frontbuffer && frameDir && FrameCount() >= from && FrameCount() % every == 0)
                WritePng(frontbuffer, fmt::format("{}/frame_{:05}.png", frameDir, FrameCount()));

            CAMetalLayer* layer = (__bridge CAMetalLayer*)MetalLayer();
            id<CAMetalDrawable> drawable = layer ? [layer nextDrawable] : nil;
            if (drawable && frontbuffer)
            {
                EndPass();
                MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = drawable.texture;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
                id<MTLRenderCommandEncoder> encoder = [Commands() renderCommandEncoderWithDescriptor:pass];
                // Fit the guest frame (width x height of the front buffer) into the drawable.
                double scale = std::min(double(drawable.texture.width) / width, double(drawable.texture.height) / height);
                double w = width * scale, h = height * scale;
                [encoder setViewport:{ (drawable.texture.width - w) / 2, (drawable.texture.height - h) / 2, w, h, 0, 1 }];
                [encoder setRenderPipelineState:g_presentPipeline];
                float uvScale[2] = { float(width) / (fetch.size_2d.width + 1), float(height) / (fetch.size_2d.height + 1) };
                [encoder setFragmentBytes:uvScale length:sizeof(uvScale) atIndex:0];
                [encoder setFragmentTexture:frontbuffer atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [encoder endEncoding];
                // The game shows a new frame every vblank (60 fps) or every other one (30 fps); a minimum
                // on-screen time keeps frames evenly paced on 120 Hz (ProMotion) displays. Just under a frame,
                // so timing jitter in the vblank thread never makes presentation hold the game back.
                [Commands() presentDrawable:drawable afterMinimumDuration:1.0 / (FrameRate() + 1)];
            }
            Submit(false);
            InvalidateTextures();
            // A new render scale starts with the next frame, on new targets (the game redraws every frame).
            if (const uint32_t scale = uint32_t(RenderScale()); scale != g_scale)
            {
                g_scale = scale;
                g_targets.clear();
                g_pendingDepthClears.clear();
                g_drawnExtents.clear();
                for (auto& color : g_passColors)
                    color = nil;
                g_passDepth = nil;
                Log("Renderer: render scale {}x", scale);
            }
            g_count.swaps++;
            static auto last = std::chrono::steady_clock::now();
            if (g_stats && std::chrono::steady_clock::now() - last >= std::chrono::seconds(1))
            {
                last = std::chrono::steady_clock::now();
                Log("GPU stats: {} swaps, {} draws ({} drawn, {} no shader, {} no target, {} scissored), {} resolves ({} copies), "
                    "{} texture binds ({} missing), {} uploads; {} passes, {} command buffers ({} waited), GPU busy {:.1f} ms", g_count.swaps, g_count.draws, g_count.drawn, g_count.noShader, g_count.noTarget,
                    g_count.scissored, g_count.resolves, g_count.copies, g_count.textures, g_count.missingTextures, TakeTextureUploadCount(),
                    g_count.passes, g_count.buffers, g_count.waits, g_gpuNanos.exchange(0) / 1e6);
                g_count = {};
            }
        }
    }

    // Resolve copies EDRAM pixels into guest memory as tiled k_8_8_8_8 (and into the resolve's GPU copy);
    // clear fills a render target region. Tiled2D follows Xenia's texture_address.h.
    static const char* kResolveShaders = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct ResolveParams
{
    uint x0, y0, width, height;
    uint pitchAligned, heightAligned;
    uint destBase, endian, swap, clearValue;
    uint scale;
};
static uint tiled_2d(uint x, uint y, uint pitch, uint bppLog2)
{
    uint outer = (((y >> 5) * (pitch >> 5) + (x >> 5)) << 6);
    uint inner = (((y >> 1) & 7u) << 3) | (x & 7u);
    uint bytes = (outer | inner) << bppLog2;
    uint bank = (y >> 4) & 1u;
    uint pipe = ((x >> 3) & 3u) ^ (((y >> 3) & 1u) << 1);
    return ((y & 1u) << 4) | (pipe << 6) | (bank << 11) | (bytes & 15u) | (((bytes >> 4) & 1u) << 5) |
           (((bytes >> 5) & 7u) << 8) | ((bytes >> 8) << 12);
}
static uint swap_word(uint v, uint endian)
{
    switch (endian & 3u)
    {
    case 1u: return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    case 2u: return (v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24);
    case 3u: return (v >> 16) | (v << 16);
    default: return v;
    }
}
kernel void resolve(texture2d<float, access::read> source [[texture(0)]], texture2d<float, access::write> copy [[texture(1)]],
                    device uint* memory [[buffer(0)]], constant ResolveParams& p [[buffer(1)]], uint2 id [[thread_position_in_grid]])
{
    uint x = p.x0 + id.x, y = p.y0 + id.y;
    if (id.x >= p.width || id.y >= p.height || x >= p.pitchAligned || y >= p.heightAligned)
        return;
    // The GPU copy keeps every host pixel; guest memory gets each guest pixel's average.
    float4 c = float4(0.0);
    for (uint dy = 0u; dy < p.scale; dy++)
        for (uint dx = 0u; dx < p.scale; dx++)
        {
            uint2 h = uint2(x, y) * p.scale + uint2(dx, dy);
            float4 v = source.read(h);
            if (p.swap != 0u)
                v = v.zyxw;
            if (h.x < copy.get_width() && h.y < copy.get_height())
                copy.write(v, h);
            c += v;
        }
    c /= float(p.scale * p.scale);
    memory[(p.destBase + tiled_2d(x, y, p.pitchAligned, 2u)) >> 2] = swap_word(pack_float_to_unorm4x8(c), p.endian);
}
kernel void clear(texture2d<float, access::write> target [[texture(1)]], constant ResolveParams& p [[buffer(1)]],
                  uint2 id [[thread_position_in_grid]])
{
    if (id.x < p.width && id.y < p.height)
        for (uint dy = 0u; dy < p.scale; dy++)
            for (uint dx = 0u; dx < p.scale; dx++)
                target.write(unpack_unorm4x8_to_float(p.clearValue), (uint2(p.x0, p.y0) + id) * p.scale + uint2(dx, dy));
}
)MSL";

    static const char* kPresentShader = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Out { float4 position [[position]]; float2 uv; };
vertex Out present_vertex(uint id [[vertex_id]])
{
    float2 uv = float2((id << 1) & 2, id & 2);
    Out o;
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}
// Catmull-Rom (bicubic) in nine bilinear taps (the weights of each axis's two middle texels are merged into
// one tap between them): sharper than bilinear when the frame is enlarged to fill a larger window.
static float3 catmull_rom(texture2d<float> t, sampler s, float2 uv, float2 size)
{
    float2 p = uv * size, p1 = floor(p - 0.5) + 0.5, f = p - p1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f)), w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (p1 - 1.0) / size, t3 = (p1 + 2.0) / size, t12 = (p1 + w2 / w12) / size;
    float3 c = (t.sample(s, float2(t0.x, t0.y)).rgb * w0.x + t.sample(s, float2(t12.x, t0.y)).rgb * w12.x + t.sample(s, float2(t3.x, t0.y)).rgb * w3.x) * w0.y +
               (t.sample(s, float2(t0.x, t12.y)).rgb * w0.x + t.sample(s, float2(t12.x, t12.y)).rgb * w12.x + t.sample(s, float2(t3.x, t12.y)).rgb * w3.x) * w12.y +
               (t.sample(s, float2(t0.x, t3.y)).rgb * w0.x + t.sample(s, float2(t12.x, t3.y)).rgb * w12.x + t.sample(s, float2(t3.x, t3.y)).rgb * w3.x) * w3.y;
    return saturate(c);
}
fragment float4 present_fragment(Out in [[stage_in]], constant float2& uvScale [[buffer(0)]], texture2d<float> t [[texture(0)]])
{
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float2 uv = in.uv * uvScale;
    // Scaling a high-resolution frame down to the window: four bilinear taps a quarter pixel apart, so every
    // source texel counts (one tap per pixel skips texels and shimmers). Scaling up: Catmull-Rom.
    float2 dx = dfdx(uv) * 0.25, dy = dfdy(uv) * 0.25;
    float2 size = float2(t.get_width(), t.get_height());
    if (max(length(dx * size), length(dy * size)) <= 0.25)
        return float4(catmull_rom(t, s, uv, size), 1.0);
    float3 c = t.sample(s, uv - dx - dy).rgb + t.sample(s, uv + dx - dy).rgb + t.sample(s, uv - dx + dy).rgb +
               t.sample(s, uv + dx + dy).rgb;
    return float4(c * 0.25, 1.0);
}
)MSL";

    void InitializeRenderer()
    {
        g_device = MTLCreateSystemDefaultDevice();
        g_queue = [g_device newCommandQueue];
        Log("Renderer: {}", [g_device.name UTF8String]);
        InitializeTextures(g_device);
        g_scale = uint32_t(RenderScale());

        // Guest physical memory, shared with the GPU without copies; offset 0 is physical address 0.
        g_memory = [g_device newBufferWithBytesNoCopy:Translate(PhysicalToVirtual(0)) length:0x20000000
                                              options:MTLResourceStorageModeShared deallocator:nil];
        if (!g_memory)
            Fatal("Renderer: cannot map guest physical memory for the GPU");

        MTLTextureDescriptor* dummy = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        g_dummyTexture = [g_device newTextureWithDescriptor:dummy];

        NSError* error = nil;
        id<MTLLibrary> library = [g_device newLibraryWithSource:@(kPresentShader) options:nil error:&error];
        MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
        desc.vertexFunction = [library newFunctionWithName:@"present_vertex"];
        desc.fragmentFunction = [library newFunctionWithName:@"present_fragment"];
        desc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_presentPipeline = [g_device newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!g_presentPipeline)
            Fatal("Renderer: present pipeline: {}", error.localizedDescription.UTF8String);
        id<MTLLibrary> resolveLibrary = [g_device newLibraryWithSource:@(kResolveShaders) options:nil error:&error];
        if (!resolveLibrary)
            Fatal("Renderer: resolve shaders: {}", error.localizedDescription.UTF8String);
        g_resolvePipeline = [g_device newComputePipelineStateWithFunction:[resolveLibrary newFunctionWithName:@"resolve"] error:&error];
        g_clearPipeline = [g_device newComputePipelineStateWithFunction:[resolveLibrary newFunctionWithName:@"clear"] error:&error];
        if (!g_resolvePipeline || !g_clearPipeline)
            Fatal("Renderer: resolve pipelines: {}", error.localizedDescription.UTF8String);
        SetTextureSync([] { Submit(true); });
        SetTextureCopy([](id<MTLTexture> from, id<MTLTexture> to) {
            EndPass();
            id<MTLBlitCommandEncoder> blit = [Commands() blitCommandEncoder];
            [blit copyFromTexture:from sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(to.width, to.height, 1)
                        toTexture:to destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
        });

        Backend backend;
        backend.loadShader = LoadShader;
        backend.draw = [](const DrawRequest& draw) {
            @autoreleasepool
            {
                Draw(draw);
            }
        };
        backend.swap = Swap;
        backend.sync = Sync;
        SetBackend(std::move(backend));
    }
}
