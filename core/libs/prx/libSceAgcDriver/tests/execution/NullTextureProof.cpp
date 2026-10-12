#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/ShaderResources.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
constexpr std::uint32_t Words = 32;
constexpr std::uint32_t Results = 17;
constexpr std::size_t BlockBytes = 65536;
std::uint32_t* Output = nullptr;

alignas(256) constexpr std::array<std::uint32_t, 55> Code{
    0x34060087, 0x7e3c0300, 0x7e3e0280, 0x36420087, 0x7e440280, 0xd5480023, 0x02050700, 0xf0001f08,
    0x00010a1e, 0xf0001f08, 0x00010e1e, 0xf0001a08, 0x0001121e, 0xf0001408, 0x0001141e, 0xf0041f08,
    0x00011521, 0xf0041308, 0x00011921, 0xbf8c3f70, 0xe0701000, 0x80000a03, 0xe0701004, 0x80000b03,
    0xe0701008, 0x80000c03, 0xe070100c, 0x80000d03, 0xe0701010, 0x80000e03, 0xe0701014, 0x80000f03,
    0xe0701018, 0x80001003, 0xe070101c, 0x80001103, 0xe0701020, 0x80001203, 0xe0701024, 0x80001303,
    0xe0701028, 0x80001403, 0xe070102c, 0x80001503, 0xe0701030, 0x80001603, 0xe0701034, 0x80001703,
    0xe0701038, 0x80001803, 0xe070103c, 0x80001903, 0xe0701040, 0x80001a03, 0xbf810000,
};

void Dispatch(AgcDriver::VulkanDevice& device, bool wait) {
    static std::optional<ShaderRecompiler::RecompileResult> compiled;
    std::fill(Output, Output + Threads * Words, 0xdeadbeefu);
    std::vector<std::uint32_t> userData(16, 0u);
    const auto address = reinterpret_cast<std::uintptr_t>(Output);
    userData[0] = static_cast<std::uint32_t>(address);
    userData[1] = static_cast<std::uint32_t>((address >> 32u) & 0xffffu);
    userData[2] = Threads * Words * 4u;
    userData[3] = 0x31016facu;
    const std::span<const std::uint32_t> code(Code);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0u, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    if (!compiled.has_value()) compiled = ShaderRecompiler::Recompile(request);
    device.Dispatch(*compiled, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    if (wait) device.WaitIdle();
}

void CheckZero(const char* when) {
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        for (std::uint32_t index = 0; index < Results; ++index) {
            const auto actual = Output[tid * Words + index];
            Require(actual == 0u, std::string("null texture proof: ") + when + ": thread " + std::to_string(tid) + " result " + std::to_string(index) + " is " + std::to_string(actual) + ", expected 0 from the null texture");
        }
    }
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_NO_DISPATCH_RECIPE", "1");
#else
    setenv("APS5_NO_DISPATCH_RECIPE", "1", 1);
#endif
    const char* proofSetting = std::getenv("APS5_NULL_TEXTURE_PROOF");
    const bool proof = proofSetting == nullptr || (proofSetting[0] != '\0' && proofSetting[0] != '0');
    try {
        auto* block = static_cast<std::uint32_t*>(::operator new(BlockBytes, std::align_val_t{BlockBytes}, std::nothrow));
        Require(block != nullptr, "null texture proof: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, true, false);
        struct Release {
            std::uint32_t* block;
            ~Release() {
                GuestAllocations::Mutation().Remove(block);
                ::operator delete(block, std::align_val_t{BlockBytes});
            }
        } release{block};
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        Output = block;
        AgcDriver::GuestMemory::BumpCollectEpoch();
        Dispatch(*device, true);
        CheckZero("first dispatch");
        const auto before = AgcDriver::Graphics::RevalidateCounters();
        for (int repeat = 0; repeat < 4; ++repeat) {
            AgcDriver::GuestMemory::BumpCollectEpoch();
            Dispatch(*device, true);
            CheckZero("repeat");
        }
        const auto after = AgcDriver::Graphics::RevalidateCounters();
        const auto fast = after.fast - before.fast;
        const auto full = after.full - before.full;
        const auto noRecord = after.noRecord - before.noRecord;
        std::cout << "null texture proof (" << (proof ? "on" : "off") << "): " << fast << " fast, " << full << " full, " << noRecord << " no record\n";
        if (fast + full == 0) {
            std::puts("null texture proof: the dispatches did not reuse a template (no Revalidate): skipped");
            return VulkanTestSkipped;
        }
        if (proof) {
            Require(noRecord == 0 && full == 0 && fast == 4, "null texture proof: with APS5_NULL_TEXTURE_PROOF every repeat must be proved fast");
        } else {
            Require(noRecord == 4 && full == 4, "null texture proof: without the switch every repeat must take the full walk for want of a record");
        }
        std::cout << "null texture proof tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
