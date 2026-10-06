#ifndef CORE_SHADER_RECOMPILIER_INTERMEDIATEREPRESENTATION_INCLUDE_INTERMEDIATEREPRESENTATION_IRMETADATA_RESOURCEPLAN_HPP
#define CORE_SHADER_RECOMPILIER_INTERMEDIATEREPRESENTATION_INCLUDE_INTERMEDIATEREPRESENTATION_IRMETADATA_RESOURCEPLAN_HPP

#include "IntermediateRepresentation/IrBlock.hpp"
#include "IntermediateRepresentation/IrValue.hpp"
#include "IntermediateRepresentation/IrMetadata/ControlFlowInfo.hpp"
#include "IntermediateRepresentation/IrMetadata/DescriptorBinding.hpp"
#include "IntermediateRepresentation/IrMetadata/ResourceInfo.hpp"
#include "IntermediateRepresentation/IrMetadata/ShaderInfo.hpp"
#include "IntermediateRepresentation/IrMetadata/ShaderStage.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace ShaderRecompiler {

struct DescriptorValue {
    std::array<std::uint32_t, 8> dwords = {};
    std::uint32_t dwordCount = 0;

    bool operator==(const DescriptorValue& other) const = default;
};

enum class UniformFillKind { None, Buffer, Image };

struct UniformFill {
    UniformFillKind kind = UniformFillKind::None;
    std::uint32_t resource = 0;
    std::array<std::uint32_t, 3> groupStride {};
    std::uint32_t words = 0;
    std::uint32_t value = 0;

    bool operator==(const UniformFill& other) const = default;
};

struct ResourceSnapshot {
    std::vector<DescriptorValue> buffers;
    std::vector<DescriptorValue> images;
    std::vector<DescriptorValue> samplers;
    std::vector<std::uint32_t> flattenedSrt;
    std::vector<std::uint32_t> userData;
    UniformFill uniformFill;
};

struct UniformFillPlan {
    UniformFill fill;
    std::array<IrValue*, 4> values{};
};

inline constexpr std::uint32_t NativePushConstantSize = sizeof(PushData);

// A plan value as the SRT walk reads it, in one 32-byte record: the walk then touches a few
// contiguous cache lines per shader instead of every IrValue and its argument vector.
struct CompactPlanValue {
    std::uint64_t immediate = 0;
    std::uint64_t flags = 0;
    std::uint32_t firstArgument = 0;
    std::uint32_t registerIndex = 0;
    IrType type = IrType::Void;
    IrOpcode opcode = IrOpcode::Void;
    std::uint8_t argumentCount = 0;
    bool hasImmediate = false;
};

// IrResourcePlan's values (dense ids) and the walk's roots in compact form. Value ids, Identity
// values resolved: `arguments` holds each value's arguments from firstArgument on; `sourceDwords`
// eight per descriptor source, `srtReads` one per SRT read, `conditions` one per control flow
// block (NoValue for none). Empty when the plan could not be expressed this way.
struct CompactResourcePlan {
    static constexpr std::uint32_t NoValue = 0xffffffffu;
    std::vector<CompactPlanValue> values;
    std::vector<std::uint32_t> arguments;
    std::vector<std::uint32_t> sourceDwords;
    std::vector<std::uint32_t> srtReads;
    std::vector<std::uint32_t> conditions;
};

struct IrResourcePlan {
    IrShaderStage stage = IrShaderStage::Unknown;
    std::uint64_t shaderHash = 0;
    std::uint32_t userDataBase = 0;
    std::uint32_t userDataCount = 64;
    std::vector<std::unique_ptr<IrValue>> valueStorage;
    std::vector<std::unique_ptr<IrBlock>> blockStorage;
    std::vector<MemoryInfo> memoryInfo;
    std::vector<DescriptorSource> descriptorSources;
    std::vector<ResourceBlock> controlFlow;
    std::vector<std::uint32_t> materializationSources;
    std::vector<SrtRead> srtReads;
    std::vector<std::uint8_t> cleanFlatSlots;
    // One byte per srtReads slot, 1 when the CPU walk never consumes the slot's value (see
    // Detail::ComputePureFlatSlots): a driver may reuse a capture whose words differ only there.
    std::vector<std::uint8_t> pureFlatSlots;
    // Set by ResourceMaterializer::ExtractPlan: every value reachable from the plan's roots is in
    // valueStorage and its Id() is its index there, so the SRT walk memoizes per value in a flat
    // array instead of a hash table.
    bool denseValueIds = false;
    // Built by ExtractPlan with dense value ids; the SRT walk uses it when values is not empty.
    CompactResourcePlan compact;
    bool requiresSpecializationMemory = false;
    bool srtPlanComplete = false;
    bool resourceTrackingComplete = false;
    ShaderInfo info;
    UniformFillPlan uniformFill;
};

}

#endif
