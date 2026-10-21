// Runs XenonAnalyse's function analyzer at an address and prints the blocks it finds.
// usage: fnanalyze <image.bin> <addr>...
#include <cstdio>
#include <cstdlib>
#include <file.h>
#include <function.h>

int main(int argc, char** argv)
{
    if (argc < 3)
        return fprintf(stderr, "usage: fnanalyze <image.bin> <addr>...\n"), 1;

    auto img = LoadFile(argv[1]);
    for (int i = 2; i < argc; i++)
    {
        size_t addr = strtoul(argv[i], nullptr, 16);
        size_t offset = addr - 0x82000000;
        auto fn = Function::Analyze(img.data() + offset, img.size() - offset, addr);
        printf("%zX: size 0x%zX\n", fn.base, fn.size);
        for (auto& b : fn.blocks)
            printf("  block %zX..%zX\n", fn.base + b.base, fn.base + b.base + b.size);
    }
    return 0;
}
