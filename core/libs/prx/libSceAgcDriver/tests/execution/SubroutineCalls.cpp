// Subroutine calls (s_swappc_b64, docs/dev/NIGHT_SHIFT.md C1; ControlFlow/src/SubroutineCalls.cpp). Two kernels:
// (1) the Wolverine form: the callee's address comes in user data (s[8:9]), its code is a memory region of its own,
// and it is called twice, so each call site needs its own copy and its own return; (2) a callee in the caller's code
// after s_endpgm, reached through s_getpc_b64 + s_add_u32/s_addc_u32. The callee adds 7 and doubles v1. Then the forms
// Wolverine's menu shaders use: (3) a callee that returns with s_swappc_b64 null, s[30:31] and ends in s_code_end
// padding; (4) a callee that saves its return pair, calls another callee (its address in s[10:11]), puts the pair back
// and returns the same way.
#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 32;
alignas(256) std::array<std::uint32_t, Threads> Output{};

// v1 = lane; s[30:31] = s[8:9]; call; s[30:31] = s[8:9] again; call; buffer_store_dword v1 at lane * 4; s_endpgm.
alignas(256) constexpr std::array<std::uint32_t, 11> UserDataCallCode{
    0x7e020300, // v_mov_b32 v1, v0
    0xbe9e0308, // s_mov_b32 s30, s8
    0xbe9f0309, // s_mov_b32 s31, s9
    0xbe9e211e, // s_swappc_b64 s[30:31], s[30:31]
    0xbe9e0308, // s_mov_b32 s30, s8
    0xbe9f0309, // s_mov_b32 s31, s9
    0xbe9e211e, // s_swappc_b64 s[30:31], s[30:31]
    0x34040082, // v_lshlrev_b32 v2, 2, v0
    0xe0701000, 0x80010102, // buffer_store_dword v1, v2, s[4:7], 0 offen
    0xbf810000, // s_endpgm
};

// v1 = (v1 + 7) * 2; return.
alignas(256) constexpr std::array<std::uint32_t, 3> CalleeCode{
    0x4a020287, // v_add_nc_u32 v1, 7, v1
    0x34020281, // v_lshlrev_b32 v1, 1, v1
    0xbe80201e, // s_setpc_b64 s[30:31]
};

// UserDataCallCode with a branch before each target move: the targets are traced across control flow.
alignas(256) constexpr std::array<std::uint32_t, 13> BranchCallCode{
    0x7e020300, // 0x00 v_mov_b32 v1, v0
    0xbf820000, // 0x04 s_branch 0x08
    0xbe9e0308, // 0x08 s_mov_b32 s30, s8
    0xbe9f0309, // 0x0c s_mov_b32 s31, s9
    0xbe9e211e, // 0x10 s_swappc_b64 s[30:31], s[30:31]
    0xbf820000, // 0x14 s_branch 0x18
    0xbe9e0308, // 0x18 s_mov_b32 s30, s8
    0xbe9f0309, // 0x1c s_mov_b32 s31, s9
    0xbe9e211e, // 0x20 s_swappc_b64 s[30:31], s[30:31]
    0x34040082, // 0x24 v_lshlrev_b32 v2, 2, v0
    0xe0701000, 0x80010102, // 0x28 buffer_store_dword v1, v2, s[4:7], 0 offen
    0xbf810000, // 0x30 s_endpgm
};

// A loop back to the target move after s8 is overwritten: the target is not the user data on every iteration.
alignas(256) constexpr std::array<std::uint32_t, 7> LoopCallCode{
    0x7e020300, // 0x00 v_mov_b32 v1, v0
    0xbe9e0308, // 0x04 s_mov_b32 s30, s8
    0xbe9f0309, // 0x08 s_mov_b32 s31, s9
    0xbe9e211e, // 0x0c s_swappc_b64 s[30:31], s[30:31]
    0xbe880380, // 0x10 s_mov_b32 s8, 0
    0xbf85fffb, // 0x14 s_cbranch_scc1 0x04
    0xbf810000, // 0x18 s_endpgm
};

// v1 = lane; s[30:31] = the s_getpc_b64 result (0x08) + 0x20, the callee after s_endpgm; call; store; s_endpgm.
alignas(256) constexpr std::array<std::uint32_t, 13> RelativeCallCode{
    0x7e020300, // 0x00 v_mov_b32 v1, v0
    0xbe9e1f00, // 0x04 s_getpc_b64 s[30:31] (= 0x08)
    0x801eff1e, 0x00000020, // 0x08 s_add_u32 s30, s30, 0x20 (the callee at 0x28)
    0x821f801f, // 0x10 s_addc_u32 s31, s31, 0
    0xbe9e211e, // 0x14 s_swappc_b64 s[30:31], s[30:31]
    0x34040082, // 0x18 v_lshlrev_b32 v2, 2, v0
    0xe0701000, 0x80010102, // 0x1c buffer_store_dword v1, v2, s[4:7], 0 offen
    0xbf810000, // 0x24 s_endpgm
    0x4a020287, // 0x28 v_add_nc_u32 v1, 7, v1
    0x34020281, // 0x2c v_lshlrev_b32 v1, 1, v1
    0xbe80201e, // 0x30 s_setpc_b64 s[30:31]
};

// v1 = (v1 + 7) * 2; return through a null-destination swap; s_code_end padding.
alignas(256) constexpr std::array<std::uint32_t, 4> NullSwapCalleeCode{
    0x4a020287, // v_add_nc_u32 v1, 7, v1
    0x34020281, // v_lshlrev_b32 v1, 1, v1
    0xbefd211e, // s_swappc_b64 null, s[30:31]
    0xbf9f0000, // s_code_end
};

// Saves s[30:31], calls the callee at s[10:11], restores s[30:31], v1 = (v1 + 7) * 2, returns like the one above.
alignas(256) constexpr std::array<std::uint32_t, 9> NestedCalleeCode{
    0xbea2041e, // s_mov_b64 s[34:35], s[30:31]
    0xbe9e030a, // s_mov_b32 s30, s10
    0xbe9f030b, // s_mov_b32 s31, s11
    0xbe9e211e, // s_swappc_b64 s[30:31], s[30:31]
    0xbe9e0422, // s_mov_b64 s[30:31], s[34:35]
    0x4a020287, // v_add_nc_u32 v1, 7, v1
    0x34020281, // v_lshlrev_b32 v1, 1, v1
    0xbefd211e, // s_swappc_b64 null, s[30:31]
    0xbf9f0000, // s_code_end
};

// The function table form (Wolverine's 0x11d049aa00): the target pair is loaded per wave from a table (through the
// buffer V# in s[12:15], entry offset s16, which the scan does not resolve); every target the table holds gets a copy
// and a dispatcher. A V# rather than a raw pointer: the test's table is not guest memory an address load can reach.
alignas(256) constexpr std::array<std::uint32_t, 9> TableCallCode{
    0x7e020300, // v_mov_b32 v1, v0
    0xf4240786, 0x20000000, // s_buffer_load_dwordx2 s[30:31], s[12:15], s16
    0xbf8cc07f, // s_waitcnt lgkmcnt(0)
    0xbe9e211e, // s_swappc_b64 s[30:31], s[30:31]
    0x34040082, // v_lshlrev_b32 v2, 2, v0
    0xe0701000, 0x80010102, // buffer_store_dword v1, v2, s[4:7], 0 offen
    0xbf810000, // s_endpgm
};

// v1 = v1 + 1; return.
alignas(256) constexpr std::array<std::uint32_t, 2> IncrementCalleeCode{
    0x4a020281, // v_add_nc_u32 v1, 1, v1
    0xbe80201e, // s_setpc_b64 s[30:31]
};

// v1 = v1 * 2; return.
alignas(256) constexpr std::array<std::uint32_t, 2> DoubleCalleeCode{
    0x34020281, // v_lshlrev_b32 v1, 1, v1
    0xbe80201e, // s_setpc_b64 s[30:31]
};

// Three entries, one per callee, then a null entry.
alignas(256) std::array<std::uint64_t, 4> CallTable{};

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t count) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), count * 4u, 0x31016facu};
}

void Run(AgcDriver::VulkanDevice& device, std::span<const std::uint32_t> code, std::span<const ShaderRecompiler::MemoryRegion> memory, const std::uint32_t* calleeCode = CalleeCode.data(), const std::uint32_t* innerCode = CalleeCode.data(), std::uint32_t entry = 0) {
    Output.fill(0xdeadbeefu);
    std::vector<std::uint32_t> userData(17, 0u);
    const auto table = BufferDescriptor(CallTable.data(), static_cast<std::uint32_t>(CallTable.size() * 2u));
    std::copy(table.begin(), table.end(), userData.begin() + 12);
    userData[16] = entry * static_cast<std::uint32_t>(sizeof(std::uint64_t));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size()));
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    const auto callee = reinterpret_cast<std::uintptr_t>(calleeCode);
    userData[8] = static_cast<std::uint32_t>(callee);
    userData[9] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(callee) >> 32u);
    const auto inner = reinterpret_cast<std::uintptr_t>(innerCode);
    userData[10] = static_cast<std::uint32_t>(inner);
    userData[11] = static_cast<std::uint32_t>(static_cast<std::uint64_t>(inner) >> 32u);
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {32, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void Check(const char* what, std::uint32_t calls) {
    for (std::uint32_t lane = 0; lane < Threads; ++lane) {
        std::uint32_t expected = lane;
        for (std::uint32_t call = 0; call < calls; ++call) expected = (expected + 7u) * 2u;
        Require(Output[lane] == expected, std::string("subroutine calls (") + what + "): lane " + std::to_string(lane) + " is " + std::to_string(Output[lane]) + ", expected " + std::to_string(expected));
    }
}

}

int main() {
#ifdef _WIN32
    _putenv_s("APS5_SUBROUTINE_CALLS", "1");
#else
    setenv("APS5_SUBROUTINE_CALLS", "1", 1);
#endif
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        {
            const std::span<const std::uint32_t> code(UserDataCallCode);
            const std::span<const std::uint32_t> callee(CalleeCode);
            const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}, {reinterpret_cast<std::uintptr_t>(callee.data()), std::as_bytes(callee)}}};
            Run(*device, code, memory);
            Check("user data callee, two calls", 2);
        }
        {
            const std::span<const std::uint32_t> words(RelativeCallCode);
            const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(words.data()), std::as_bytes(words)}}};
            Run(*device, words, memory);
            Check("program counter relative callee, one call", 1);
        }
        {
            const std::span<const std::uint32_t> code(UserDataCallCode);
            const std::span<const std::uint32_t> callee(NullSwapCalleeCode);
            const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}, {reinterpret_cast<std::uintptr_t>(callee.data()), std::as_bytes(callee)}}};
            Run(*device, code, memory, NullSwapCalleeCode.data());
            Check("callee returning through a null-destination swap, two calls", 2);
        }
        {
            const std::span<const std::uint32_t> code(UserDataCallCode);
            const std::span<const std::uint32_t> outer(NestedCalleeCode);
            const std::span<const std::uint32_t> inner(NullSwapCalleeCode);
            const std::array<ShaderRecompiler::MemoryRegion, 3> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}, {reinterpret_cast<std::uintptr_t>(outer.data()), std::as_bytes(outer)}, {reinterpret_cast<std::uintptr_t>(inner.data()), std::as_bytes(inner)}}};
            Run(*device, code, memory, NestedCalleeCode.data(), NullSwapCalleeCode.data());
            Check("nested call, two calls of a callee that calls another", 4);
        }
        {
            const std::span<const std::uint32_t> code(BranchCallCode);
            const std::span<const std::uint32_t> callee(CalleeCode);
            const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}, {reinterpret_cast<std::uintptr_t>(callee.data()), std::as_bytes(callee)}}};
            Run(*device, code, memory);
            Check("user data callee traced past branch targets, two calls", 2);
        }
        {
            // A function table call: each entry offset runs its own callee through the dispatcher.
            const std::span<const std::uint32_t> code(TableCallCode);
            const std::span<const std::uint32_t> add(CalleeCode);
            const std::span<const std::uint32_t> increment(IncrementCalleeCode);
            const std::span<const std::uint32_t> twice(DoubleCalleeCode);
            CallTable = {reinterpret_cast<std::uintptr_t>(add.data()), reinterpret_cast<std::uintptr_t>(increment.data()), reinterpret_cast<std::uintptr_t>(twice.data()), 0u};
            const std::span<const std::uint64_t> table(CallTable);
            const std::array<ShaderRecompiler::MemoryRegion, 5> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}, {reinterpret_cast<std::uintptr_t>(add.data()), std::as_bytes(add)}, {reinterpret_cast<std::uintptr_t>(increment.data()), std::as_bytes(increment)}, {reinterpret_cast<std::uintptr_t>(twice.data()), std::as_bytes(twice)}, {reinterpret_cast<std::uintptr_t>(table.data()), std::as_bytes(table)}}};
            for (std::uint32_t entry = 0; entry < 3; ++entry) {
                Run(*device, code, memory, CalleeCode.data(), CalleeCode.data(), entry);
                for (std::uint32_t lane = 0; lane < Threads; ++lane) {
                    const std::uint32_t expected = entry == 0 ? (lane + 7u) * 2u : entry == 1 ? lane + 1u : lane * 2u;
                    Require(Output[lane] == expected, "subroutine calls (function table entry " + std::to_string(entry) + "): lane " + std::to_string(lane) + " is " + std::to_string(Output[lane]) + ", expected " + std::to_string(expected));
                }
            }
            // An entry that holds no target the table had at compile time skips the call.
            CallTable[1] = 0;
            Run(*device, code, memory, CalleeCode.data(), CalleeCode.data(), 3);
            Check("function table entry without a target", 0);
        }
        {
            // A write that a backward branch carries to the call refuses the user data.
            const std::span<const std::uint32_t> code(LoopCallCode);
            const std::span<const std::uint32_t> callee(CalleeCode);
            const std::array<ShaderRecompiler::MemoryRegion, 2> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}, {reinterpret_cast<std::uintptr_t>(callee.data()), std::as_bytes(callee)}}};
            bool refused = false;
            try {
                Run(*device, code, memory);
            } catch (const std::exception& error) {
                refused = std::string(error.what()).find("indirect call") != std::string::npos;
                if (!refused) throw;
            }
            Require(refused, "subroutine calls: a target overwritten on a loop's back edge was taken as user data");
        }
        {
            // A callee whose code the request lacks is a clear error.
            const std::span<const std::uint32_t> code(UserDataCallCode);
            const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
            bool refused = false;
            try {
                Run(*device, code, memory);
            } catch (const std::exception& error) {
                refused = std::string(error.what()).find("is not in the request") != std::string::npos;
                if (!refused) throw;
            }
            Require(refused, "subroutine calls: a callee outside the request compiled");
        }
        std::puts("subroutine calls tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
