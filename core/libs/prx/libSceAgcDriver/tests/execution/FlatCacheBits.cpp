#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t Results = 16;
constexpr std::uint32_t Sentinel = 0x05e471e1;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint8_t Fill = 0xcd;
alignas(256) std::array<std::uint32_t, Threads * Results> Output{};

alignas(256) constexpr std::array<std::uint32_t, 58> FlatCacheBitsCode{
    0x34020084, 0x34040086, 0xdc388000, 0x04080001, 0xbf8c3f70, 0xd70f6a1e, 0x02020208, 0x7e3e0209,
    0x503e3e80, 0xdc318000, 0x0a080001, 0xdc328004, 0x0b080001, 0xdc309008, 0x0c080001, 0xdc33900c,
    0x0d080001, 0xdc359004, 0x0e080001, 0xdc228001, 0x10080001, 0xdc259007, 0x11080001, 0xbf8c3f70,
    0xdc739000, 0x00080501, 0xdc628006, 0x00080601, 0xbf8c3f70, 0xdc359000, 0x12080001, 0xdccb9008,
    0x14080701, 0xdcce900c, 0x00080401, 0xbf8c3f70, 0xdc358008, 0x15080001, 0xdc311000, 0x177d001e,
    0xdc730000, 0x007d071e, 0xbf8c0070, 0xdcef1000, 0x187d061e, 0xbf8c0070, 0xdc320000, 0x197d001e,
    0xbf8c0070, 0xe0781000, 0x80010a02, 0xe0781010, 0x80010e02, 0xe0781020, 0x80011202, 0xe0781030,
    0x80011602, 0xbf810000
};

struct Row {
    std::array<std::uint32_t, 4> memory;
    std::array<std::uint32_t, Results> results;
};

constexpr std::array<Row, 32> Rows{{
    {{0x00000001, 0x0000007f, 0x00000080, 0x7fffffff}, {0x00000001, 0x0000007f, 0x00000080, 0x7fffffff, 0x0000007f, 0x00000080, 0x00000000, 0x00000000, 0x0000007f, 0x0080007f, 0x00000080, 0x8000007f, 0x7ffffffe, 0x0000007f, 0x7fffffff, 0x7fffff7f}},
    {{0x0000007f, 0x000000ff, 0x80000000, 0x80ff7f01}, {0x0000007f, 0x000000ff, 0x80000000, 0x80ff7f01, 0x000000ff, 0x80000000, 0x00000000, 0x00000000, 0x000000ff, 0x000000ff, 0x80000000, 0x00ff7f01, 0x80ff7e82, 0x000000ff, 0x80ff7f01, 0x00ff7f01}},
    {{0x00000080, 0x80000000, 0xdeadbeef, 0x000000ff}, {0x00000080, 0x80000000, 0xdeadbeef, 0x000000ff, 0x80000000, 0xdeadbeef, 0x00000000, 0xffffff80, 0x80000000, 0x80ef0000, 0xdeadbeef, 0xdeadbfee, 0x0000007f, 0x80000000, 0x000000ff, 0xdeadbe10}},
    {{0x000000ff, 0x12345678, 0x00000001, 0xdeadbeef}, {0x000000ff, 0x12345678, 0x00000001, 0xdeadbeef, 0x12345678, 0x00000001, 0x00000000, 0x00000012, 0x12345678, 0x12015678, 0x00000001, 0xdeadbef0, 0xdeadbdf0, 0x12345678, 0xdeadbeef, 0xdeadbeee}},
    {{0x7fffffff, 0x80ff7f01, 0x000000ff, 0x00000080}, {0x7fffffff, 0x80ff7f01, 0x000000ff, 0x00000080, 0x80ff7f01, 0x000000ff, 0x000000ff, 0xffffff80, 0x80ff7f01, 0x80ff7f01, 0x000000ff, 0x0000017f, 0x80000081, 0x80ff7f01, 0x00000080, 0x0000007f}},
    {{0x80000000, 0x00000001, 0xffffffff, 0x12345678}, {0x80000000, 0x00000001, 0xffffffff, 0x12345678, 0x00000001, 0xffffffff, 0x00000000, 0x00000000, 0x00000001, 0x00ff0001, 0xffffffff, 0x12345677, 0x92345678, 0x00000001, 0x12345678, 0xedcba987}},
    {{0xffffffff, 0x00000080, 0x80ff7f01, 0x0000007f}, {0xffffffff, 0x00000080, 0x80ff7f01, 0x0000007f, 0x00000080, 0x80ff7f01, 0x000000ff, 0x00000000, 0x00000080, 0x00010080, 0x80ff7f01, 0x80ff7f80, 0x00000080, 0x00000080, 0x0000007f, 0x80ff7f7e}},
    {{0x12345678, 0x7fffffff, 0x0000007f, 0xffffffff}, {0x12345678, 0x7fffffff, 0x0000007f, 0xffffffff, 0x7fffffff, 0x0000007f, 0x00000056, 0x0000007f, 0x7fffffff, 0x7f7fffff, 0x0000007f, 0x0000007e, 0xedcba987, 0x7fffffff, 0xffffffff, 0xffffff80}},
    {{0xdeadbeef, 0xffffffff, 0x7fffffff, 0x00000001}, {0xdeadbeef, 0xffffffff, 0x7fffffff, 0x00000001, 0xffffffff, 0x7fffffff, 0x000000be, 0xffffffff, 0xffffffff, 0xffffffff, 0x7fffffff, 0x80000000, 0x21524112, 0xffffffff, 0x00000001, 0x7ffffffe}},
    {{0x80ff7f01, 0xdeadbeef, 0x12345678, 0x80000000}, {0x80ff7f01, 0xdeadbeef, 0x12345678, 0x80000000, 0xdeadbeef, 0x12345678, 0x0000007f, 0xffffffde, 0xdeadbeef, 0xde78beef, 0x12345678, 0x92345678, 0xff0080ff, 0xdeadbeef, 0x80000000, 0x92345678}},
    {{0x00000000, 0x00000000, 0x00000000, 0x00000000}, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {{0x00000001, 0x0000007f, 0x00000080, 0x7fffffff}, {0x00000001, 0x0000007f, 0x00000080, 0x7fffffff, 0x0000007f, 0x00000080, 0x00000000, 0x00000000, 0x0000007f, 0x0080007f, 0x00000080, 0x8000007f, 0x7ffffffe, 0x0000007f, 0x7fffffff, 0x7fffff7f}},
    {{0x0000007f, 0x000000ff, 0x80000000, 0x80ff7f01}, {0x0000007f, 0x000000ff, 0x80000000, 0x80ff7f01, 0x000000ff, 0x80000000, 0x00000000, 0x00000000, 0x000000ff, 0x000000ff, 0x80000000, 0x00ff7f01, 0x80ff7e82, 0x000000ff, 0x80ff7f01, 0x00ff7f01}},
    {{0x00000080, 0x80000000, 0xdeadbeef, 0x000000ff}, {0x00000080, 0x80000000, 0xdeadbeef, 0x000000ff, 0x80000000, 0xdeadbeef, 0x00000000, 0xffffff80, 0x80000000, 0x80ef0000, 0xdeadbeef, 0xdeadbfee, 0x0000007f, 0x80000000, 0x000000ff, 0xdeadbe10}},
    {{0x000000ff, 0x12345678, 0x00000001, 0xdeadbeef}, {0x000000ff, 0x12345678, 0x00000001, 0xdeadbeef, 0x12345678, 0x00000001, 0x00000000, 0x00000012, 0x12345678, 0x12015678, 0x00000001, 0xdeadbef0, 0xdeadbdf0, 0x12345678, 0xdeadbeef, 0xdeadbeee}},
    {{0x7fffffff, 0x80ff7f01, 0x000000ff, 0x00000080}, {0x7fffffff, 0x80ff7f01, 0x000000ff, 0x00000080, 0x80ff7f01, 0x000000ff, 0x000000ff, 0xffffff80, 0x80ff7f01, 0x80ff7f01, 0x000000ff, 0x0000017f, 0x80000081, 0x80ff7f01, 0x00000080, 0x0000007f}},
    {{0x2265b1f5, 0x91b7584a, 0xd8f16adf, 0xcd613e30}, {0x2265b1f5, 0x91b7584a, 0xd8f16adf, 0xcd613e30, 0x91b7584a, 0xd8f16adf, 0x000000b1, 0xffffff91, 0x91b7584a, 0x91df584a, 0xd8f16adf, 0xa652a90f, 0xaafb8c3b, 0x91b7584a, 0xcd613e30, 0x159054ef}},
    {{0xc386bbc4, 0x1027c4d1, 0x414c343c, 0x1e2feb89}, {0xc386bbc4, 0x1027c4d1, 0x414c343c, 0x1e2feb89, 0x1027c4d1, 0x414c343c, 0x000000bb, 0x00000010, 0x1027c4d1, 0x103cc4d1, 0x414c343c, 0x5f7c1fc5, 0x5aa92fc5, 0x1027c4d1, 0x1e2feb89, 0x5f63dfb5}},
    {{0x7ed4d57b, 0xc2ce6f44, 0x7311d8a3, 0x78e51061}, {0x7ed4d57b, 0xc2ce6f44, 0x7311d8a3, 0x78e51061, 0xc2ce6f44, 0x7311d8a3, 0x000000d5, 0xffffffc2, 0xc2ce6f44, 0xc2a36f44, 0x7311d8a3, 0xebf6e904, 0xfa103ae6, 0xc2ce6f44, 0x78e51061, 0x0bf4c8c2}},
    {{0xa6cecc1b, 0x612e7696, 0xc9e9c616, 0x35bf992d}, {0xa6cecc1b, 0x612e7696, 0xc9e9c616, 0x35bf992d, 0x612e7696, 0xc9e9c616, 0x000000cc, 0x00000061, 0x612e7696, 0x61167696, 0xc9e9c616, 0xffa95f43, 0x8ef0cd12, 0x612e7696, 0x35bf992d, 0xfc565f3b}},
    {{0x18072e8c, 0x7ce42c82, 0x0741c7a8, 0xe4b06ce6}, {0x18072e8c, 0x7ce42c82, 0x0741c7a8, 0xe4b06ce6, 0x7ce42c82, 0x0741c7a8, 0x0000002e, 0x0000007c, 0x7ce42c82, 0x7ca82c82, 0x0741c7a8, 0xebf2348e, 0xcca93e5a, 0x7ce42c82, 0xe4b06ce6, 0xe3f1ab4e}},
    {{0xd5f4b3b2, 0x63ca828d, 0x6ec9d286, 0x9b810e76}, {0xd5f4b3b2, 0x63ca828d, 0x6ec9d286, 0x9b810e76, 0x63ca828d, 0x6ec9d286, 0x000000b3, 0x00000063, 0x63ca828d, 0x6386828d, 0x6ec9d286, 0x0a4ae0fc, 0xc58c5ac4, 0x63ca828d, 0x9b810e76, 0xf548dcf0}},
    {{0xc324c985, 0xc4647159, 0x008a05a6, 0xb2221a58}, {0xc324c985, 0xc4647159, 0x008a05a6, 0xb2221a58, 0xc4647159, 0x008a05a6, 0x000000c9, 0xffffffc4, 0xc4647159, 0xc4a67159, 0x008a05a6, 0xb2ac1ffe, 0xeefd50d3, 0xc4647159, 0xb2221a58, 0xb2a81ffe}},
    {{0x7204e52d, 0x442e3d43, 0xb8b6d8fe, 0xcd447e35}, {0x7204e52d, 0x442e3d43, 0xb8b6d8fe, 0xcd447e35, 0x442e3d43, 0xb8b6d8fe, 0x000000e5, 0x00000044, 0x442e3d43, 0x44fe3d43, 0xb8b6d8fe, 0x85fb5733, 0x5b3f9908, 0x442e3d43, 0xcd447e35, 0x75f2a6cb}},
    {{0x3a902931, 0x9755d4c1, 0xf1fd42a2, 0x1a2b8f1f}, {0x3a902931, 0x9755d4c1, 0xf1fd42a2, 0x1a2b8f1f, 0x9755d4c1, 0xf1fd42a2, 0x00000029, 0xffffff97, 0x9755d4c1, 0x97a2d4c1, 0xf1fd42a2, 0x0c28d1c1, 0xdf9b65ee, 0x9755d4c1, 0x1a2b8f1f, 0xebd6cdbd}},
    {{0xe6c3f339, 0x51431193, 0x07d4bedc, 0x05b6e6e3}, {0xe6c3f339, 0x51431193, 0x07d4bedc, 0x05b6e6e3, 0x51431193, 0x07d4bedc, 0x000000f3, 0x00000051, 0x51431193, 0x51dc1193, 0x07d4bedc, 0x0d8ba5bf, 0x1ef2f3aa, 0x51431193, 0x05b6e6e3, 0x0262583f}},
    {{0x06839eb9, 0xa648a7dd, 0x8a9a021e, 0x025b413f}, {0x06839eb9, 0xa648a7dd, 0x8a9a021e, 0x025b413f, 0xa648a7dd, 0x8a9a021e, 0x0000009e, 0xffffffa6, 0xa648a7dd, 0xa61ea7dd, 0x8a9a021e, 0x8cf5435d, 0xfbd7a286, 0xa648a7dd, 0x025b413f, 0x88c14321}},
    {{0xf06c144a, 0xe1988ad9, 0x619699cf, 0xafbd67f9}, {0xf06c144a, 0xe1988ad9, 0x619699cf, 0xafbd67f9, 0xe1988ad9, 0x619699cf, 0x00000014, 0xffffffe1, 0xe1988ad9, 0xe1cf8ad9, 0x619699cf, 0x115401c8, 0xbf5153af, 0xe1988ad9, 0xafbd67f9, 0xce2bfe36}},
    {{0x37730edf, 0xf8130c42, 0x6c0fd4f5, 0xb9d179e0}, {0x37730edf, 0xf8130c42, 0x6c0fd4f5, 0xb9d179e0, 0xf8130c42, 0x6c0fd4f5, 0x0000000e, 0xfffffff8, 0xf8130c42, 0xf8f50c42, 0x6c0fd4f5, 0x25e14ed5, 0x825e6b01, 0xf8130c42, 0xb9d179e0, 0xd5dead15}},
    {{0x076f3787, 0x8712b8bc, 0x38c0c8fd, 0xc381e88f}, {0x076f3787, 0x8712b8bc, 0x38c0c8fd, 0xc381e88f, 0x8712b8bc, 0x38c0c8fd, 0x00000037, 0xffffff87, 0x8712b8bc, 0x87fdb8bc, 0x38c0c8fd, 0xfc42b18c, 0xbc12b108, 0x8712b8bc, 0xc381e88f, 0xfb412072}},
    {{0x701966a0, 0xf06d3fef, 0x7eed8d14, 0x8d88348a}, {0x701966a0, 0xf06d3fef, 0x7eed8d14, 0x8d88348a, 0xf06d3fef, 0x7eed8d14, 0x00000066, 0xfffffff0, 0xf06d3fef, 0xf0143fef, 0x7eed8d14, 0x0c75c19e, 0x1d6ecdea, 0xf06d3fef, 0x8d88348a, 0xf365b99e}},
    {{0x3bab6c39, 0x587fd280, 0x3b1a11df, 0xad45f23d}, {0x3bab6c39, 0x587fd280, 0x3b1a11df, 0xad45f23d, 0x587fd280, 0x3b1a11df, 0x0000006c, 0x00000058, 0x587fd280, 0x58dfd280, 0x3b1a11df, 0xe860041c, 0x719a8604, 0x587fd280, 0xad45f23d, 0x965fe3e2}}
}};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "flat cache bits: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }

private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x01016facu};
}

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%x", value);
    return text;
}

std::array<std::uint32_t, 4> FinalMemory(const Row& row) {
    const auto [a, b, c, d] = row.memory;
    return {d ^ c, (b & ~0xff0000u) | ((c & 0xffu) << 16u), c + d, d - a};
}

void Dispatch(AgcDriver::VulkanDevice& device, std::uint32_t waveSize, const std::uint8_t* base) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base));
    std::vector<std::uint32_t> userData(10, 0u);
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    userData[8] = static_cast<std::uint32_t>(address);
    userData[9] = static_cast<std::uint32_t>(address >> 32u);
    const std::span<const std::uint32_t> code(FlatCacheBitsCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Run(AgcDriver::VulkanDevice& device, GuestBlock& guest, std::uint32_t waveSize) {
    std::memset(guest.Data(), Fill, BlockBytes);
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        std::memcpy(guest.Data() + tid * 16u, Rows[tid % Rows.size()].memory.data(), 16u);
    }
    Output.fill(Sentinel);
    Dispatch(device, waveSize, guest.Data());
    const auto wave = "flat cache bits: wave" + std::to_string(waveSize) + " lane ";
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto& row = Rows[tid % Rows.size()];
        for (std::uint32_t result = 0; result < Results; ++result) {
            const auto actual = Output[tid * Results + result];
            Require(actual == row.results[result], wave + std::to_string(tid) + " v" + std::to_string(10u + result) + " is " + Hex(actual) + ", expected " + Hex(row.results[result]));
        }
        std::array<std::uint32_t, 4> memory{};
        std::memcpy(memory.data(), guest.Data() + tid * 16u, 16u);
        const auto expected = FinalMemory(row);
        for (std::uint32_t dword = 0; dword < 4u; ++dword) {
            Require(memory[dword] == expected[dword], wave + std::to_string(tid) + " memory dword " + std::to_string(dword) + " is " + Hex(memory[dword]) + ", expected " + Hex(expected[dword]));
        }
    }
    for (std::size_t offset = Threads * 16u; offset < BlockBytes; ++offset) {
        Require(guest.Data()[offset] == Fill, wave + "store outside the rows changed byte " + std::to_string(offset));
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock guest;
        Run(*device, guest, 32);
        Run(*device, guest, 64);
        std::puts("flat cache bits tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
