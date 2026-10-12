#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

using AgcDriver::Graphics::Require;
using namespace ShaderRecompiler;

// With APS5_WIDE_BINDLESS the table has 300 entries, the textures at keys 5 and 290 (past the slot heap) and key
// 150 empty; without it the 4 entries hold the textures at keys 0 and 1.
constexpr std::uint32_t WideEntries = 300u;
constexpr std::array<std::uint32_t, 3> WideKeys{5u, 290u, 150u};

struct alignas(4096) GuestData {
    std::array<std::array<std::uint32_t, 8>, WideEntries> heap{};
    std::array<std::uint32_t, 12> material{};
    std::array<std::uint32_t, 16> srt{};
    std::array<std::uint32_t, 64> output{};
};

alignas(256) std::array<std::array<std::uint32_t, 64>, 2> textures{};
std::array<GuestData, 12> guest;
constexpr std::array<std::uint32_t, 23> code{0xbf068002u, 0xbf850013u, 0xf4080100u, 0xfa000000u, 0xf4080200u, 0xfa000010u, 0xf4080300u, 0xfa000020u, 0xf4080700u, 0xfa000030u, 0x7e200500u, 0x93109010u, 0xf4200406u, 0x20000004u, 0x8f108510u, 0xf42c0502u, 0x20000000u, 0xf09c0f08u, 0x00450000u, 0xe0700000u, 0x80070000u, 0xbf810000u, 0xbf800000u};

std::array<std::uint32_t, 4> Buffer(const void* pointer, std::uint32_t stride, std::uint32_t records) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) | (stride << 16u), records, stride == 0u ? 0x31016facu : 0x01016facu};
}

void Run(AgcDriver::VulkanDevice& device) {
    textures[0].fill(0x40000000u);
    textures[1].fill(0x40400000u);
    const auto threads = std::min(32u, device.Target().subgroupSize);
    const bool wide = std::getenv("APS5_WIDE_BINDLESS") != nullptr;
    const bool feedback = wide && std::getenv("APS5_NO_WIDE_FEEDBACK") == nullptr;
    const auto entries = wide ? WideEntries : 4u;
    const auto heapKey = [&](std::uint32_t key) { return wide && key < WideKeys.size() ? WideKeys[key] : key; };
    CompiledShaderArtifact artifact;
    for (std::uint32_t iteration = 0u; iteration < guest.size(); ++iteration) {
        auto& data = guest[iteration];
        const auto table = Buffer(data.heap.data(), 32u, entries);
        const auto material = Buffer(data.material.data(), 16u, 3u);
        std::copy(table.begin(), table.end(), data.srt.begin());
        std::copy(material.begin(), material.end(), data.srt.begin() + 8u);
        const auto srtAddress = reinterpret_cast<std::uintptr_t>(data.srt.data());
        const std::array<std::uint32_t, 3> userData{static_cast<std::uint32_t>(srtAddress), static_cast<std::uint32_t>(srtAddress >> 32u), iteration % 6u == 0u ? 0u : 1u};
        const std::array regions{MemoryRegion{reinterpret_cast<std::uintptr_t>(&data), std::as_bytes(std::span(&data, 1u))}};
        const auto key = iteration % 4u;
        if (key < 2u) {
            for (std::uint32_t texture = 0u; texture < textures.size(); ++texture) {
                const auto address = reinterpret_cast<std::uintptr_t>(textures[texture].data());
                data.heap[heapKey(texture)] = {static_cast<std::uint32_t>(address >> 8u), static_cast<std::uint32_t>(address >> 40u) | ((texture == 0u ? 20u : 22u) << 20u) | (3u << 30u), 7u, 0x90000facu, 0u, 0u, 0u, 0u};
            }
        }
        data.material[1] = key == 3u ? 0xffffffffu : heapKey(key);
        data.material[5] = 0u;
        data.material[9] = 1u;
        data.output.fill(0xdeadbeefu);
        const auto output = Buffer(data.output.data(), 0u, 256u);
        std::copy(output.begin(), output.end(), data.srt.begin() + 12u);
        const ShaderComputeStageInfo compute{{threads, 1u, 1u}, 0u, {false, false, false}, false, 1u};
        RecompileRequest request{{ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0u, {}}, {32u, 0u, userData, compute, std::nullopt, std::nullopt, regions}, device.Target(), {0u, 0u, 0u, 128u}};
        const auto shader = Recompile(request);
        if (artifact.variantId == 0u) artifact = shader;
        else Require(shader.cacheHit && shader.variantId == artifact.variantId, "bindless table changed the compiled artifact");
        if (wide) {
            // A wide table reads no runtime metadata: its entries are WideImages elements at the key.
            Require(shader.runtimeImageResources.empty(), "wide bindless table kept runtime image metadata");
            const auto binding = std::ranges::find_if(shader.bindings, [](const DescriptorBinding& value) { return RuntimeAbi::IsWideImages(static_cast<RuntimeAbi::Binding>(value.binding % static_cast<std::uint32_t>(RuntimeAbi::Binding::Count))); });
            Require(binding != shader.bindings.end(), "wide bindless table has no WideImages binding");
            // Only bound entries take elements: at least the 16 of the smallest array.
            const auto least = 16u;
            Require(binding->count >= least && binding->guestDescriptor.size() == binding->count * 8u, "wide bindless table binds " + std::to_string(binding->count) + " WideImages elements with " + std::to_string(binding->guestDescriptor.size()) + " words at " + std::to_string(iteration));
        } else if (iteration == 1u) {
            auto invalid = shader;
            auto binding = std::ranges::find_if(invalid.bindings, [](const DescriptorBinding& value) { return value.role == DescriptorRole::ShaderData; });
            Require(binding != invalid.bindings.end(), "bindless shader has no runtime metadata");
            const auto offset = invalid.imageMetadataDword + invalid.runtimeImageResources.at(0) * (sizeof(RuntimeAbi::ResourceMetadata) / sizeof(std::uint32_t)) + offsetof(RuntimeAbi::ResourceMetadata, firstElement) / sizeof(std::uint32_t);
            binding->guestDescriptor.at(offset) = RuntimeAbi::SampledHeapCapacity;
            bool rejected = false;
            try {
                device.Dispatch(invalid, 1u, 1u, 1u);
            } catch (const std::exception& error) {
                rejected = std::string_view(error.what()).find("runtime metadata exceeds its bound heap") != std::string_view::npos;
            }
            Require(rejected, "driver accepted an out-of-range runtime heap index");
        }
        device.Dispatch(shader, 1u, 1u, 1u);
        device.SubmitRecorded(false);
        if (feedback) {
            // The first dispatch over a table samples null entries and flags the keys it reads; once it is done,
            // the table binds them and the dispatch reads the textures.
            device.WaitIdle();
            data.output.fill(0xdeadbeefu);
            device.Dispatch(Recompile(request), 1u, 1u, 1u);
            device.SubmitRecorded(false);
        }
    }
    device.WaitIdle();
    std::string failures;
    for (std::uint32_t iteration = 0u; iteration < guest.size(); ++iteration) {
        const auto expected = iteration % 6u == 0u ? 0xdeadbeefu : iteration % 4u < 2u ? textures[iteration % 2u][0] : 0u;
        if (guest[iteration].output[0] != expected) failures += " " + std::to_string(iteration) + ": value=" + std::to_string(guest[iteration].output[0]) + " expected=" + std::to_string(expected);
    }
    Require(failures.empty(), "bindless resource selection or lifetime failed at" + failures);
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Run(*device);
        std::cout << "bindless image materialization tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
