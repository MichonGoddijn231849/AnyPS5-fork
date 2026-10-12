// APS5_DEPTH_CLEAR_DRAWS (docs/dev/NIGHT_SHIFT.md C5; Graphics/src/Draw.cpp recordClearDraw): a DB_RENDER_CONTROL
// depth clear draw writes the clear value over the area it covers, the scissor within the viewport, and nothing else.
// Three draws on a 64x64 color target with a D32 depth target:
//   0. depth test NEVER: makes the depth surface (cleared to 0.0 when it is made) and writes nothing;
//   1. the clear draw, DB_DEPTH_CLEAR = 1.0, scissor = the left half;
//   2. a full-screen triangle at z = 0.5, depth test LESS, white: it passes where the depth is 1.0 (the left half).
// So the left half must be white and the right half untouched. The shaders: clear.vert / clear.frag (GLSL in the
// comments below), compiled with glslang.
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 64;
alignas(4096) std::array<std::uint8_t, Width * Height * 4> Pixels{};
alignas(4096) std::array<float, Width * Height> Depth{};

// #version 450
// out gl_PerVertex { vec4 gl_Position; };
// void main() {
//     vec2 positions[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
//     gl_Position = vec4(positions[gl_VertexIndex], 0.5, 1.0);
// }
constexpr std::array<std::uint32_t, 226> VertexCode{
    0x07230203u, 0x00010000u, 0x0008000bu, 0x00000026u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0007000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000016u, 0x0000001au, 0x00030003u,
    0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00050005u, 0x0000000cu,
    0x69736f70u, 0x6e6f6974u, 0x00000073u, 0x00060005u, 0x00000014u, 0x505f6c67u, 0x65567265u, 0x78657472u,
    0x00000000u, 0x00060006u, 0x00000014u, 0x00000000u, 0x505f6c67u, 0x7469736fu, 0x006e6f69u, 0x00030005u,
    0x00000016u, 0x00000000u, 0x00060005u, 0x0000001au, 0x565f6c67u, 0x65747265u, 0x646e4978u, 0x00007865u,
    0x00030047u, 0x00000014u, 0x00000002u, 0x00050048u, 0x00000014u, 0x00000000u, 0x0000000bu, 0x00000000u,
    0x00040047u, 0x0000001au, 0x0000000bu, 0x0000002au, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u,
    0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000002u,
    0x00040015u, 0x00000008u, 0x00000020u, 0x00000000u, 0x0004002bu, 0x00000008u, 0x00000009u, 0x00000003u,
    0x0004001cu, 0x0000000au, 0x00000007u, 0x00000009u, 0x00040020u, 0x0000000bu, 0x00000007u, 0x0000000au,
    0x0004002bu, 0x00000006u, 0x0000000du, 0xbf800000u, 0x0005002cu, 0x00000007u, 0x0000000eu, 0x0000000du,
    0x0000000du, 0x0004002bu, 0x00000006u, 0x0000000fu, 0x40400000u, 0x0005002cu, 0x00000007u, 0x00000010u,
    0x0000000fu, 0x0000000du, 0x0005002cu, 0x00000007u, 0x00000011u, 0x0000000du, 0x0000000fu, 0x0006002cu,
    0x0000000au, 0x00000012u, 0x0000000eu, 0x00000010u, 0x00000011u, 0x00040017u, 0x00000013u, 0x00000006u,
    0x00000004u, 0x0003001eu, 0x00000014u, 0x00000013u, 0x00040020u, 0x00000015u, 0x00000003u, 0x00000014u,
    0x0004003bu, 0x00000015u, 0x00000016u, 0x00000003u, 0x00040015u, 0x00000017u, 0x00000020u, 0x00000001u,
    0x0004002bu, 0x00000017u, 0x00000018u, 0x00000000u, 0x00040020u, 0x00000019u, 0x00000001u, 0x00000017u,
    0x0004003bu, 0x00000019u, 0x0000001au, 0x00000001u, 0x00040020u, 0x0000001cu, 0x00000007u, 0x00000007u,
    0x0004002bu, 0x00000006u, 0x0000001fu, 0x3f000000u, 0x0004002bu, 0x00000006u, 0x00000020u, 0x3f800000u,
    0x00040020u, 0x00000024u, 0x00000003u, 0x00000013u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u,
    0x00000003u, 0x000200f8u, 0x00000005u, 0x0004003bu, 0x0000000bu, 0x0000000cu, 0x00000007u, 0x0003003eu,
    0x0000000cu, 0x00000012u, 0x0004003du, 0x00000017u, 0x0000001bu, 0x0000001au, 0x00050041u, 0x0000001cu,
    0x0000001du, 0x0000000cu, 0x0000001bu, 0x0004003du, 0x00000007u, 0x0000001eu, 0x0000001du, 0x00050051u,
    0x00000006u, 0x00000021u, 0x0000001eu, 0x00000000u, 0x00050051u, 0x00000006u, 0x00000022u, 0x0000001eu,
    0x00000001u, 0x00070050u, 0x00000013u, 0x00000023u, 0x00000021u, 0x00000022u, 0x0000001fu, 0x00000020u,
    0x00050041u, 0x00000024u, 0x00000025u, 0x00000016u, 0x00000018u, 0x0003003eu, 0x00000025u, 0x00000023u,
    0x000100fdu, 0x00010038u
};

// #version 450
// layout(location = 0) out vec4 color;
// void main() { color = vec4(1.0, 1.0, 1.0, 1.0); }
constexpr std::array<std::uint32_t, 83> FragmentCode{
    0x07230203u, 0x00010000u, 0x0008000bu, 0x0000000cu, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0006000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x00030010u, 0x00000004u,
    0x00000007u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
    0x00040005u, 0x00000009u, 0x6f6c6f63u, 0x00000072u, 0x00040047u, 0x00000009u, 0x0000001eu, 0x00000000u,
    0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u,
    0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u, 0x00000003u, 0x00000007u,
    0x0004003bu, 0x00000008u, 0x00000009u, 0x00000003u, 0x0004002bu, 0x00000006u, 0x0000000au, 0x3f800000u,
    0x0007002cu, 0x00000007u, 0x0000000bu, 0x0000000au, 0x0000000au, 0x0000000au, 0x0000000au, 0x00050036u,
    0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x0003003eu, 0x00000009u,
    0x0000000bu, 0x000100fdu, 0x00010038u
};

ShaderRecompiler::RecompileResult Module(std::span<const std::uint32_t> words) {
    ShaderRecompiler::RecompileResult result;
    result.spirv.resize(words.size());
    std::copy(words.begin(), words.end(), result.spirv.data());
    return result;
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_DEPTH_CLEAR_DRAWS", "1");
#else
    setenv("APS5_DEPTH_CLEAR_DRAWS", "1", 1);
#endif
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        // Registered guest memory, so the color target is written back to it.
        GuestAllocations::Mutation().Add(Pixels.data(), sizeof(Pixels), true, true, true);
        GuestAllocations::Mutation().Add(Depth.data(), sizeof(Depth), true, true, true);
        struct Release {
            ~Release() {
                GuestAllocations::Mutation().Remove(Pixels.data());
                GuestAllocations::Mutation().Remove(Depth.data());
            }
        } release;
        auto vertex = Module(VertexCode);
        auto fragment = Module(FragmentCode);
        const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
            {ShaderRecompiler::ShaderStage::Vertex, &vertex, 0},
            {ShaderRecompiler::ShaderStage::Fragment, &fragment, 0}
        }};
        Pixels.fill(0);
        AgcDriver::Graphics::State state{};
        state.stages.path = AgcDriver::Graphics::ShaderPath::Vertex;
        state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
        state.colors = {state.color};
        state.hasColorTarget = true;
        state.negativeOneToOne = false;
        state.cullMode = VK_CULL_MODE_NONE;
        state.renderExtent = {Width, Height};
        state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
        state.scissor = {{0, 0}, {Width, Height}};
        state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        AgcDriver::Graphics::DepthTarget depth{};
        depth.address = reinterpret_cast<std::uintptr_t>(Depth.data());
        depth.extent = {Width, Height};
        depth.format = VK_FORMAT_D32_SFLOAT;
        depth.clearDepth = 0.0f;
        state.depth = depth;
        const AgcDriver::Pm4::DrawParameters draw{0, 3, 0, 1, 0, false};

        // 0: makes the depth surface, cleared to 0.0; writes nothing.
        state.depthTest = true;
        state.depthCompare = VK_COMPARE_OP_NEVER;
        state.blend.colorWriteMask = 0;
        state.blends = {state.blend};
        device->Draw(state, draw, shaders);

        // 1: the clear draw over the left half.
        auto clear = state;
        clear.depth->clearDepth = 1.0f;
        clear.depthClearDraw = true;
        clear.depthWrite = true;
        clear.depthCompare = VK_COMPARE_OP_ALWAYS;
        clear.scissor = {{0, 0}, {Width / 2, Height}};
        device->Draw(clear, draw, shaders);

        // 2: white where 0.5 < depth.
        state.depthCompare = VK_COMPARE_OP_LESS;
        state.blend.colorWriteMask = 15;
        state.blends = {state.blend};
        device->Draw(state, draw, shaders);
        AgcDriver::Graphics::StorageTexture::FlushAllPending("test read");
        device->WaitIdle();

        std::uint32_t whiteLeft = 0, whiteRight = 0;
        for (std::uint32_t y = 0; y < Height; ++y) {
            for (std::uint32_t x = 0; x < Width; ++x) (x < Width / 2 ? whiteLeft : whiteRight) += Pixels[(y * Width + x) * 4] == 255 ? 1u : 0u;
        }
        std::printf("white pixels: left half %u of %u, right half %u of %u\n", whiteLeft, Width * Height / 2, whiteRight, Width * Height / 2);
        for (std::uint32_t y = 0; y < Height; ++y) {
            for (std::uint32_t x = 0; x < Width; ++x) {
                const auto value = Pixels[(y * Width + x) * 4];
                const bool white = value == 255;
                const bool expected = x < Width / 2;
                Require(white == expected, "depth clear draw: pixel (" + std::to_string(x) + ", " + std::to_string(y) + ") is " + std::to_string(value) + (expected ? ", expected white (cleared to 1.0)" : ", expected untouched (depth 0.0)"));
            }
        }
        std::puts("depth clear draw tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
