// Shader translator test harness.
//
//   h3shader [--disasm] <out dir> <shader.bin>...
//
// Each input is microcode as dumped by H3_GPU_CAPTURE (big-endian dwords;
// vs_*.bin = vertex, ps_*.bin = pixel). Writes <out dir>/<name>.vert/.frag
// and prints one line per shader. Check the output with glslang, e.g.
//   glslangValidator -V --target-env vulkan1.1 <out dir>/*.vert <out dir>/*.frag

#include <gpu/shader/translator.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv)
{
    bool disasm = false;
    int first = 1;
    if (argc > 1 && strcmp(argv[1], "--disasm") == 0)
    {
        disasm = true;
        first = 2;
    }
    if (argc - first < 2)
    {
        fprintf(stderr, "usage: h3shader [--disasm] <out dir> <shader.bin>...\n");
        return 1;
    }
    std::filesystem::path outDir = argv[first];
    std::filesystem::create_directories(outDir);

    int failures = 0;
    for (int i = first + 1; i < argc; i++)
    {
        std::filesystem::path path = argv[i];
        std::ifstream file(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<uint32_t> dwords(bytes.size() / 4);
        for (size_t j = 0; j < dwords.size(); j++)
        {
            uint32_t value;
            memcpy(&value, bytes.data() + j * 4, 4);
            dwords[j] = __builtin_bswap32(value);
        }

        std::string name = path.stem().string();
        bool isVertex = name.rfind("vs", 0) == 0;
        if (disasm)
        {
            printf("== %s\n%s", name.c_str(), h3::gpu::shader::Disassemble(isVertex, dwords.data(), uint32_t(dwords.size())).c_str());
            continue;
        }

        auto result = h3::gpu::shader::Translate(isVertex, dwords.data(), uint32_t(dwords.size()));
        if (!result.ok)
        {
            printf("FAIL %s: %s\n", name.c_str(), result.error.c_str());
            failures++;
            continue;
        }
        std::filesystem::path out = outDir / (name + (isVertex ? ".vert" : ".frag"));
        std::ofstream(out) << result.glsl;
        printf("ok   %s: %zu textures, %zu vfetch, colors %X%s\n", name.c_str(), result.info.textures.size(),
            result.info.vertexFetchConstants.size(), result.info.colorTargetMask, result.info.writesDepth ? ", depth" : "");
    }
    return failures ? 1 : 0;
}
