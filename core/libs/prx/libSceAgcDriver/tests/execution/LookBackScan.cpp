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

// GTA V's single-pass prefix sum (decoupled look-back) that gives each BVH node its record index
// before the BVH build kernel. A wave64 workgroup takes its block id from a counter
// (buffer_atomic_add glc), scans its 256 elements in place (4 chunks of 64, v_movrels/v_movreld and
// DPP), publishes {flag, value} for its block with buffer_atomic_swap_x2 (1 = the block's own sum,
// 2 = inclusive of all earlier blocks), then has each lane read one of the 64 previous blocks' pairs,
// spinning while any flag is 0. Without an inclusive pair in the window it adds all 64 sums and moves
// the window 64 blocks back; that path only runs with more than 64 blocks. The last block writes the
// total. User data: s[0:3] the counter's V#, s[4:5] the pairs, s[6:7] the elements, s[8:9] the
// total, s10 elements per block, s11 blocks, s12 elements.
// The words are the game's kernel (shader_7ff64803ff00.req).
constexpr std::array<std::uint32_t, 492> ScanCode{
    0xbfa00003u, 0x878e817eu, 0x8811ff05u, 0x00080000u, 0xbefe040eu, 0xbe900304u, 0xbe92030bu, 0xbe9303ffu,
    0x00016204u, 0x8805ff07u, 0x00040000u, 0xbe840306u, 0xbe86030cu, 0xbe8703ffu, 0x00016204u, 0xbf880003u,
    0x7e020281u, 0xe0c84000u, 0x80000100u, 0xbf8c3f70u, 0xd7600002u, 0x00010101u, 0xbefe04c1u, 0x9003860au,
    0xbf0a0380u, 0x93000a02u, 0x4a240000u, 0x7d88240cu, 0xbf840006u, 0x7e020280u, 0xbefe046au, 0xbf880002u,
    0xe0302000u, 0x80010112u, 0xbefe04c1u, 0xd76d0011u, 0x04018000u, 0xbf0a0381u, 0x7d88220cu, 0xbf840006u,
    0x7e040280u, 0xbefe046au, 0xbf880002u, 0xe0302000u, 0x80010211u, 0xbefe04c1u, 0xd76d0010u, 0x0401fe00u,
    0x00000080u, 0xbf0a0382u, 0x7d88200cu, 0xbf840006u, 0x7e060280u, 0xbefe046au, 0xbf880002u, 0xe0302000u,
    0x80010310u, 0xbefe04c1u, 0xd76d000fu, 0x0401fe00u, 0x000000c0u, 0xbf0a0383u, 0x7d881e0cu, 0xbf840006u,
    0x7e080280u, 0xbefe046au, 0xbf880002u, 0xe0302000u, 0x8001040fu, 0xbefe04c1u, 0xd76d000eu, 0x0401fe00u,
    0x00000100u, 0xbf0a0384u, 0x7d881c0cu, 0xbf840006u, 0x7e0a0280u, 0xbefe046au, 0xbf880002u, 0xe0302000u,
    0x8001050eu, 0xbefe04c1u, 0xd76d000du, 0x0401fe00u, 0x00000140u, 0xbf0a0385u, 0x7d881a0cu, 0xbf840006u,
    0x7e0c0280u, 0xbefe046au, 0xbf880002u, 0xe0302000u, 0x8001060du, 0xbefe04c1u, 0xd76d000cu, 0x0401fe00u,
    0x00000180u, 0xbf0a0386u, 0x7d88180cu, 0xbf840006u, 0x7e0e0280u, 0xbefe046au, 0xbf880002u, 0xe0302000u,
    0x8001070cu, 0xbefe04c1u, 0xd76d000bu, 0x0401fe00u, 0x000001c0u, 0xbf0a0387u, 0x7d88160cu, 0xbf840006u,
    0x7e100280u, 0xbefe046au, 0xbf880002u, 0xe0302000u, 0x8001080bu, 0xbefe04c1u, 0xbe8d0380u, 0xbe8a0380u,
    0xbf0a030du, 0xbf840020u, 0xbefc030du, 0x810d810du, 0xbf8c3f70u, 0x7e148701u, 0x7e12030au, 0xbeea287eu,
    0x02281480u, 0x4a2828fau, 0xff011114u, 0x4a2828fau, 0xff011214u, 0x4a2828fau, 0xff011414u, 0x4a2828fau,
    0xff011814u, 0xd7781013u, 0x03058314u, 0x4a2826fau, 0xaf00e414u, 0x92fea0a0u, 0xd7600000u, 0x00013f14u,
    0x4a282800u, 0xbefe046au, 0x4c121314u, 0x4a14130au, 0x4a12120au, 0xd760006au, 0x00017f0au, 0x7e028509u,
    0x810a6a0au, 0xbf82ffdeu, 0xbefe0481u, 0x7e260202u, 0xbf088002u, 0x7e14020au, 0x856a8281u, 0x7e12026au,
    0xe1402000u, 0x80040913u, 0xbf088002u, 0xbe8d0380u, 0xbefe04c1u, 0xbf840058u, 0xd76d0013u, 0x0401fe02u,
    0xffffffc0u, 0xbe8d0380u, 0x7d0626f9u, 0x06868080u, 0x7e120281u, 0x7e140280u, 0xbefe0400u, 0xbf880002u,
    0xe034e000u, 0x80040913u, 0xbefe04c1u, 0xbf8c3f70u, 0x7d041280u, 0xbf13806au, 0xbf840006u, 0xbefe0400u,
    0xbf880002u, 0xe034e000u, 0x80040913u, 0xbefe04c1u, 0xbf82fff6u, 0x7d0412f9u, 0x06868082u, 0xbf138000u,
    0xbf850016u, 0x4e2626c0u, 0xbeea287eu, 0x02121480u, 0x4a1212fau, 0xff011109u, 0x4a1212fau, 0xff011209u,
    0x4a1212fau, 0xff011409u, 0x4a1212fau, 0xff011809u, 0xd778100au, 0x03058309u, 0x4a121509u, 0xbefe046au,
    0xd7600000u, 0x00013f09u, 0xd7600001u, 0x00017f09u, 0x81140100u, 0x810d140du, 0xbf82ffd3u, 0x7e127201u,
    0x7d8a12c1u, 0x4c1212bfu, 0xbf870002u, 0x7e127200u, 0x4c12129fu, 0x7d881300u, 0x7e000509u, 0x02001480u,
    0xbeea287eu, 0xd7600014u, 0x0000010au, 0x02120080u, 0x4a1212fau, 0xff011109u, 0x4a1212fau, 0xff011209u,
    0x4a1212fau, 0xff011409u, 0x4a1212fau, 0xff011809u, 0xd7781000u, 0x03058309u, 0x4a120109u, 0xbefe0481u,
    0xd7600000u, 0x00013f09u, 0xd7600001u, 0x00017f09u, 0x81150100u, 0x816a1514u, 0x810d0d6au, 0x7e000202u,
    0x4a141af9u, 0x8686060au, 0x7e120282u, 0xe1402000u, 0x80040900u, 0xbefe04c1u, 0x7d86240cu, 0xbf090380u,
    0xbf800000u, 0x8580807eu, 0x8dea006au, 0xbefe046au, 0xbf880004u, 0xbf8c3f70u, 0x4a00020du, 0xe0702000u,
    0x80010012u, 0xbefe04c1u, 0xbf090381u, 0x7d86220cu, 0x8580807eu, 0x8dea006au, 0xbefe046au, 0xbf880004u,
    0xbf8c3f70u, 0x4a00040du, 0xe0702000u, 0x80010011u, 0xbefe04c1u, 0xbf090382u, 0x7d86200cu, 0x8580807eu,
    0x8dea006au, 0xbefe046au, 0xbf880004u, 0xbf8c3f70u, 0x4a00060du, 0xe0702000u, 0x80010010u, 0xbefe04c1u,
    0xbf090383u, 0x7d861e0cu, 0x8580807eu, 0x8dea006au, 0xbefe046au, 0xbf880004u, 0xbf8c3f70u, 0x4a00080du,
    0xe0702000u, 0x8001000fu, 0xbefe04c1u, 0xbf090384u, 0x7d861c0cu, 0x8580807eu, 0x8dea006au, 0xbefe046au,
    0xbf880004u, 0xbf8c3f70u, 0x4a000a0du, 0xe0702000u, 0x8001000eu, 0xbefe04c1u, 0xbf090385u, 0x7d861a0cu,
    0x8580807eu, 0x8dea006au, 0xbefe046au, 0xbf880004u, 0xbf8c3f70u, 0x4a000c0du, 0xe0702000u, 0x8001000du,
    0xbefe04c1u, 0xbf090386u, 0x7d86180cu, 0x8580807eu, 0x8dea006au, 0xbefe046au, 0xbf880004u, 0xbf8c3f70u,
    0x4a000e0du, 0xe0702000u, 0x8001000cu, 0xbefe04c1u, 0xbf090387u, 0x7d86160cu, 0x8580807eu, 0x8dea006au,
    0xbefe046au, 0xbf880004u, 0xbf8c3f70u, 0x4a00100du, 0xe0702000u, 0x8001000bu, 0x816ac10bu, 0xbefe04c1u,
    0xbf066a02u, 0xbf800000u, 0x85ea807eu, 0x87ea0e6au, 0xbefe046au, 0xbf880008u, 0x4a0014f9u, 0x8686060du,
    0xbe8a0381u, 0xbe8b03ffu, 0x00016204u, 0xbe891d92u, 0xe0700000u, 0x80020000u, 0xbf810000u, 0xbf9f0000u,
    0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u,
    0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u,
    0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u,
    0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0xbf9f0000u, 0x00000000u, 0x00000000u,
    0x30306c73u, 0x0000013fu, 0x00000156u, 0x00000000u, 0x00102557u, 0x0401a600u, 0x0b100a10u, 0x40100c10u,
    0x4c104b40u, 0x4e104d10u, 0x79606f10u, 0x00107a10u, 0x00006e00u, 0x21200f64u, 0x25402410u, 0x64000010u,
    0x40241021u, 0x28600000u, 0x00102950u, 0x30c9a100u, 0x9e0800c0u, 0xa0841941u, 0x18c08418u, 0x8418c084u,
    0xc08418c0u, 0x18c08418u, 0x8418c084u, 0xe0c01040u, 0x08430820u, 0x08104108u, 0x2334108bu, 0x38838838u,
    0x88388388u, 0x83883883u, 0x80f01040u, 0x98241e05u, 0x0d8442e0u, 0xe1183e0fu, 0x17158644u, 0x46e1985eu,
    0x7e1f1d88u, 0x8a48e218u, 0x989e2725u, 0x2d8c4ae2u, 0xe318be2fu, 0x37358e4cu, 0x4ee398deu, 0xfe3f3d90u,
    0x9250e418u, 0x991e4745u, 0x4f4d92e4u, 0x6a25112eu, 0xe6ab29cau, 0xcb6b32bcu, 0x2afcbb2du, 0x31cc6c33u,
    0xf34b3cbbu, 0xdb36b5cau, 0x6716f378u, 0xddd6739cu, 0x9ede6779u, 0xf9fdf67bu, 0x83a0e067u, 0x687a1e16u,
    0x368ba2e2u, 0xe468fa3eu, 0x5e5693a4u, 0x68a6697au, 0xf7777777u, 0x5554fdddu, 0xcb000205u, 0x70817d02u,
    0x00080019u, 0x003f0146u, 0xa41002a4u, 0x3402a602u, 0xaa000a34u, 0xaa100c02u, 0x3402b002u, 0x1502b631u,
    0x400202bcu, 0x02b60101u, 0x8b2ef007u, 0xd74d0005u, 0xe0828001u, 0x00001601u, 0x00000000u, 0x00000000u,
    0x65726162u, 0x746f6f66u, 0x442e4a87u, 0x00000000u, 0x00000001u, 0x00000618u, 0x00000000u, 0x00000156u,
    0x980d163fu, 0xeddd574eu, 0x00000000u, 0x00000000u,
};

constexpr std::uint32_t Lanes = 64;
constexpr std::uint32_t PerBlock = 256;
constexpr std::uint32_t Blocks = 125;
constexpr std::uint32_t Elements = 31860;

alignas(256) std::array<std::uint32_t, 4> Counter{};
alignas(256) std::array<std::uint32_t, Blocks * 2> Pairs{};
alignas(256) std::array<std::uint32_t, Blocks * PerBlock> Data{};
alignas(256) std::array<std::uint32_t, 4> Total{};

std::uint32_t Value(std::uint32_t index, std::uint32_t seed) {
    return ((index * 2654435761u + seed * 40503u) >> 13u) % 3u;
}

void Run(AgcDriver::VulkanDevice& device, std::uint32_t groups) {
    alignas(256) static std::array<std::uint32_t, ScanCode.size()> code{};
    code = ScanCode;
    const auto address = [](const void* data) { return reinterpret_cast<std::uintptr_t>(data); };
    std::vector<std::uint32_t> userData{
        static_cast<std::uint32_t>(address(Counter.data())), static_cast<std::uint32_t>((address(Counter.data()) >> 32u) & 0xffffu) | 0x00040000u, 1u, 0x00005204u,
        static_cast<std::uint32_t>(address(Pairs.data())), static_cast<std::uint32_t>(address(Pairs.data()) >> 32u),
        static_cast<std::uint32_t>(address(Data.data())), static_cast<std::uint32_t>(address(Data.data()) >> 32u),
        static_cast<std::uint32_t>(address(Total.data())), static_cast<std::uint32_t>(address(Total.data()) >> 32u),
        PerBlock, Blocks, Elements};
    const std::span<const std::uint32_t> words(code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{address(code.data()), std::as_bytes(words)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Lanes, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, address(code.data()), words, 0, {}},
        {Lanes, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, groups, 1, 1, {}, address(code.data()));
    device.WaitIdle();
}

std::uint32_t BlockSum(const std::vector<std::uint32_t>& input, std::uint32_t block) {
    std::uint32_t sum = 0;
    for (std::uint32_t i = block * PerBlock; i < (block + 1) * PerBlock && i < Elements; ++i) sum += input[i];
    return sum;
}

// One workgroup that gets block `block` while blocks 0..block-1 have published (block 0 inclusive,
// the others their own sums), so the look-back walks back exactly as far as it does in the game
// when the earlier blocks have not finished.
void CheckLookBack(AgcDriver::VulkanDevice& device, std::uint32_t block) {
    const auto name = "look-back from block " + std::to_string(block);
    std::vector<std::uint32_t> input(Data.size(), 0u);
    for (std::uint32_t i = 0; i < Elements; ++i) input[i] = Value(i, block);
    Pairs.fill(0u);
    std::uint32_t prefix = 0;
    for (std::uint32_t b = 0; b < block; ++b) {
        const auto sum = BlockSum(input, b);
        prefix += sum;
        Pairs[2 * b] = b == 0 ? 2u : 1u;
        Pairs[2 * b + 1] = b == 0 ? prefix : sum;
    }
    Counter.fill(0u);
    Counter[0] = block;
    std::copy(input.begin(), input.end(), Data.begin());
    Total.fill(0xdeadbeefu);
    Run(device, 1);
    Require(Counter[0] == block + 1, name + ": the counter is " + std::to_string(Counter[0]));
    Require(Pairs[2 * block] == 2u, name + ": its flag is " + std::to_string(Pairs[2 * block]) + ", expected 2");
    Require(Pairs[2 * block + 1] == prefix + BlockSum(input, block), name + ": it published " + std::to_string(Pairs[2 * block + 1]) + ", expected " + std::to_string(prefix + BlockSum(input, block)) + " (prefix " + std::to_string(prefix) + ")");
    std::uint32_t running = prefix;
    for (std::uint32_t i = block * PerBlock; i < (block + 1) * PerBlock && i < Elements; ++i) {
        Require(Data[i] == running, name + ": element " + std::to_string(i) + " is " + std::to_string(Data[i]) + ", expected " + std::to_string(running) + " (prefix " + std::to_string(prefix) + ")");
        running += input[i];
    }
    if (block == Blocks - 1) Require(Total[0] == running, name + ": the total is " + std::to_string(Total[0]) + ", expected " + std::to_string(running));
}

// All 125 workgroups at once, as the game dispatches it.
void CheckWhole(AgcDriver::VulkanDevice& device, std::uint32_t seed) {
    const auto name = "whole scan " + std::to_string(seed);
    std::vector<std::uint32_t> input(Data.size(), 0u);
    for (std::uint32_t i = 0; i < Elements; ++i) input[i] = Value(i, seed);
    Counter.fill(0u);
    Pairs.fill(0u);
    std::copy(input.begin(), input.end(), Data.begin());
    Total.fill(0xdeadbeefu);
    Run(device, Blocks);
    std::uint32_t running = 0;
    for (std::uint32_t i = 0; i < Elements; ++i) {
        Require(Data[i] == running, name + ": element " + std::to_string(i) + " (block " + std::to_string(i / PerBlock) + ") is " + std::to_string(Data[i]) + ", expected " + std::to_string(running));
        running += input[i];
    }
    Require(Total[0] == running, name + ": the total is " + std::to_string(Total[0]) + ", expected " + std::to_string(running));
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
        for (const std::uint32_t block : {0u, 1u, 50u, 63u, 64u, 65u, 100u, 124u}) CheckLookBack(*device, block);
        for (std::uint32_t seed = 0; seed < 50; ++seed) CheckWhole(*device, seed);
        std::puts("look-back scan tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
