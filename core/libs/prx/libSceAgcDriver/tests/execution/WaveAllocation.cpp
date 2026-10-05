#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <bit>
#include <cstdio>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Lanes = 64;
constexpr std::uint32_t Stride = 2;
constexpr std::uint32_t CounterStart = 100;
alignas(256) std::array<std::uint32_t, Lanes * Stride> Output{};
alignas(256) std::array<std::uint32_t, 4> Counter{};

// A wave-aggregated allocation as GTA V's BVH builder does it: the first active lane adds 4 per active
// lane to a counter, the wave reads the old value back with v_readfirstlane, and each lane takes its
// rank among the active lanes from v_mbcnt.
//   v_lshlrev_b32 v4, 3, v0
//   s_mov_b32 exec_lo, <low> ; s_mov_b32 exec_hi, <high>
//   s_ff1_i32_b64 vcc_lo, exec ; s_bcnt1_i32_b64 s10, exec ; s_lshl_b64 vcc, 1, vcc_lo
//   s_and_saveexec_b64 s[8:9], vcc ; s_lshl_b32 vcc_lo, s10, 2 ; v_mov_b32 v1, vcc_lo
//   buffer_atomic_add v1, off, s[4:7], 0 glc
//   s_mov_b64 exec, s[8:9] ; v_mbcnt_hi_u32_b32 v2, s9, 0 ; s_waitcnt vmcnt(0)
//   v_readfirstlane_b32 vcc_lo, v1 ; v_mbcnt_lo_u32_b32 v1, s8, v2 ; v_mov_b32 v3, vcc_lo
//   buffer_store_dword v1, v4, s[12:15], 0 offen ; buffer_store_dword v3, v4, s[12:15], 0 offen offset:4
//   s_endpgm
// With `groups`, v4 is the lane's global index * 8 (v_mad_u32_u24 v4, s16, 64, v0; v_lshlrev_b32 v4, 3, v4).
std::array<std::uint32_t, 29> Code(std::uint32_t low, std::uint32_t high, bool groups) {
    if (groups) {
        return {0xd7460004u, 0x04018010u, 0x34080883u, 0xbefe03ffu, low, 0xbeff03ffu, high, 0xbeea147eu, 0xbe8a107eu, 0x8fea6a81u,
                0xbe88246au, 0x8f6a820au, 0x7e02026au, 0xe0c84000u, 0x80010100u, 0xbefe0408u, 0xd7660002u, 0x00010009u, 0xbf8c3f70u,
                0x7ed40501u, 0xd7650001u, 0x00020408u, 0x7e06026au, 0xe0701000u, 0x80030104u, 0xe0701004u, 0x80030304u, 0xbf810000u, 0xbf810000u};
    }
    return {0x34080083u, 0xbefe03ffu, low, 0xbeff03ffu, high, 0xbeea147eu, 0xbe8a107eu, 0x8fea6a81u, 0xbe88246au, 0x8f6a820au,
            0x7e02026au, 0xe0c84000u, 0x80010100u, 0xbefe0408u, 0xd7660002u, 0x00010009u, 0xbf8c3f70u, 0x7ed40501u, 0xd7650001u,
            0x00020408u, 0x7e06026au, 0xe0701000u, 0x80030104u, 0xe0701004u, 0x80030304u, 0xbf810000u, 0xbf810000u, 0xbf810000u, 0xbf810000u};
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t count, std::uint32_t stride) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), count, 0x01016facu};
}

void Run(AgcDriver::VulkanDevice& device, std::uint64_t mask, std::uint32_t groups) {
    alignas(256) static std::array<std::uint32_t, 29> code{};
    code = Code(static_cast<std::uint32_t>(mask), static_cast<std::uint32_t>(mask >> 32u), groups > 1);
    Output.fill(0xdeadbeefu);
    Counter.fill(0u);
    Counter[0] = CounterStart;
    std::vector<std::uint32_t> userData(16, 0u);
    const auto counter = BufferDescriptor(Counter.data(), static_cast<std::uint32_t>(Counter.size()), 4u);
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * sizeof(std::uint32_t)), 0u);
    std::copy(counter.begin(), counter.end(), userData.begin() + 4);
    std::copy(output.begin(), output.end(), userData.begin() + 12);
    const std::span<const std::uint32_t> words(code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(words)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0, {groups > 1, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), words, 0, {}},
        {Lanes, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, groups, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check(std::uint64_t mask) {
    const auto name = "wave allocation with exec 0x" + [&] {
        char text[24];
        std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(mask));
        return std::string(text);
    }();
    const auto active = static_cast<std::uint32_t>(std::popcount(mask));
    Require(Counter[0] == CounterStart + 4u * active, name + ": the counter is " + std::to_string(Counter[0]) + ", expected " + std::to_string(CounterStart + 4u * active));
    std::uint32_t rank = 0;
    for (std::uint32_t lane = 0; lane < Lanes; ++lane) {
        const auto slot = Output[lane * Stride];
        const auto base = Output[lane * Stride + 1];
        if (((mask >> lane) & 1u) == 0) {
            Require(slot == 0xdeadbeefu && base == 0xdeadbeefu, name + ": inactive lane " + std::to_string(lane) + " stored");
            continue;
        }
        Require(base == CounterStart, name + ": lane " + std::to_string(lane) + " read the old counter as " + std::to_string(base) + ", expected " + std::to_string(CounterStart));
        Require(slot == rank, name + ": lane " + std::to_string(lane) + " has rank " + std::to_string(slot) + ", expected " + std::to_string(rank));
        ++rank;
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
        for (const std::uint64_t mask : {0xffffffffffffffffull, 0x0000ff0000000000ull, 0x000000ffff000000ull, 0x8000000000000001ull, 0x8000000000000000ull, 0x00000000000000f0ull}) {
            Run(*device, mask, 1);
            Check(mask);
        }
        std::puts("wave allocation tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
