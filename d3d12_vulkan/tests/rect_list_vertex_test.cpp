// rect_list_vertex on a translated vertex shader: d3d12vk_rect_list_vertex_test IN.spv OUT.spv
// Writes the rewritten module for spirv-val; prints the attribute locations it moved.
#include "rect_list_vertex.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s IN.spv OUT.spv\n", argv[0]);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::vector<std::uint32_t> words(bytes.size() / 4);
    std::memcpy(words.data(), bytes.data(), words.size() * 4);
    std::string why;
    const leonida::RectVertexShader out = leonida::rect_list_vertex(words, why);
    if (out.words.empty()) {
        std::printf("refused: %s\n", why.c_str());
        return 1;
    }
    std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char*>(out.words.data()),
                                                   std::streamsize(out.words.size() * 4));
    std::printf("rewritten: %zu words -> %zu words; attribute locations", words.size(), out.words.size());
    for (const auto l : out.attribute_locations) std::printf(" %u", l);
    std::printf("\n");
    return 0;
}
