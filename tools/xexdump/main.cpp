// Loads a XEX with XenonUtils and writes the mapped image plus a text summary.
#include <cstdio>
#include <file.h>
#include <image.h>

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: xexdump <default.xex> <out.bin>\n");
        return 1;
    }

    auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    if (image.data == nullptr)
    {
        fprintf(stderr, "failed to load %s\n", argv[1]);
        return 1;
    }

    FILE* out = fopen(argv[2], "wb");
    fwrite(image.data.get(), 1, image.size, out);
    fclose(out);

    printf("base %08zX size %08X entry %08zX\n", image.base, image.size, image.entry_point);
    for (auto& s : image.sections)
        printf("section %-8s %08zX %08X %s\n", s.name.c_str(), s.base, s.size, (s.flags & SectionFlags_Code) ? "code" : "data");
    for (auto& sym : image.symbols)
        printf("import %08zX %s\n", sym.address, sym.name.c_str());
    return 0;
}
