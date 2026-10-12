#ifndef CORE_SHADER_RECOMPILER_OPTIMIZATION_WIDEFEEDBACK_HPP
#define CORE_SHADER_RECOMPILER_OPTIMIZATION_WIDEFEEDBACK_HPP

#include <cstdint>

namespace ShaderRecompiler::WideFeedback {

// Which entries of the wide bindless tables (APS5_WIDE_BINDLESS) shaders sample: a wide access stores 1 at its
// table's region + key in a device-wide buffer (the WideFeedback binding) that the host reads, so a table binds only
// the entries the GPU has sampled (a first access reads zeros once). Word 0 takes the stores of unmapped keys.
inline constexpr std::uint32_t Capacity = 4u << 20u;

// The host view of the buffer (null disables the feedback: every entry is bound). Forgets the regions.
void SetMemory(const volatile std::uint32_t* words, std::uint32_t count);
[[nodiscard]] bool Enabled();
// The first flag of the table at `table` (its heap base) with `entries` entries, 0 when the buffer is full.
[[nodiscard]] std::uint32_t Region(std::uint64_t table, std::uint32_t entries);
[[nodiscard]] bool Touched(std::uint32_t region, std::uint32_t key);

}

#endif
