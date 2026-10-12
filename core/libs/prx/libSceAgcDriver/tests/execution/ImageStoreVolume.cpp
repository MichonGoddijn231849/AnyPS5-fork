#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Texture.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
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
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

// image_store over a whole thick SW_4KB_S volume from many workgroups (Wolverine's froxel fog volumes: 160x90x64
// 10_11_11 float, 4x4x2 or 4x4x4 threads per group): every texel of every slab must reach guest memory.

namespace {

using namespace AgcDriver::Graphics;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Width = 160;
constexpr std::uint32_t Height = 90;
constexpr std::uint32_t Depth = 64;
constexpr std::uint32_t Format10_11_11Float = 36;
constexpr std::uint32_t Type3D = 10;
constexpr std::uint32_t TileStandard4KB = 0x05;
constexpr std::size_t BlockBytes = 4u << 20u;

// v_lshl_add_u32 v0, s8, 2, v0; v_lshl_add_u32 v1, s9, 2, v1; v_lshl_add_u32 v2, s10, <z shift>, v2;
// v_mov_b32 v3..v5, 1.0; image_store v[3:5], v[0:2], s[0:7] dmask:0x7 dim:3D; s_endpgm
alignas(256) constexpr std::array<std::uint32_t, 13> StoreZ2Code{
    0xd7460000, 0x04010408, 0xd7460001, 0x04050409, 0xd7460002, 0x0409020a, 0x7e0602f2, 0x7e0802f2,
    0x7e0a02f2, 0xf0200710, 0x00000300, 0xbf810000, 0x00000000,
};
alignas(256) constexpr std::array<std::uint32_t, 13> StoreZ4Code{
    0xd7460000, 0x04010408, 0xd7460001, 0x04050409, 0xd7460002, 0x0409040a, 0x7e0602f2, 0x7e0802f2,
    0x7e0a02f2, 0xf0200710, 0x00000300, 0xbf810000, 0x00000000,
};
// The bounds check of Wolverine's froxel light injection (11d02d3b00, wave32): s[8:10] = width, height and depth,
// group ids in s[12:14].
// v_lshl_add_u32 v0, s12, 2, v0; v_lshl_add_u32 v1, s13, 2, v1; v_lshl_add_u32 v2, s14, 1, v2;
// v_cmp_le_u32 vcc_lo, s10, v2; v_cmp_le_u32_sdwa s16, s9, v1; v_cmp_le_u32_sdwa vcc_hi, s8, v0;
// s_or_b32 vcc_lo, s16, vcc_lo; s_nor_b32 s17, vcc_hi, vcc_lo; s_mov_b32 exec_lo, s17;
// v_mov_b32 v3..v5, 1.0; image_store v[3:5], v[0:2], s[0:7] dmask:0x7 dim:3D; s_endpgm
alignas(256) constexpr std::array<std::uint32_t, 22> BoundsCode{
    0xd7460000, 0x0401040c, 0xd7460001, 0x0405040d, 0xd7460002, 0x0409020e, 0x7d86040a, 0x7d8602f9,
    0x06869009, 0x7d8600f9, 0x0686eb08, 0x886a6a10, 0x8d116a6b, 0xbefe0311, 0x7e0602f2, 0x7e0802f2,
    0x7e0a02f2, 0xf0200710, 0x00000300, 0xbf810000, 0x00000000, 0x00000000,
};

class GuestBlock {
public:
    GuestBlock() {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(65536, BlockBytes));
#endif
        Require(block != nullptr, "image store volume: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, true);
    }
    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
#ifdef _WIN32
        VirtualFree(block, 0, MEM_RELEASE);
#else
        std::free(block);
#endif
    }
    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;
    std::uint8_t* Data() { return block; }
private:
    std::uint8_t* block = nullptr;
};

std::array<std::uint32_t, 8> TextureDescriptor(const void* data) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(data));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (Format10_11_11Float << 20u) | (((Width - 1u) & 3u) << 30u),
        ((Width - 1u) >> 2u) | ((Height - 1u) << 14u),
        0xfacu | (TileStandard4KB << 20u) | (Type3D << 28u),
        Depth - 1u, 0u, 0u, 0u,
    };
}

std::uint64_t EquationOffset(const TextureSwizzleEquation& equation, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    std::uint64_t offset = 0;
    for (std::uint32_t bit = 0; bit < 16; ++bit) {
        const auto mask = equation.bits[bit];
        const auto selected = (x & (mask & 0xfffu)) ^ ((y << 12) & (mask & 0xfff000u)) ^ ((z << 24) & (mask & 0xff000000u));
        offset |= static_cast<std::uint64_t>(std::popcount(selected) & 1u) << bit;
    }
    return offset;
}

void Run(AgcDriver::VulkanDevice& device, std::uint8_t* texels, std::span<const std::uint32_t> code, std::uint32_t wave, std::uint32_t threadsZ, std::span<const std::uint32_t> extraUserData, const std::string& what) {
    const auto texture = TextureDescriptor(texels);
    const auto descriptor = DecodeTextureResource(texture);
    const auto geometry = DescribeSurface(descriptor);
    Require(geometry.thick && geometry.guestBytes <= BlockBytes, what + ": the volume is not a thick surface that fits the guest block");
    std::memset(texels, 0, static_cast<std::size_t>(geometry.guestBytes));
    std::vector<std::uint32_t> userData(texture.begin(), texture.end());
    userData.insert(userData.end(), extraUserData.begin(), extraUserData.end());
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{4, 4, threadsZ}, 0u, {true, true, true}, false, 3};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {wave, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, Width / 4u, (Height + 3u) / 4u, Depth / threadsZ, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
    StorageTexture::FlushPending(reinterpret_cast<std::uintptr_t>(texels), static_cast<std::size_t>(geometry.guestBytes), nullptr, "test");
    device.WaitIdle();
    const auto* equation = FindTextureSwizzleEquation(0x100u | TileStandard4KB, 4u);
    Require(equation != nullptr, what + ": the thick SW_4KB_S equation is missing");
    const auto& mip = geometry.mips.at(0);
    const auto block = ThickBlockExtent(descriptor.tileMode, 4u);
    std::array<std::uint32_t, Depth> missing{};
    std::string first;
    for (std::uint32_t z = 0; z < Depth; ++z) {
        for (std::uint32_t y = 0; y < Height; ++y) {
            for (std::uint32_t x = 0; x < Width; ++x) {
                const auto offset = geometry.GuestLayerOffset(z) + mip.tiledOffset + (static_cast<std::uint64_t>(y / block[1]) * mip.blocksPerRow + x / block[0]) * 4096u + EquationOffset(*equation, x, y, z);
                std::uint32_t value = 0;
                std::memcpy(&value, texels + offset, 4);
                if (value != 0) continue;
                if (first.empty()) first = "(" + std::to_string(x) + ", " + std::to_string(y) + ", " + std::to_string(z) + ") at offset " + std::to_string(offset);
                ++missing[z];
            }
        }
    }
    std::string slices;
    for (std::uint32_t z = 0; z < Depth; ++z) {
        if (missing[z] != 0) slices += " z" + std::to_string(z) + ":" + std::to_string(missing[z]);
    }
    Require(first.empty(), what + ": texels never reached guest memory, first " + first + "; missing per slice" + slices);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock block;
        Run(*device, block.Data(), StoreZ2Code, 32u, 2u, {}, "image_store to a thick SW_4KB_S volume from 4x4x2 wave32 groups");
        Run(*device, block.Data(), StoreZ4Code, 64u, 4u, {}, "image_store to a thick SW_4KB_S volume from 4x4x4 wave64 groups");
        constexpr std::array<std::uint32_t, 4> bounds{Width, Height, Depth, 0u};
        Run(*device, block.Data(), BoundsCode, 32u, 2u, bounds, "image_store behind a wave32 bounds check that writes vcc_hi through VOPC SDWA");
        std::puts("image store volume tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
