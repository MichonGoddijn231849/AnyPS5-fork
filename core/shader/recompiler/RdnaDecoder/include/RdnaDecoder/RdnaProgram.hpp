#ifndef CORE_SHADER_RECOMPILIER_RDNADECODER_INCLUDE_RDNADECODER_RDNAPROGRAM_HPP
#define CORE_SHADER_RECOMPILIER_RDNADECODER_INCLUDE_RDNADECODER_RDNAPROGRAM_HPP

#include "RdnaDecoder/RdnaInstruction.hpp"
#include <cstdint>
#include <vector>
#include <span>

namespace ShaderRecompiler {

struct RdnaProgram {
    std::span<const std::uint32_t> code;
    // The caller's code followed by its inlined callees' copies, when InlineSubroutineCalls added any (`code` views it).
    std::vector<std::uint32_t> ownedCode;
    std::vector<RdnaInstruction> instructions;
};

[[nodiscard]] const RdnaInstruction* FindInstructionAtProgramCounter(const RdnaProgram& program, std::uint32_t programCounter);

}

#endif
