#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERINPUTSTATE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_SHADERINPUTSTATE_HPP

#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include "Recompiler.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace AgcDriver::Graphics {

ShaderRecompiler::ShaderPixelStageInfo DecodePixelStageInfo(const Registers& context, const std::array<std::uint8_t, 8>& exportMappings, bool nullProgram = false);
// APS5_PS_STATE_FROM_SHADER=1: a pixel shader header lists context registers (SPI_PS_IN_CONTROL, SPI_PS_INPUT_ENA/ADDR,
// SPI_SHADER_Z/COL_FORMAT, DB_SHADER_CONTROL, CB_SHADER_MASK, ...) a draw's context may lack (Wolverine's menu: NUM_INTERP 0
// in the context, 1 in the header). ApplyPixelHeaderRegisters writes them into `context` when the header declares more
// interpolants than the context has, and says whether it did.
bool PixelStateFromShaderEnabled();
bool ApplyPixelHeaderRegisters(Registers& context, std::span<const std::byte> header, std::uint64_t headerAddress);
// `pixel` with the SPI_PS_INPUT_CNTL inputs whose parameter `parameterExports` (the previous stage's) lacks set to
// read their default; `defaulted` counts them.
ShaderRecompiler::ShaderPixelStageInfo LinkPixelInputs(const ShaderRecompiler::ShaderPixelStageInfo& pixel, std::span<const std::uint32_t> parameterExports, std::uint32_t& defaulted);
ShaderRecompiler::ShaderComputeStageInfo DecodeComputeStageInfo(const Registers& shader, std::span<const std::byte> header);
// A guest range DecodeVertexStageInfo read (an attribute word, a vertex V#) with the bytes as read:
// the draw cache validates them by value with the stage's capture (design_cpu_final rule RD).
struct DecodeRead {
    std::uint64_t address;
    std::vector<std::byte> bytes;
};
ShaderRecompiler::ShaderVertexStageInfo DecodeVertexStageInfo(std::span<const std::byte> header, std::uint64_t headerAddress, std::span<const std::uint32_t> userData, std::vector<DecodeRead>* reads = nullptr, bool staticAbi = false);

}

#endif
