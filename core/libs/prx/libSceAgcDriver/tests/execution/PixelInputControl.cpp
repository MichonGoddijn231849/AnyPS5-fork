#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 64;
constexpr std::uint32_t Height = 32;
alignas(256) std::array<std::byte, Width * Height * 4> Pixels{};
alignas(256) std::array<std::array<float, 4>, 3> Triangle{{{-1.0f, -1.0f, 0.5f, 1.0f}, {3.0f, -1.0f, 0.5f, 1.0f}, {-1.0f, 3.0f, 0.5f, 1.0f}}};

alignas(256) constexpr std::array<std::uint32_t, 6> PositionVertexCode{
    0xe0382000, 0x80000005, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 12> ParameterVertexCode{
    0xe0382000, 0x80000005, 0x7e0c02f2, 0x7e0e0280, 0xf800020f, 0x07070707, 0xf800021f, 0x06060706, 0xbf8c3f70, 0xf80008cf, 0x03020100, 0xbf810000,
};

alignas(256) constexpr std::array<std::uint32_t, 7> PixelCode{
    0xc8020002, 0xc8060102, 0xc80a0202, 0xc80e0302, 0xf800180f, 0x03020100, 0xbf810000,
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t stride, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%x", value);
    return text;
}

void Draw(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> vertexCode, std::uint32_t control, std::uint32_t expectedDefaulted) {
    Pixels.fill(std::byte{0x55});
    const auto target = device.Target();
    std::vector<std::uint32_t> vertexUserData(4, 0u);
    const auto vertexBuffer = BufferDescriptor(Triangle.data(), 16u, static_cast<std::uint32_t>(Triangle.size()));
    std::copy(vertexBuffer.begin(), vertexBuffer.end(), vertexUserData.begin());
    const std::array<ShaderRecompiler::MemoryRegion, 1> vertexMemory{{{reinterpret_cast<std::uintptr_t>(vertexCode.data()), std::as_bytes(vertexCode)}}};
    ShaderRecompiler::RecompileRequest vertex{
        {ShaderStage::Vertex, reinterpret_cast<std::uintptr_t>(vertexCode.data()), vertexCode, 0, {}},
        {64, 0, vertexUserData, std::nullopt, std::nullopt, ShaderRecompiler::ShaderVertexStageInfo{}, vertexMemory},
        target,
        {0, 0, 0, 64}
    };
    vertex.useCache = false;
    const auto vertexResult = ShaderRecompiler::Recompile(vertex);
    const auto vertexPush = static_cast<std::uint32_t>(vertexResult.pushConstants.size());

    ShaderRecompiler::ShaderPixelStageInfo decoded{};
    decoded.interpolatorCount = 1;
    decoded.interpolatorSettings[0] = control;
    decoded.targetOutputMode[0] = 9;
    decoded.targetExportMapping.fill(0xe4u);
    std::uint32_t defaulted = 0;
    const auto pixel = AgcDriver::Graphics::LinkPixelInputs(decoded, vertexResult.parameterExports, defaulted);
    Require(defaulted == expectedDefaulted, "SPI_PS_INPUT_CNTL " + Hex(control) + ": " + std::to_string(defaulted) + " inputs defaulted for unexported parameters");
    const std::array<ShaderRecompiler::MemoryRegion, 1> pixelMemory{{{reinterpret_cast<std::uintptr_t>(PixelCode.data()), std::as_bytes(std::span(PixelCode))}}};
    ShaderRecompiler::RecompileRequest fragment{
        {ShaderStage::Fragment, reinterpret_cast<std::uintptr_t>(PixelCode.data()), PixelCode, 0, {}},
        {64, 0, {}, std::nullopt, pixel, std::nullopt, pixelMemory},
        target,
        {0, 0, vertexPush, 128 - vertexPush}
    };
    fragment.useCache = false;
    const auto pixelResult = ShaderRecompiler::Recompile(fragment);
    const std::array<AgcDriver::Graphics::CompiledShader, 2> shaders{{
        {ShaderStage::Vertex, &vertexResult, 0},
        {ShaderStage::Fragment, &pixelResult, vertexPush}
    }};

    AgcDriver::Graphics::State state{};
    state.stages = {AgcDriver::Graphics::ShaderPath::Vertex, 0u, 64u, 64u, std::nullopt, std::nullopt};
    state.color = {reinterpret_cast<std::uintptr_t>(Pixels.data()), {Width, Height}, VK_FORMAT_R8G8B8A8_UNORM, Pixels.size(), 0xe4u};
    state.colors = {state.color};
    state.hasColorTarget = true;
    state.renderExtent = {Width, Height};
    state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    state.viewport = {0, static_cast<float>(Height), static_cast<float>(Width), -static_cast<float>(Height), 0, 1};
    state.negativeOneToOne = false;
    state.scissor = {{0, 0}, {Width, Height}};
    state.cullMode = VK_CULL_MODE_NONE;
    state.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    state.blend.colorWriteMask = 15;
    state.blends = {state.blend};
    state.blendConstants = {};
    const AgcDriver::Pm4::DrawParameters draw{0, static_cast<std::uint32_t>(Triangle.size()), 0, 1, 0, false};
    device.Draw(state, draw, shaders);
    device.WaitIdle();
}

void Check(std::uint32_t control, std::array<std::uint8_t, 4> expected) {
    for (std::uint32_t pixel = 0; pixel < Width * Height; ++pixel) {
        for (std::uint32_t channel = 0; channel < 4; ++channel) {
            const auto value = std::to_integer<std::uint8_t>(Pixels[pixel * 4u + channel]);
            Require(value == expected[channel], "SPI_PS_INPUT_CNTL " + Hex(control) + ": pixel " + std::to_string(pixel) + " channel " + std::to_string(channel) + " is " + std::to_string(value) + ", expected " + std::to_string(expected[channel]));
        }
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        const std::array<std::array<std::uint8_t, 4>, 4> defaults{{{0, 0, 0, 0}, {0, 0, 0, 255}, {255, 255, 255, 0}, {255, 255, 255, 255}}};
        for (std::uint32_t value = 0; value < defaults.size(); ++value) {
            const auto control = 0x20u | (value << 8u);
            Draw(*device, ParameterVertexCode, control, 0);
            Check(control, defaults[value]);
        }
        Draw(*device, ParameterVertexCode, 0x401u, 0);
        Check(0x401u, {255, 0, 255, 255});
        Draw(*device, ParameterVertexCode, 0x400u, 0);
        Check(0x400u, {0, 0, 0, 0});
        Draw(*device, PositionVertexCode, 0x400u, 1);
        Check(0x400u, {0, 0, 0, 0});
        Draw(*device, ParameterVertexCode, 0x402u, 1);
        Check(0x402u, {0, 0, 0, 0});
        std::puts("pixel input control tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
