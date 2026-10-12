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
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Uint32 = 20;
constexpr std::uint32_t Type2D = 9;
constexpr std::uint32_t IdentitySwizzle = 0xfacu;
constexpr std::uint32_t Width = 4;
constexpr std::uint32_t Stored = 0x42u;
constexpr std::uint32_t Untouched = 0x11111111u;

struct alignas(4096) GuestTables {
    std::array<std::array<std::uint32_t, 8>, 2> heap{};
    std::array<std::array<std::uint32_t, 4>, 3> materials{};
    std::array<std::uint32_t, 16> output{};
    std::array<std::uint32_t, 16> srt{};
};

struct alignas(4096) TableTexture {
    std::array<std::uint32_t, 4096> texels{};
};

GuestTables Tables;
TableTexture TableTextures[2];

alignas(256) const std::array<std::uint32_t, 23> TableStoreCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080300u, 0xfa000020u, 0xf4080700u, 0xfa000030u,
    0x7e200500u, 0x93109010u, 0xf4200406u, 0x20000004u, 0x8f108510u, 0xf42c0502u, 0x20000000u, 0x7e020280u,
    0x7e0402ffu, Stored, 0xf0201108u, 0x00050200u, 0xe0700000u, 0x80070000u, 0xbf810000u,
};

// TableStoreCode with the key read at a constant offset (materials[0].y, no per-lane selector): one key for the whole
// dispatch, which the materializer reads when it binds the table.
alignas(256) const std::array<std::uint32_t, 23> UniformKeyStoreCode{
    0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080300u, 0xfa000020u, 0xf4080700u, 0xfa000030u,
    0xbf800000u, 0xbf800000u, 0xf4200406u, 0xfa000004u, 0x8f108510u, 0xf42c0502u, 0x20000000u, 0x7e020280u,
    0x7e0402ffu, Stored, 0xf0201108u, 0x00050200u, 0xe0700000u, 0x80070000u, 0xbf810000u,
};

std::string Hex(std::uint32_t value) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08x", value);
    return text;
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t records, std::uint32_t stride = 0u) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu) | (stride << 16u), records, stride == 0u ? 0x31016facu : 0x01016facu};
}

std::array<std::uint32_t, 8> TextureDescriptor(const void* texels, std::uint32_t format, std::uint32_t width, std::uint32_t height) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(texels));
    return {
        static_cast<std::uint32_t>(address >> 8u),
        static_cast<std::uint32_t>((address >> 40u) & 0xffu) | (format << 20u) | (((width - 1u) & 3u) << 30u),
        ((width - 1u) >> 2u) | ((height - 1u) << 14u),
        IdentitySwizzle | (Type2D << 28u),
        0u,
        0u,
        0u,
        0u,
    };
}

void Run(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, const char* what) {
    for (std::uint32_t entry = 0; entry < 2u; ++entry) Tables.heap[entry] = TextureDescriptor(TableTextures[entry].texels.data(), Uint32, Width, Width);
    const auto heap = BufferDescriptor(Tables.heap.data(), 2u, 32u);
    const auto materials = BufferDescriptor(Tables.materials.data(), 3u, 16u);
    const auto output = BufferDescriptor(Tables.output.data(), 64u);
    std::copy(heap.begin(), heap.end(), Tables.srt.begin());
    std::copy(materials.begin(), materials.end(), Tables.srt.begin() + 8);
    std::copy(output.begin(), output.end(), Tables.srt.begin() + 12);
    const auto srtAddress = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(Tables.srt.data()));
    const std::vector<std::uint32_t> userData{static_cast<std::uint32_t>(srtAddress), static_cast<std::uint32_t>(srtAddress >> 32u)};
    const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{
        {reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)},
        {reinterpret_cast<std::uintptr_t>(&Tables), std::as_bytes(std::span<const GuestTables, 1>(&Tables, 1))},
    }};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Width, 1, 1}, 0u, {false, false, false}, false, 1};
    for (const std::uint32_t key : {0u, 1u}) {
        for (auto& texture : TableTextures) texture.texels.fill(Untouched);
        Tables.materials = {{{0u, key, 0u, 0u}, {0u, 0u, 0u, 0u}, {0u, 1u, 0u, 0u}}};
        Tables.output.fill(0xdeadbeefu);
        ShaderRecompiler::RecompileRequest request{
            {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
            {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
            device.Target(),
            {0, 0, 0, 128}
        };
        request.useCache = false;
        const auto result = ShaderRecompiler::Recompile(request);
        const auto storage = std::ranges::find_if(result.bindings, [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.kind == ShaderRecompiler::DescriptorKind::StorageImage; });
        Require(storage != result.bindings.end() && storage->count >= 2u, "storage image table: the slots are not bound as storage images");
        device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
        device.WaitIdle();
        for (std::uint32_t entry = 0; entry < 2u; ++entry) {
            for (std::uint32_t x = 0; x < Width; ++x) {
                const auto expected = entry == key ? Stored : Untouched;
                const auto actual = TableTextures[entry].texels[x];
                Require(actual == expected, std::string(what) + " key " + std::to_string(key) + ": entry " + std::to_string(entry) + " texel " + std::to_string(x) + " is " + Hex(actual) + ", expected " + Hex(expected));
            }
        }
        Require(Tables.output[0] == 0u, std::string(what) + " key " + std::to_string(key) + ": lane 0 wrote " + Hex(Tables.output[0]));
    }
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_BINDLESS_STORAGE_TABLES", "1");
    _putenv_s("APS5_EAGER_LINEAR_WRITEBACK", "1");
#else
    setenv("APS5_BINDLESS_STORAGE_TABLES", "1", 1);
    setenv("APS5_EAGER_LINEAR_WRITEBACK", "1", 1);
#endif
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        for (auto& texture : TableTextures) GuestAllocations::Mutation().Add(texture.texels.data(), sizeof(texture.texels), true, true, true);
        struct Release {
            ~Release() {
                for (auto& texture : TableTextures) GuestAllocations::Mutation().Remove(texture.texels.data());
            }
        } release;
        Run(*device, TableStoreCode, "storage image table");
        Run(*device, UniformKeyStoreCode, "storage image table with a uniform key");
        std::puts("bindless storage image table tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
