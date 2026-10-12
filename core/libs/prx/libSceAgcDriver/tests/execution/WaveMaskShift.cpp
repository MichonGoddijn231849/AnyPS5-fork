#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
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

constexpr std::uint32_t Lanes = 64;
alignas(256) std::array<std::uint32_t, Lanes * 2> Output{};

// A wave mask shifted per lane, as GTA V's BVH refit counts the arriving children of a lane's group
// of four: v_lshrrev_b64 v[1:2], v12, <mask> with v12 = lane & 60, the result's low four bits kept.
//   v_and_b32 v12, 60, v0 ; v_lshlrev_b32 v4, 3, v0
//   s_mov_b32 vcc_lo, <low> ; s_mov_b32 vcc_hi, <high> ; s_mov_b32 s4, <low> ; s_mov_b32 s5, <high>
//   v_lshrrev_b64 v[1:2], v12, vcc ; v_and_b32 v1, 15, v1
//   v_lshrrev_b64 v[2:3], v12, s[4:5] ; v_and_b32 v2, 15, v2
//   buffer_store_dword v1, v4, s[12:15], 0 offen ; buffer_store_dword v2, v4, s[12:15], 0 offen offset:4
//   s_endpgm
std::array<std::uint32_t, 24> Code(std::uint64_t mask) {
    const auto low = static_cast<std::uint32_t>(mask);
    const auto high = static_cast<std::uint32_t>(mask >> 32u);
    return {0x361800bcu, 0x34080083u, 0xbeea03ffu, low, 0xbeeb03ffu, high, 0xbe8403ffu, low, 0xbe8503ffu, high,
            0xd7000001u, 0x0000d50cu, 0x3602028fu, 0xd7000002u, 0x0000090cu, 0x3604048fu,
            0xe0701000u, 0x80030104u, 0xe0701004u, 0x80030204u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
}

// The same shift of a vcc that a v_cmp wrote lane by lane: vcc = (lane & bits) != 0.
//   v_and_b32 v12, 60, v0 ; v_lshlrev_b32 v4, 3, v0 ; v_and_b32 v5, <bits>, v0 ; v_cmp_ne_u32 vcc, 0, v5
//   v_lshrrev_b64 v[1:2], v12, vcc ; v_and_b32 v1, 15, v1 ; v_mov_b32 v2, 0 ; stores as above
std::array<std::uint32_t, 24> CompareCode(std::uint32_t bits) {
    return {0x361800bcu, 0x34080083u, 0x360a00ffu, bits, 0x7d8a0a80u, 0xd7000001u, 0x0000d50cu, 0x3602028fu, 0x7e040280u,
            0xe0701000u, 0x80030104u, 0xe0701004u, 0x80030204u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u,
            0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
}

std::array<std::uint32_t, 4> RawDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x01016facu};
}

void Run(AgcDriver::VulkanDevice& device, const std::array<std::uint32_t, 24>& program) {
    alignas(256) static std::array<std::uint32_t, 24> code{};
    code = program;
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(16, 0u);
    const auto output = RawDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * sizeof(std::uint32_t)));
    std::copy(output.begin(), output.end(), userData.begin() + 12);
    const std::span<const std::uint32_t> words(code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(words)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), words, 0, {}},
        {Lanes, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check(std::uint64_t mask, bool sgprs = true) {
    char name[64];
    std::snprintf(name, sizeof(name), "wave mask shift of 0x%016llx", static_cast<unsigned long long>(mask));
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
        const auto expected = static_cast<std::uint32_t>((mask >> (lane & 60u)) & 15u);
        const auto fromVcc = Output[lane * 2];
        const auto fromSgprs = Output[lane * 2 + 1];
        Require(fromVcc == expected, std::string(name) + ": lane " + std::to_string(lane) + " shifted vcc to " + std::to_string(fromVcc) + ", expected " + std::to_string(expected));
        Require(!sgprs || fromSgprs == expected, std::string(name) + ": lane " + std::to_string(lane) + " shifted s[4:5] to " + std::to_string(fromSgprs) + ", expected " + std::to_string(expected));
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        if (device->Target().subgroupSize < 32) {
            std::printf("skipped, subgroup size %u cannot hold a wave32\n", device->Target().subgroupSize);
            return VulkanTestSkipped;
        }
        for (const std::uint64_t mask : {0x0123456789abcdefull, 0xf0f0f0f00f0f0f0full, 0xffffffff00000000ull, 0x00000000ffffffffull, 0x8421842184218421ull}) {
            Run(*device, Code(mask));
            Check(mask);
        }
        for (const std::uint32_t bits : {0x24u, 0x3u, 0x20u, 0x13u, 0x2cu}) {
            std::uint64_t mask = 0;
            for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
                if ((lane & bits) != 0) mask |= std::uint64_t{1} << lane;
            }
            Run(*device, CompareCode(bits));
            Check(mask, false);
        }
        std::puts("wave mask shift tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
