// Disassembles dumped Xenos shader microcode with Xenia's parser.
// usage: xenos_disasm <vs_|ps_ file.bin>...
#include <cstdio>
#include <filesystem>
#include <vector>
#include "xenia/gpu/shader.h"
#include "xenia/base/byte_order.h"

namespace rt
{
    void LogLine(const std::string& line) { fprintf(stderr, "%s\n", line.c_str()); }
}

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++)
    {
        FILE* f = fopen(argv[i], "rb");
        if (!f)
            continue;
        std::vector<uint32_t> words;
        uint32_t w;
        while (fread(&w, 4, 1, f) == 1)
            words.push_back(w); // guest (big-endian) order; Shader swaps it
        fclose(f);
        bool pixel = std::filesystem::path(argv[i]).filename().string().rfind("ps_", 0) == 0;
        xe::gpu::Shader shader(pixel ? xe::gpu::xenos::ShaderType::kPixel : xe::gpu::xenos::ShaderType::kVertex, 0, words.data(), words.size());
        xe::StringBuffer buffer;
        shader.AnalyzeUcode(buffer);
        printf("==== %s\n%s\n", argv[i], shader.ucode_disassembly().c_str());
    }
    return 0;
}
