// Disassembles a dumped image (base 0x82000000) around addresses.
// usage: disas <image.bin> <before> <after> <addr>...   (addr "mtctr" = every mtctr r0 in .text)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <disasm.h>
#include <file.h>

constexpr uint32_t Base = 0x82000000;

static uint32_t Word(const std::vector<uint8_t>& img, uint32_t addr)
{
    const uint8_t* p = &img[addr - Base];
    return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static void Dump(const std::vector<uint8_t>& img, uint32_t addr, int before, int after)
{
    for (uint32_t a = addr - before * 4; a <= addr + after * 4; a += 4)
    {
        ppc_insn insn{};
        ppc::Disassemble(&img[a - Base], a, insn);
        printf("%s%08X  %08X  %-8s %s\n", a == addr ? ">" : " ", a, Word(img, a),
            insn.opcode ? insn.opcode->name : "???", insn.op_str);
    }
    printf("\n");
}

int main(int argc, char** argv)
{
    if (argc < 5)
        return fprintf(stderr, "usage: disas <image.bin> <before> <after> <addr|mtctr>...\n"), 1;

    auto img = LoadFile(argv[1]);
    int before = atoi(argv[2]), after = atoi(argv[3]);
    for (int i = 4; i < argc; i++)
    {
        if (strcmp(argv[i], "mtctr") == 0)
        {
            for (uint32_t a = 0x82140000; a < 0x82884C8C; a += 4)
                if (Word(img, a) == 0x7C0903A6)
                    Dump(img, a, before, after);
        }
        else
            Dump(img, strtoul(argv[i], nullptr, 16), before, after);
    }
    return 0;
}
