// Translates dumped Xenos shaders to MSL and compiles each with Metal, reporting failures.
// usage: msl_check <vs_|ps_ file.bin>...   (PEGGLE2_MSL_PRINT=1 prints the source)
#include <cstdio>
#include <filesystem>
#include <vector>
#import <Metal/Metal.h>
#include "../../runtime/gpu/msl_prelude.h"
#include "../../runtime/gpu/msl_translator.h"

namespace rt { void LogLine(const std::string& line) { fprintf(stderr, "%s\n", line.c_str()); } }

int main(int argc, char** argv)
{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    int failures = 0;
    for (int i = 1; i < argc; i++)
    {
        FILE* f = fopen(argv[i], "rb");
        if (!f)
            continue;
        std::vector<uint32_t> words;
        uint32_t w;
        while (fread(&w, 4, 1, f) == 1)
            words.push_back(w);
        fclose(f);
        bool pixel = std::filesystem::path(argv[i]).filename().string().rfind("ps_", 0) == 0;
        xe::gpu::Shader shader(pixel ? xe::gpu::xenos::ShaderType::kPixel : xe::gpu::xenos::ShaderType::kVertex, 0, words.data(), words.size());
        xe::StringBuffer buffer;
        shader.AnalyzeUcode(buffer);
        rt::gpu::MslTranslator translator;
        rt::gpu::ShaderInfo info;
        std::string body = translator.Translate(shader, info);
        std::string source = std::string(rt::gpu::kMslPrelude) + body + (pixel ? rt::gpu::FragmentEntry(info) : rt::gpu::kMslVertexEntry);
        if (!info.errors.empty())
            printf("translation errors in %s:\n%s", argv[i], info.errors.c_str());
        if (getenv("PEGGLE2_MSL_PRINT"))
            printf("==== %s\n%s\n", argv[i], body.c_str());
        NSError* error = nil;
        MTLCompileOptions* options = [MTLCompileOptions new];
        if (@available(macOS 15.0, *))
            options.mathMode = MTLMathModeSafe; // as the renderer compiles them
        id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
        if (!library)
        {
            failures++;
            printf("FAIL %s\n%s\n", argv[i], error.localizedDescription.UTF8String);
        }
        else
            printf("ok   %s (textures %X, colors %X, interpolators %X)\n", argv[i], info.textureSlots, info.colorTargets, info.interpolators);
    }
    return failures ? 1 : 0;
}
