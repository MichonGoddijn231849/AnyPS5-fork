#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t FormatCount = 77;
constexpr std::uint32_t Lanes = 32;

alignas(256) std::array<std::uint32_t, 256> Input{};
alignas(256) std::array<std::uint32_t, Lanes * 4> Output{};

// buffer_load_format_x..xyzw v[4:7] from a V# the shader ORs with s8 (so the descriptor is GPU-selected), stored as
// four dwords per lane.
std::array<std::uint32_t, 9> Kernel(std::uint32_t components) {
    return {
        0x7e100500u,                                    // v_mov_b32 v8, v0
        0x88030803u,                                    // s_or_b32 s3, s3, s8
        0x34040084u,                                    // v_lshlrev_b32 v2, 4, v0
        0xe0000000u | ((components - 1u) << 18u),       // buffer_load_format_{x,xy,xyz,xyzw} v[4:7], off, s[0:3], 0
        0x80000400u,
        0xe0781000u,                                    // buffer_store_dwordx4 v[4:7], v2, s[4:7], 0 offen
        0x80010402u,
        0xbf810000u,                                    // s_endpgm
        0u,
    };
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t records) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), records, 0x31016facu};
}

void SetLayoutSwitch(bool on) {
#ifdef _WIN32
    _putenv_s("APS5_FORMAT_LOAD_LAYOUT_SWITCH", on ? "1" : "0");
#else
    setenv("APS5_FORMAT_LOAD_LAYOUT_SWITCH", on ? "1" : "0", 1);
#endif
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

struct Outcome {
    std::array<std::uint32_t, Lanes * 4> values{};
    std::string error;
    std::size_t words = 0;
};

std::array<std::uint32_t, 4> InputDescriptor(std::uint32_t format, std::uint32_t selectors, std::uint32_t stride, std::uint32_t records) {
    auto input = BufferDescriptor(Input.data(), records);
    input[1] |= stride << 16u;
    input[3] = 0x11800000u | (format << 12u) | selectors;
    return input;
}

// One compile per kernel and path: the V# is user data the shader reads at run time, so each case only rewrites the
// V#'s four words in the push data.
struct Compiled {
    ShaderRecompiler::RecompileResult result;
    std::size_t descriptorOffset = 0;
};

Compiled Compile(AgcDriver::VulkanDevice& device, const std::array<std::uint32_t, 9>& code, bool layoutSwitch) {
    SetLayoutSwitch(layoutSwitch);
    std::vector<std::uint32_t> userData(12, 0u);
    const auto input = InputDescriptor(1u, 0xfacu, 16u, 32u);
    std::copy(input.begin(), input.end(), userData.begin());
    const auto output = BufferDescriptor(Output.data(), Lanes * 16u);
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(std::span(code))}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1u, 1u}, 0u, {false, false, false}, false, 1u};
    ShaderRecompiler::RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}}, {32u, 0u, userData, compute, std::nullopt, std::nullopt, memory}, device.Target(), {0u, 0u, 0u, 128u}};
    request.useCache = false;
    Compiled compiled{ShaderRecompiler::Recompile(request)};
    const auto bytes = std::as_bytes(std::span(input));
    const auto& push = compiled.result.pushConstants;
    const auto found = std::search(push.begin(), push.end(), bytes.begin(), bytes.end());
    Require(found != push.end() && std::search(found + 1, push.end(), bytes.begin(), bytes.end()) == push.end(), "the kernel's V# is not once in its push data");
    compiled.descriptorOffset = static_cast<std::size_t>(found - push.begin());
    return compiled;
}

Outcome Load(AgcDriver::VulkanDevice& device, const Compiled& compiled, std::uint64_t program, std::uint32_t format, std::uint32_t selectors, std::uint32_t stride, std::uint32_t records) {
    auto result = compiled.result;
    const auto input = InputDescriptor(format, selectors, stride, records);
    std::memcpy(result.pushConstants.data() + compiled.descriptorOffset, input.data(), sizeof(input));
    Outcome outcome;
    outcome.words = result.spirv.size();
    Output.fill(0xdeadbeefu);
    try {
        device.Dispatch(result, 1, 1, 1, {}, program);
        device.WaitIdle();
    } catch (const std::exception& error) {
        outcome.error = error.what();
    }
    outcome.values = Output;
    return outcome;
}

void Run(AgcDriver::VulkanDevice& device) {
    std::uint32_t seed = 0x2545f491u;
    for (auto& word : Input) {
        seed ^= seed << 13u;
        seed ^= seed >> 17u;
        seed ^= seed << 5u;
        word = seed;
    }
    for (const auto [index, value] : std::array<std::pair<std::size_t, std::uint32_t>, 10>{{{0, 0u}, {1, 0xffffffffu}, {4, 0x80000000u}, {5, 0x7f800001u}, {8, 0x00000001u}, {9, 0x7fffffffu}, {12, 0x00010000u}, {13, 0x8001ffffu}, {16, 0x7c00fc00u}, {17, 0x03ff83ffu}}}) Input[index] = value;
    if (std::min(Lanes, device.Target().subgroupSize) != Lanes) throw std::runtime_error("the device has no 32-lane subgroups");
    struct Shape {
        std::uint32_t stride;
        std::uint32_t records;
    };
    constexpr std::array<Shape, 2> shapes{{{16u, 32u}, {6u, 20u}}};
    constexpr std::array<std::uint32_t, 4> selectorSets{0xfacu, 0x977u, 0xb08u, 0x91au};
    std::size_t compared = 0;
    std::size_t nonzero = 0;
    std::size_t tableWords = 0;
    std::size_t switchWords = 0;
    for (std::uint32_t components = 1; components <= 4u; ++components) {
        const auto code = Kernel(components);
        const auto program = reinterpret_cast<std::uintptr_t>(code.data());
        const auto tableShader = Compile(device, code, false);
        const auto layoutShader = Compile(device, code, true);
        Require(tableShader.result.spirv != layoutShader.result.spirv, "the layout switch and the table compiled the same SPIR-V");
        for (std::uint32_t format = 0; format <= FormatCount + 1u; ++format) {
            for (std::size_t selector = 0; selector < selectorSets.size(); ++selector) {
                for (std::size_t shape = 0; shape < shapes.size(); ++shape) {
                    const auto table = Load(device, tableShader, program, format, selectorSets[selector], shapes[shape].stride, shapes[shape].records);
                    const auto layout = Load(device, layoutShader, program, format, selectorSets[selector], shapes[shape].stride, shapes[shape].records);
                    const auto what = "format " + std::to_string(format) + ", " + std::to_string(components) + " components, selectors " + Hex(selectorSets[selector]) + ", stride " + std::to_string(shapes[shape].stride);
                    Require(table.error == layout.error, what + ": the table load " + (table.error.empty() ? "ran" : "threw \"" + table.error + "\"") + ", the layout switch " + (layout.error.empty() ? "ran" : "threw \"" + layout.error + "\""));
                    for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
                        for (std::uint32_t component = 0; component < components; ++component) {
                            const auto at = lane * 4u + component;
                            Require(table.values[at] == layout.values[at], what + ": lane " + std::to_string(lane) + " component " + std::to_string(component) + " reads " + Hex(table.values[at]) + " through the table, " + Hex(layout.values[at]) + " through the layout switch");
                            nonzero += table.values[at] != 0u && table.values[at] != 0xdeadbeefu;
                        }
                    }
                    ++compared;
                    if (components == 4u && format == 1u && selector == 0u && shape == 0u) {
                        tableWords = table.words;
                        switchWords = layout.words;
                    }
                }
            }
        }
    }
    Require(nonzero > compared, "the runtime format loads read almost nothing: the comparison proves nothing");
    std::printf("runtime format table: %zu loads equal to the layout switch (%zu SPIR-V words against %zu for a four-component load)\n", compared, tableWords, switchWords);
}

}

int main() {
    try {
#ifdef _WIN32
        _putenv_s("APS5_NO_RESULT_MEMO", "1");
#else
        setenv("APS5_NO_RESULT_MEMO", "1", 1);
#endif
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestAllocations::Mutation().Add(Input.data(), sizeof(Input), true, true, true);
        GuestAllocations::Mutation().Add(Output.data(), sizeof(Output), true, true, true);
        struct Release {
            ~Release() {
                GuestAllocations::Mutation().Remove(Input.data());
                GuestAllocations::Mutation().Remove(Output.data());
            }
        } release;
        Run(*device);
        std::puts("runtime format table tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
