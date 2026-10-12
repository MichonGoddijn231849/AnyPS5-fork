#include "SpirvBackend/SpirvRuntimeBuffer.hpp"
#include "SpirvBackend/SpirvEmitterInstructions.hpp"
#include "SpirvBackend/SpirvBda.hpp"
#include "SpirvBackend/SpirvBufferFormat.hpp"
#include "SpirvBackend/SpirvMemory/SpirvSubgroup.hpp"
#include "SpirvBackend/SpirvMemory/SpirvInputOutput.hpp"
#include "PipelineSpecialization.hpp"
#include "SpirvBackend/SpirvEmitterHelpers.hpp"
#include <spirv/unified1/GLSL.std.450.h>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <array>
#include <string>
#include <vector>

namespace ShaderRecompiler {
namespace {

struct RuntimeDescriptor {
    std::uint32_t base;
    std::uint32_t records;
    std::uint32_t stride;
    std::uint32_t word3;
    std::uint32_t swizzle;
    std::uint32_t index;
    std::uint32_t valid;
    std::uint32_t offset = 0;
    std::uint32_t soffset = 0;
};

struct RuntimeAddress {
    std::uint32_t guest;
    std::uint32_t inBounds;
};

std::uint32_t field(SpirvEmitterState& state, std::uint32_t word, std::uint32_t offset, std::uint32_t count) {
    return EmitBitFieldUExtract(state, word, ConstantU32(state, offset), ConstantU32(state, count));
}

std::uint32_t both(SpirvEmitterState& state, std::uint32_t left, std::uint32_t right) {
    return Binary(state, spv::OpLogicalAnd, TypeBool(state), left, right);
}

std::uint32_t equal(SpirvEmitterState& state, std::uint32_t left, std::uint32_t right) {
    return Binary(state, spv::OpIEqual, TypeBool(state), left, right);
}

std::uint32_t nonzero(SpirvEmitterState& state, std::uint32_t value) {
    return Binary(state, spv::OpINotEqual, TypeBool(state), value, ConstantU32(state, 0u));
}

void faultIf(SpirvValueEmitContext& context, const IrValue& instruction, std::uint32_t condition, std::uint32_t address, std::uint32_t bytes, BdaAbi::FaultReason reason) {
    auto& state = context.state;
    EmitIfCondition(state, condition, [&] { RecordBdaFault(state, address, ConstantU32(state, bytes), BdaInstructionPc(state, instruction), reason); });
    StopBdaInvocationIf(state, condition);
}

RuntimeDescriptor descriptor(SpirvValueEmitContext& context, const IrValue& instruction) {
    auto& state = context.state;
    const auto* handle = instruction.Argument(0)->Resolve();
    if (handle->Opcode() != IrOpcode::GetBufferResource || handle->ArgumentCount() != 4u) context.Fail(instruction, "requires a runtime four-dword V#");
    const auto& memory = context.Memory(instruction);
    const auto first = PipelineSpecialization::BufferBase + memory.resource * PipelineSpecialization::BufferWords;
    const auto word1 = memory.gpuDescriptor ? context.Arg(*handle, 1u) : state.module.SpecializationConstant(TypeU32(state), first, 0u);
    const auto word3 = memory.gpuDescriptor ? context.Arg(*handle, 3u) : state.module.SpecializationConstant(TypeU32(state), first + 1u, 0u);
    const auto u32 = TypeU32(state);
    auto base = ConstantU32(state, 0u);
    if (memory.gpuDescriptor || BufferAccessOf(instruction.Opcode()) == BufferAccess::Atomic) {
        const auto u64 = TypeScalarU64(state);
        const auto high = Binary(state, spv::OpShiftLeftLogical, u64, Unary(state, spv::OpUConvert, u64, field(state, context.Arg(*handle, 1u), 0u, 16u)), BdaConstant(state, 32u));
        base = Binary(state, spv::OpBitwiseOr, u64, Unary(state, spv::OpUConvert, u64, context.Arg(*handle, 0u)), high);
    }
    auto invalid = nonzero(state, field(state, word3, 30u, 2u));
    auto index = ConstantU32(state, 0u);
    if (context.Memory(instruction).kind == ResourceKind::Buffer) {
        index = context.Arg(instruction, 1u);
        const auto addTid = nonzero(state, field(state, word3, 23u, 1u));
        if (state.program.Resources().stage == IrShaderStage::Compute) {
            const auto lane = state.laneCount == 1u && state.hostSubgroupSize < state.program.WaveSize() ?
                Binary(state, spv::OpBitwiseAnd, u32, EmitLocalInvocationIndex(state), ConstantU32(state, state.program.WaveSize() - 1u)) : EmitSubgroupLocalInvocationId(state);
            index = Binary(state, spv::OpIAdd, u32, index, Select(state, u32, addTid, lane, ConstantU32(state, 0u)));
        } else {
            invalid = Binary(state, spv::OpLogicalOr, TypeBool(state), invalid, addTid);
        }
    }
    if (memory.gpuDescriptor) faultIf(context, instruction, invalid, base, 0u, BdaAbi::FaultReason::InvalidDescriptor);
    const auto stride = field(state, word1, 16u, 14u);
    const auto valid = memory.gpuDescriptor ? both(state, Unary(state, spv::OpLogicalNot, TypeBool(state), invalid), Binary(state, spv::OpINotEqual, TypeBool(state), base, BdaConstant(state, 0u))) : nonzero(state, state.module.SpecializationConstant(u32, first + 2u, 0u));
    return {base, context.Arg(*handle, 2u), stride, word3, both(state, nonzero(state, stride), nonzero(state, field(state, word1, 31u, 1u))), index, valid};
}

// The byte address and bounds of `bytes` (a u32 value) at the element offset `offset` (a u32 value, the component's
// included).
RuntimeAddress addressAt(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t offset, std::uint32_t bytes) {
    auto& state = context.state;
    const auto u32 = TypeU32(state);
    const auto boolean = TypeBool(state);
    const auto constant = [&](std::uint32_t value) { return ConstantU32(state, value); };
    const auto add = [&](std::uint32_t left, std::uint32_t right) { return Binary(state, spv::OpIAdd, u32, left, right); };
    const auto mul = [&](std::uint32_t left, std::uint32_t right) { return Binary(state, spv::OpIMul, u32, left, right); };
    const auto less = [&](std::uint32_t left, std::uint32_t right) { return Binary(state, spv::OpULessThan, boolean, left, right); };
    const auto fits = [&](std::uint32_t offset, std::uint32_t size) {
        return both(state, Binary(state, spv::OpUGreaterThanEqual, boolean, size, bytes), Binary(state, spv::OpULessThanEqual, boolean, offset, Binary(state, spv::OpISub, u32, size, bytes)));
    };
    const auto soffset = resource.soffset != 0u ? resource.soffset : context.Arg(instruction, 3u);
    const auto indexShift = add(field(state, resource.word3, 21u, 2u), constant(3u));
    const auto indices = Binary(state, spv::OpShiftLeftLogical, u32, constant(1u), indexShift);
    const auto indexMsb = Binary(state, spv::OpShiftRightLogical, u32, resource.index, indexShift);
    const auto indexLsb = Binary(state, spv::OpBitwiseAnd, u32, resource.index, Binary(state, spv::OpISub, u32, indices, constant(1u)));
    const auto offsetMsb = Binary(state, spv::OpBitwiseAnd, u32, offset, constant(~3u));
    const auto offsetLsb = Binary(state, spv::OpBitwiseAnd, u32, offset, constant(3u));
    const auto swizzled = add(mul(add(mul(indexMsb, resource.stride), offsetMsb), indices), add(Binary(state, spv::OpShiftLeftLogical, u32, indexLsb, constant(2u)), offsetLsb));
    const auto linear = add(mul(resource.index, resource.stride), offset);
    const auto byte = add(Select(state, u32, resource.swizzle, swizzled, linear), soffset);
    const auto mode = field(state, resource.word3, 28u, 2u);
    const auto indexInBounds = less(resource.index, resource.records);
    const auto structured = both(state, indexInBounds, less(offset, resource.stride));
    const auto rawRecords = Binary(state, spv::OpISub, u32, resource.records, soffset);
    const auto raw = both(state, Binary(state, spv::OpULessThanEqual, boolean, soffset, resource.records), Select(state, boolean, resource.swizzle, both(state, less(resource.index, rawRecords), fits(offset, resource.stride)), fits(offset, rawRecords)));
    auto inBounds = Select(state, boolean, equal(state, mode, constant(0u)), structured, indexInBounds);
    inBounds = Select(state, boolean, less(mode, constant(2u)), inBounds, Select(state, boolean, equal(state, mode, constant(2u)), nonzero(state, resource.records), raw));
    inBounds = both(state, resource.valid, both(state, nonzero(state, field(state, resource.word3, 12u, 7u)), inBounds));
    return {byte, inBounds};
}

std::uint32_t elementOffset(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t componentOffset) {
    auto& state = context.state;
    if (resource.offset != 0u) return Binary(state, spv::OpIAdd, TypeU32(state), resource.offset, componentOffset);
    return Binary(state, spv::OpIAdd, TypeU32(state), context.Arg(instruction, 2u), Binary(state, spv::OpIAdd, TypeU32(state), ConstantU32(state, context.Memory(instruction).offset), componentOffset));
}

RuntimeAddress addressByte(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t componentOffset, std::uint32_t bytes) {
    auto& state = context.state;
    const auto u32 = TypeU32(state);
    const auto offset = resource.offset != 0u ? Binary(state, spv::OpIAdd, u32, resource.offset, ConstantU32(state, componentOffset)) : Binary(state, spv::OpIAdd, u32, context.Arg(instruction, 2u), ConstantU32(state, context.Memory(instruction).offset + componentOffset));
    return addressAt(context, instruction, resource, offset, ConstantU32(state, bytes));
}

RuntimeAddress address(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t componentOffset, std::uint32_t bytes) {
    const auto located = addressByte(context, instruction, resource, componentOffset, bytes);
    if (!context.Memory(instruction).gpuDescriptor) return located;
    return {AddBdaAddress(context, instruction, resource.base, Unary(context.state, spv::OpUConvert, TypeScalarU64(context.state), located.guest), false), located.inBounds};
}

// address() of a component whose byte offset and size (u32 values) come from the runtime format table.
RuntimeAddress addressRuntime(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t componentOffset, std::uint32_t bytes) {
    const auto located = addressAt(context, instruction, resource, elementOffset(context, instruction, resource, componentOffset), bytes);
    if (!context.Memory(instruction).gpuDescriptor) return located;
    return {AddBdaAddress(context, instruction, resource.base, Unary(context.state, spv::OpUConvert, TypeScalarU64(context.state), located.guest), false), located.inBounds};
}

MemoryResourceAccess directAccess(SpirvValueEmitContext& context, const IrValue& instruction, bool wide = false) {
    auto& state = context.state;
    const auto& memory = context.Memory(instruction);
    const auto variable = wide ? state.storageBufferU64Variable : state.storageBufferVariable;
    if (variable == 0u) context.Fail(instruction, "direct buffer descriptor array is missing");
    const auto element = ResourceForDescriptor(state, DescriptorBindingKind::Buffers, memory.resource);
    MemoryResourceAccess access;
    access.kind = memory.kind;
    access.objectPointer = state.module.AllocateId();
    state.module.AddFunction(spv::OpAccessChain, wide ? TypeStorageBufferU64Pointer(state) : TypeStorageBufferPointer(state), access.objectPointer, variable, ConstantU32(state, element));
    access.byteOffset = state.memoryByteOffsets.at(element);
    access.memoryAccess = memory.coherent ? spv::MemoryAccessVolatileMask : 0u;
    access.length = state.module.AllocateId();
    state.module.AddFunction(spv::OpArrayLength, TypeU32(state), access.length, access.objectPointer, 0u);
    return access;
}

std::uint32_t baseMisalignment(SpirvValueEmitContext& context, const IrValue& instruction) {
    const auto& memory = context.Memory(instruction);
    if (memory.kind == ResourceKind::ScalarBuffer) return ConstantU32(context.state, 0u);
    const auto first = PipelineSpecialization::BufferBase + memory.resource * PipelineSpecialization::BufferWords;
    return context.state.module.SpecializationConstant(TypeU32(context.state), first + 3u, 0u);
}

std::uint32_t directWordIndex(SpirvEmitterState& state, const MemoryResourceAccess& access, std::uint32_t address) {
    const auto relative = Binary(state, spv::OpShiftRightLogical, TypeU32(state), address, ConstantU32(state, 2u));
    const auto base = Binary(state, spv::OpShiftRightLogical, TypeU32(state), access.byteOffset, ConstantU32(state, 2u));
    return EmitAddU32(state, relative, base);
}

std::uint32_t readBuffer(SpirvValueEmitContext& context, const IrValue& instruction, std::uint32_t address, std::uint32_t bits) {
    if (context.Memory(instruction).gpuDescriptor) return EmitBdaRead(context, instruction, address, bits);
    auto& state = context.state;
    const auto access = directAccess(context, instruction);
    const auto index = directWordIndex(state, access, address);
    const auto misalignment = baseMisalignment(context, instruction);
    const auto unaligned = nonzero(state, misalignment);
    const auto next = EmitAddU32(state, index, ConstantU32(state, 1u));
    const auto last = Select(state, TypeU32(state), unaligned, next, index);
    return EmitValueOrZeroIfCondition(state, EmitMemoryElementInBounds(state, access, last), [&] {
        const auto load = [&](std::uint32_t word) {
            const auto value = state.module.AllocateId();
            const auto pointer = EmitMemoryElementPointer(state, access, word);
            if (access.memoryAccess != 0u) state.module.AddFunction(spv::OpLoad, TypeU32(state), value, pointer, access.memoryAccess);
            else state.module.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
            return value;
        };
        const auto shift = Binary(state, spv::OpShiftLeftLogical, TypeU32(state), misalignment, ConstantU32(state, 3u));
        const auto low = Binary(state, spv::OpShiftRightLogical, TypeU32(state), load(index), shift);
        const auto high = EmitValueOrZeroIfCondition(state, unaligned, [&] {
            return Binary(state, spv::OpShiftLeftLogical, TypeU32(state), load(next), Binary(state, spv::OpISub, TypeU32(state), ConstantU32(state, 32u), shift));
        });
        auto value = EmitOrU32(state, low, high);
        if (bits != 32u) {
            const auto subwordShift = Binary(state, spv::OpShiftLeftLogical, TypeU32(state), EmitAndConstant(state, address, 3u), ConstantU32(state, 3u));
            value = Binary(state, spv::OpShiftRightLogical, TypeU32(state), value, subwordShift);
        }
        return EmitAndConstant(state, value, bits == 32u ? 0xffffffffu : (1u << bits) - 1u);
    });
}

void writeBuffer(SpirvValueEmitContext& context, const IrValue& instruction, std::uint32_t address, std::uint32_t value, std::uint32_t bits = 32u) {
    if (context.Memory(instruction).gpuDescriptor) {
        EmitBdaWrite(context, instruction, address, value, bits);
        return;
    }
    auto& state = context.state;
    const auto access = directAccess(context, instruction);
    const auto u32 = TypeU32(state);
    const auto index = directWordIndex(state, access, address);
    const auto misalignment = baseMisalignment(context, instruction);
    const auto unaligned = nonzero(state, misalignment);
    const auto next = EmitAddU32(state, index, ConstantU32(state, 1u));
    const auto last = Select(state, u32, unaligned, next, index);
    EmitIfCondition(state, EmitMemoryElementInBounds(state, access, last), [&] {
        auto mask = ConstantU32(state, bits == 32u ? 0xffffffffu : (1u << bits) - 1u);
        auto masked = Binary(state, spv::OpBitwiseAnd, u32, value, mask);
        if (bits != 32u) {
            const auto subwordShift = Binary(state, spv::OpShiftLeftLogical, u32, EmitAndConstant(state, address, 3u), ConstantU32(state, 3u));
            mask = Binary(state, spv::OpShiftLeftLogical, u32, mask, subwordShift);
            masked = Binary(state, spv::OpShiftLeftLogical, u32, masked, subwordShift);
        }
        const auto update = [&](std::uint32_t word, std::uint32_t wordMask, std::uint32_t replacement) {
            AtomicUpdate(state, EmitMemoryElementPointer(state, access, word), access.kind, [&](std::uint32_t old) {
                return EmitOrU32(state, Binary(state, spv::OpBitwiseAnd, u32, old, Unary(state, spv::OpNot, u32, wordMask)), replacement);
            });
        };
        if (bits == 32u) {
            EmitIfCondition(state, Unary(state, spv::OpLogicalNot, TypeBool(state), unaligned), [&] {
                const auto pointer = EmitMemoryElementPointer(state, access, index);
                if (access.memoryAccess != 0u) state.module.AddFunction(spv::OpStore, pointer, value, access.memoryAccess);
                else state.module.AddFunction(spv::OpStore, pointer, value);
            });
        }
        EmitIfCondition(state, bits == 32u ? unaligned : ConstantBool(state, true), [&] {
            const auto shift = Binary(state, spv::OpShiftLeftLogical, u32, misalignment, ConstantU32(state, 3u));
            update(index, Binary(state, spv::OpShiftLeftLogical, u32, mask, shift), Binary(state, spv::OpShiftLeftLogical, u32, masked, shift));
            EmitIfCondition(state, unaligned, [&] {
                const auto remaining = Binary(state, spv::OpISub, u32, ConstantU32(state, 32u), shift);
                update(next, Binary(state, spv::OpShiftRightLogical, u32, mask, remaining), Binary(state, spv::OpShiftRightLogical, u32, masked, remaining));
            });
        });
    });
}

std::uint32_t composite(SpirvEmitterState& state, std::uint32_t components, const std::array<std::uint32_t, 4>& values) {
    if (components == 1u) return values[0];
    const auto result = state.module.AllocateId();
    std::vector<std::uint32_t> words{spv::OpCompositeConstruct, TypeU32Composite(state, components), result};
    words.insert(words.end(), values.begin(), values.begin() + components);
    state.module.AddFunction(words);
    return result;
}

std::uint32_t storeData(SpirvValueEmitContext& context, const IrValue& instruction) {
    return context.Arg(instruction, instruction.ArgumentCount() - 2u);
}

std::array<std::uint32_t, 4> storeValues(SpirvEmitterState& state, std::uint32_t data, std::uint32_t components) {
    std::array<std::uint32_t, 4> result{};
    for (std::uint32_t component = 0; component < components; ++component) {
        result[component] = data;
        if (components != 1u) {
            result[component] = state.module.AllocateId();
            state.module.AddFunction(spv::OpCompositeExtract, TypeU32(state), result[component], data, component);
        }
    }
    return result;
}

std::uint32_t formatted(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, const SpirvBufferFormatInfo& info, std::uint32_t components, bool store, std::uint32_t data) {
    auto& state = context.state;
    const auto& memory = context.Memory(instruction);
    const auto u32 = TypeU32(state);
    std::array<std::uint32_t, 4> values{};
    std::array<std::uint32_t, 4> selectors{};
    std::array<std::uint32_t, 4> selected{};
    std::array<RuntimeAddress, 4> addresses{};
    auto inBounds = ConstantBool(state, true);
    for (std::uint32_t output = 0; output < components; ++output) {
        selectors[output] = memory.typed ? ConstantU32(state, output < info.componentCount ? 4u + output : 0u) : field(state, resource.word3, output * 3u, 3u);
        selected[output] = Binary(state, spv::OpUMod, u32, Binary(state, spv::OpISub, u32, selectors[output], ConstantU32(state, 4u)), ConstantU32(state, info.componentCount));
    }
    std::array<std::uint32_t, 4> required{};
    for (std::uint32_t component = 0; component < info.componentCount; ++component) {
        required[component] = ConstantBool(state, store);
        if (!store) {
            for (std::uint32_t output = 0; output < components; ++output) {
                const auto fromMemory = Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), selectors[output], ConstantU32(state, 4u));
                required[component] = Binary(state, spv::OpLogicalOr, TypeBool(state), required[component], both(state, fromMemory, equal(state, selected[output], ConstantU32(state, component))));
            }
        }
        addresses[component] = address(context, instruction, resource, GetFormatComponentByteOffset(info, component), info.packedBitfield ? 4u : info.componentBits[component] / 8u);
        inBounds = both(state, inBounds, Binary(state, spv::OpLogicalOr, TypeBool(state), Unary(state, spv::OpLogicalNot, TypeBool(state), required[component]), addresses[component].inBounds));
    }
    if (store) {
        const auto values = storeValues(state, data, components);
        EmitIfCondition(state, inBounds, [&] {
            auto packed = ConstantU32(state, 0u);
            for (std::uint32_t component = 0; component < info.componentCount; ++component) {
                const auto encoded = component >= components ? ConstantU32(state, 0u) : memory.d16 ? EmitD16StoreComponent(state, info, component, values[component]) : EmitFormatStoreComponent(state, info, component, values[component]);
                if (info.packedBitfield) {
                    packed = EmitOrU32(state, packed, Binary(state, spv::OpShiftLeftLogical, u32, EmitAndConstant(state, encoded, (1u << info.componentBits[component]) - 1u), ConstantU32(state, info.componentBitOffset[component])));
                } else {
                    writeBuffer(context, instruction, addresses[component].guest, encoded, info.componentBits[component]);
                }
            }
            if (info.packedBitfield) writeBuffer(context, instruction, addresses[0].guest, packed);
        });
        return ConstantU32(state, 0u);
    }
    for (std::uint32_t component = 0; component < info.componentCount; ++component) {
        values[component] = EmitValueOrZeroIfCondition(state, both(state, inBounds, required[component]), [&] {
            const auto bits = info.componentBits[component];
            auto raw = readBuffer(context, instruction, addresses[component].guest, info.packedBitfield ? 32u : bits);
            if (bits != 32u) {
                const auto signedType = IsSignedFormatComponent(info.type);
                const auto type = signedType ? TypeI32(state) : u32;
                const auto input = signedType ? Unary(state, spv::OpBitcast, type, raw) : raw;
                raw = state.module.AllocateId();
                state.module.AddFunction(signedType ? spv::OpBitFieldSExtract : spv::OpBitFieldUExtract, type, raw, input, ConstantU32(state, info.packedBitfield ? info.componentBitOffset[component] : 0u), ConstantU32(state, bits));
                if (signedType) raw = Unary(state, spv::OpBitcast, u32, raw);
            }
            return memory.d16 ? EmitD16FormatComponent(state, info, component, raw) : NormalizeFormatComponent(state, info, component, raw);
        });
    }
    const bool integer = info.type == SpirvFormatComponentType::Uint || info.type == SpirvFormatComponentType::Sint;
    const auto one = ConstantU32(state, integer ? 1u : memory.d16 ? 0x3c00u : 0x3f800000u);
    std::array<std::uint32_t, 4> outputs{};
    for (std::uint32_t output = 0; output < components; ++output) {
        auto value = values[0];
        for (std::uint32_t component = 1; component < info.componentCount; ++component) value = Select(state, u32, equal(state, selected[output], ConstantU32(state, component)), values[component], value);
        value = Select(state, u32, equal(state, selectors[output], ConstantU32(state, 1u)), one, value);
        outputs[output] = Select(state, u32, equal(state, selectors[output], ConstantU32(state, 0u)), ConstantU32(state, 0u), value);
    }
    return composite(state, components, outputs);
}

std::uint32_t formatSwitch(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t components, bool store, std::uint32_t data);
std::uint32_t formattedCall(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t components, bool store, std::uint32_t data);
std::uint32_t formattedGpuCall(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t components, bool store, std::uint32_t data);

std::uint32_t formattedDispatch(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t components, bool store, std::uint32_t data) {
    auto& state = context.state;
    const auto& memory = context.Memory(instruction);
    if (memory.typed) {
        const auto info = GetFormatInfo(DecodeTBufferFormat(memory.dataFormat, memory.numberFormat));
        if (info.type == SpirvFormatComponentType::Unknown) context.Fail(instruction, "typed buffer instruction has an invalid format");
        return formatted(context, instruction, resource, info, components, store, data);
    }
    if (!store && memory.gpuDescriptor) {
        auto invalid = ConstantBool(state, false);
        for (std::uint32_t component = 0; component < components; ++component) {
            const auto selector = field(state, resource.word3, component * 3u, 3u);
            invalid = Binary(state, spv::OpLogicalOr, TypeBool(state), invalid, Binary(state, spv::OpLogicalOr, TypeBool(state), equal(state, selector, ConstantU32(state, 2u)), equal(state, selector, ConstantU32(state, 3u))));
        }
        faultIf(context, instruction, invalid, resource.base, 0u, BdaAbi::FaultReason::InvalidDescriptor);
    }
    const auto* word = instruction.Argument(0)->Resolve()->Argument(3)->Resolve();
    if (memory.gpuDescriptor && word->HasImmediate()) {
        const auto format = static_cast<IrBufferFormat>((word->ImmediateU32() >> 12u) & 0x7fu);
        if (format == IrBufferFormat::Invalid) context.Fail(instruction, "formatted buffer instruction has an invalid constant format");
        return formatted(context, instruction, resource, GetFormatInfo(format), components, store, data);
    }
    if (!memory.gpuDescriptor) return formattedCall(context, instruction, resource, components, store, data);
    return formattedGpuCall(context, instruction, resource, components, store, data);
}

// What a load of a format reads and from where (components, widths, offsets, packing): the data format, whatever the
// number format. Formats with one layout share their reads.
struct FormatLayout {
    std::uint32_t componentCount = 0;
    std::array<std::uint32_t, 4> byteOffsets{};
    std::array<std::uint32_t, 4> bits{};
    std::array<std::uint32_t, 4> bitOffsets{};
    bool packed = false;
    auto operator<=>(const FormatLayout&) const = default;
};

FormatLayout layoutOf(const SpirvBufferFormatInfo& info) {
    FormatLayout layout;
    layout.componentCount = info.componentCount;
    layout.packed = info.packedBitfield;
    for (std::uint32_t component = 0; component < info.componentCount; ++component) {
        layout.byteOffsets[component] = GetFormatComponentByteOffset(info, component);
        layout.bits[component] = info.componentBits[component];
        layout.bitOffsets[component] = info.packedBitfield ? info.componentBitOffset[component] : 0u;
    }
    return layout;
}

// formatted()'s load up to the raw component bits (unsigned, extracted from their word), which depend on the layout only.
struct RawComponents {
    std::array<std::uint32_t, 4> raw{};
    std::array<std::uint32_t, 4> valid{};
    std::array<std::uint32_t, 4> selectors{};
    std::array<std::uint32_t, 4> selected{};
};

RawComponents readRawComponents(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, const SpirvBufferFormatInfo& info, std::uint32_t components) {
    auto& state = context.state;
    const auto u32 = TypeU32(state);
    RawComponents result;
    std::array<RuntimeAddress, 4> addresses{};
    std::array<std::uint32_t, 4> required{};
    auto inBounds = ConstantBool(state, true);
    for (std::uint32_t output = 0; output < components; ++output) {
        result.selectors[output] = field(state, resource.word3, output * 3u, 3u);
        result.selected[output] = Binary(state, spv::OpUMod, u32, Binary(state, spv::OpISub, u32, result.selectors[output], ConstantU32(state, 4u)), ConstantU32(state, info.componentCount));
    }
    for (std::uint32_t component = 0; component < info.componentCount; ++component) {
        required[component] = ConstantBool(state, false);
        for (std::uint32_t output = 0; output < components; ++output) {
            const auto fromMemory = Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), result.selectors[output], ConstantU32(state, 4u));
            required[component] = Binary(state, spv::OpLogicalOr, TypeBool(state), required[component], both(state, fromMemory, equal(state, result.selected[output], ConstantU32(state, component))));
        }
        addresses[component] = address(context, instruction, resource, GetFormatComponentByteOffset(info, component), info.packedBitfield ? 4u : info.componentBits[component] / 8u);
        inBounds = both(state, inBounds, Binary(state, spv::OpLogicalOr, TypeBool(state), Unary(state, spv::OpLogicalNot, TypeBool(state), required[component]), addresses[component].inBounds));
    }
    for (std::uint32_t component = 0; component < info.componentCount; ++component) {
        result.valid[component] = both(state, inBounds, required[component]);
        result.raw[component] = EmitValueOrZeroIfCondition(state, result.valid[component], [&] {
            const auto bits = info.componentBits[component];
            auto raw = readBuffer(context, instruction, addresses[component].guest, info.packedBitfield ? 32u : bits);
            if (bits != 32u) {
                const auto extracted = state.module.AllocateId();
                state.module.AddFunction(spv::OpBitFieldUExtract, u32, extracted, raw, ConstantU32(state, info.packedBitfield ? info.componentBitOffset[component] : 0u), ConstantU32(state, bits));
                raw = extracted;
            }
            return raw;
        });
    }
    return result;
}

bool formatSharedReads() {
    static const bool shared = [] { const char* text = std::getenv("APS5_FORMAT_SHARED_READS"); return text != nullptr && std::strcmp(text, "0") != 0; }();
    return shared;
}

struct SharedReadKey {
    std::uint32_t byteOffset = 0;
    std::uint32_t bytes = 0;
    std::uint32_t bits = 0;
    auto operator<=>(const SharedReadKey&) const = default;
};

struct SharedReads {
    std::vector<SharedReadKey> keys;
    std::vector<std::uint32_t> values;
    std::vector<std::array<std::uint32_t, 4>> valid;
    std::vector<std::array<std::size_t, 4>> keyOf;
};

SharedReadKey sharedReadKey(const SpirvBufferFormatInfo& info, std::uint32_t component) {
    return {GetFormatComponentByteOffset(info, component), info.packedBitfield ? 4u : info.componentBits[component] / 8u, info.packedBitfield ? 32u : info.componentBits[component]};
}

std::vector<std::uint32_t> readKeysLooped(SpirvValueEmitContext& context, const IrValue& instruction, const std::vector<SharedReadKey>& keys, const std::vector<std::uint32_t>& guests, const std::vector<std::uint32_t>& need) {
    auto& state = context.state;
    const auto u32 = TypeU32(state);
    const auto u64 = TypeScalarU64(state);
    const auto boolean = TypeBool(state);
    const auto count = static_cast<std::uint32_t>(keys.size());
    const auto zero = ConstantU32(state, 0u);
    const auto before = state.currentLabel;
    const auto header = state.module.AllocateId();
    const auto body = state.module.AllocateId();
    const auto continuation = state.module.AllocateId();
    const auto merge = state.module.AllocateId();
    const auto index = state.module.AllocateId();
    const auto nextIndex = state.module.AllocateId();
    std::vector<std::uint32_t> values(count);
    std::vector<std::uint32_t> nextValues(count);
    for (std::uint32_t k = 0; k < count; ++k) {
        values[k] = state.module.AllocateId();
        nextValues[k] = state.module.AllocateId();
    }
    state.module.AddFunction(spv::OpBranch, header);
    EmitLabel(state, header);
    state.module.AddFunction(spv::OpPhi, u32, index, zero, before, nextIndex, continuation);
    for (std::uint32_t k = 0; k < count; ++k) state.module.AddFunction(spv::OpPhi, u32, values[k], zero, before, nextValues[k], continuation);
    const auto more = Binary(state, spv::OpULessThan, boolean, index, ConstantU32(state, count));
    state.module.AddFunction(spv::OpLoopMerge, merge, continuation, spv::LoopControlMaskNone);
    state.module.AddFunction(spv::OpBranchConditional, more, body, merge);
    EmitLabel(state, body);
    std::vector<std::uint32_t> isKey(count);
    auto guest = guests[0];
    auto bits = ConstantU32(state, keys[0].bits);
    auto needed = need[0];
    for (std::uint32_t k = 0; k < count; ++k) {
        isKey[k] = Binary(state, spv::OpIEqual, boolean, index, ConstantU32(state, k));
        if (k == 0u) continue;
        guest = Select(state, u64, isKey[k], guests[k], guest);
        bits = Select(state, u32, isKey[k], ConstantU32(state, keys[k].bits), bits);
        needed = Select(state, boolean, isKey[k], need[k], needed);
    }
    std::vector<std::uint32_t> widths;
    for (const auto& key : keys) {
        if (std::find(widths.begin(), widths.end(), key.bits) == widths.end()) widths.push_back(key.bits);
    }
    const auto value = EmitValueOrZeroIfCondition(state, needed, [&] {
        std::uint32_t result = 0;
        const auto readWidth = [&](std::size_t position, auto&& self) -> std::uint32_t {
            if (position + 1u == widths.size()) return readBuffer(context, instruction, guest, widths[position]);
            return EmitValueIfElse(state, Binary(state, spv::OpIEqual, boolean, bits, ConstantU32(state, widths[position])), u32, [&] { return readBuffer(context, instruction, guest, widths[position]); }, [&] { return self(position + 1u, self); });
        };
        result = readWidth(0u, readWidth);
        return result;
    });
    for (std::uint32_t k = 0; k < count; ++k) {
        const auto selected = Select(state, u32, isKey[k], value, values[k]);
        state.module.AddFunction(spv::OpCopyObject, u32, nextValues[k], selected);
    }
    state.module.AddFunction(spv::OpBranch, continuation);
    EmitLabel(state, continuation);
    state.module.AddFunction(spv::OpIAdd, u32, nextIndex, index, ConstantU32(state, 1u));
    state.module.AddFunction(spv::OpBranch, header);
    EmitLabel(state, merge);
    return values;
}

SharedReads readSharedComponents(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, const std::vector<SpirvBufferFormatInfo>& groups, const std::vector<std::vector<std::uint32_t>>& members, std::uint32_t format, std::uint32_t components) {
    auto& state = context.state;
    const auto u32 = TypeU32(state);
    const auto boolean = TypeBool(state);
    const bool gpu = context.Memory(instruction).gpuDescriptor;
    SharedReads reads;
    for (const auto& info : groups) {
        for (std::uint32_t component = 0; component < info.componentCount; ++component) reads.keys.push_back(sharedReadKey(info, component));
    }
    std::sort(reads.keys.begin(), reads.keys.end());
    reads.keys.erase(std::unique(reads.keys.begin(), reads.keys.end()), reads.keys.end());
    std::array<std::uint32_t, 4> selectors{};
    std::array<std::uint32_t, 4> fromMemory{};
    for (std::uint32_t output = 0; output < components; ++output) {
        selectors[output] = field(state, resource.word3, output * 3u, 3u);
        fromMemory[output] = Binary(state, spv::OpUGreaterThanEqual, boolean, selectors[output], ConstantU32(state, 4u));
    }
    std::array<std::array<std::uint32_t, 4>, 5> required{};
    for (const auto& info : groups) {
        const auto count = info.componentCount;
        if (required[count][0] != 0) continue;
        for (std::uint32_t component = 0; component < count; ++component) {
            required[count][component] = ConstantBool(state, false);
            for (std::uint32_t output = 0; output < components; ++output) {
                const auto selected = Binary(state, spv::OpUMod, u32, Binary(state, spv::OpISub, u32, selectors[output], ConstantU32(state, 4u)), ConstantU32(state, count));
                required[count][component] = Binary(state, spv::OpLogicalOr, boolean, required[count][component], both(state, fromMemory[output], equal(state, selected, ConstantU32(state, component))));
            }
        }
    }
    const auto merge = state.module.AllocateId();
    const auto none = state.module.AllocateId();
    std::vector<std::uint32_t> labels;
    std::vector<std::uint32_t> branch{spv::OpSwitch, format, none};
    for (std::size_t g = 0; g < groups.size(); ++g) {
        labels.push_back(state.module.AllocateId());
        for (const auto index : members[g]) branch.insert(branch.end(), {index, labels.back()});
    }
    state.module.AddFunction(spv::OpSelectionMerge, merge, spv::SelectionControlMaskNone);
    state.module.AddFunction(branch);
    std::vector<std::uint32_t> phi{spv::OpPhi, u32, state.module.AllocateId()};
    for (std::size_t g = 0; g < groups.size(); ++g) {
        EmitLabel(state, labels[g]);
        state.module.AddFunction(spv::OpBranch, merge);
        phi.insert(phi.end(), {ConstantU32(state, static_cast<std::uint32_t>(g)), labels[g]});
    }
    EmitLabel(state, none);
    state.module.AddFunction(spv::OpBranch, merge);
    phi.insert(phi.end(), {ConstantU32(state, static_cast<std::uint32_t>(groups.size())), none});
    EmitLabel(state, merge);
    const auto group = phi[2];
    state.module.AddFunction(phi);
    std::vector<RuntimeAddress> located;
    for (const auto& key : reads.keys) located.push_back(addressByte(context, instruction, resource, key.byteOffset, key.bytes));
    const auto keyIndex = [&](const SharedReadKey& key) { return static_cast<std::size_t>(std::lower_bound(reads.keys.begin(), reads.keys.end(), key) - reads.keys.begin()); };
    std::vector<std::uint32_t> need(reads.keys.size(), ConstantBool(state, false));
    std::vector<std::uint32_t> used(reads.keys.size(), ConstantBool(state, false));
    for (std::size_t g = 0; g < groups.size(); ++g) {
        const auto& info = groups[g];
        const auto active = equal(state, group, ConstantU32(state, static_cast<std::uint32_t>(g)));
        std::array<std::size_t, 4> keyOf{};
        auto inBounds = ConstantBool(state, true);
        for (std::uint32_t component = 0; component < info.componentCount; ++component) {
            keyOf[component] = keyIndex(sharedReadKey(info, component));
            inBounds = both(state, inBounds, Binary(state, spv::OpLogicalOr, boolean, Unary(state, spv::OpLogicalNot, boolean, required[info.componentCount][component]), located[keyOf[component]].inBounds));
            used[keyOf[component]] = Binary(state, spv::OpLogicalOr, boolean, used[keyOf[component]], active);
        }
        std::array<std::uint32_t, 4> valid{};
        for (std::uint32_t component = 0; component < info.componentCount; ++component) {
            valid[component] = both(state, inBounds, required[info.componentCount][component]);
            need[keyOf[component]] = Binary(state, spv::OpLogicalOr, boolean, need[keyOf[component]], both(state, active, valid[component]));
        }
        reads.valid.push_back(valid);
        reads.keyOf.push_back(keyOf);
    }
    std::vector<std::uint32_t> guests;
    std::uint32_t overflow = 0;
    for (std::size_t k = 0; k < reads.keys.size(); ++k) {
        if (!gpu) {
            guests.push_back(located[k].guest);
            continue;
        }
        const auto u64 = TypeScalarU64(state);
        const auto guest = Binary(state, spv::OpIAdd, u64, resource.base, Unary(state, spv::OpUConvert, u64, located[k].guest));
        const auto wrapped = both(state, used[k], Binary(state, spv::OpULessThan, boolean, guest, resource.base));
        overflow = overflow == 0u ? wrapped : Binary(state, spv::OpLogicalOr, boolean, overflow, wrapped);
        guests.push_back(guest);
    }
    if (overflow != 0u) {
        EmitIfCondition(state, overflow, [&] { RecordBdaFault(state, resource.base, ConstantU32(state, 0u), BdaInstructionPc(state, instruction), BdaAbi::FaultReason::Overflow); });
        StopBdaInvocationIf(state, overflow);
    }
    if (gpu && BdaHelpersCompact() && reads.keys.size() > 1u) {
        reads.values = readKeysLooped(context, instruction, reads.keys, guests, need);
        return reads;
    }
    for (std::size_t k = 0; k < reads.keys.size(); ++k) {
        reads.values.push_back(EmitValueOrZeroIfCondition(state, need[k], [&] { return readBuffer(context, instruction, guests[k], reads.keys[k].bits); }));
    }
    return reads;
}

RawComponents sharedRawComponents(SpirvValueEmitContext& context, const SharedReads& reads, std::size_t group, const SpirvBufferFormatInfo& info) {
    auto& state = context.state;
    const auto u32 = TypeU32(state);
    RawComponents result;
    for (std::uint32_t component = 0; component < info.componentCount; ++component) {
        result.valid[component] = reads.valid[group][component];
        auto raw = reads.values[reads.keyOf[group][component]];
        const auto bits = info.componentBits[component];
        if (bits != 32u) {
            const auto extracted = state.module.AllocateId();
            state.module.AddFunction(spv::OpBitFieldUExtract, u32, extracted, raw, ConstantU32(state, info.packedBitfield ? info.componentBitOffset[component] : 0u), ConstantU32(state, bits));
            raw = extracted;
        }
        result.raw[component] = raw;
    }
    return result;
}

// The conversion of one format of the layout: sign extension and conversion of each component (zero when it was not
// read). `one` is what a destination selector of 1 reads for the format.
struct ConvertedComponents {
    std::array<std::uint32_t, 4> values{};
    std::uint32_t one = 0;
};

ConvertedComponents convertRawComponents(SpirvValueEmitContext& context, const IrValue& instruction, const SpirvBufferFormatInfo& info, const RawComponents& raw) {
    auto& state = context.state;
    const auto& memory = context.Memory(instruction);
    const auto u32 = TypeU32(state);
    ConvertedComponents result;
    for (std::uint32_t component = 0; component < 4u; ++component) {
        if (component >= info.componentCount) {
            result.values[component] = ConstantU32(state, 0u);
            continue;
        }
        const auto bits = info.componentBits[component];
        auto value = raw.raw[component];
        if (bits != 32u && IsSignedFormatComponent(info.type)) {
            const auto extended = state.module.AllocateId();
            state.module.AddFunction(spv::OpBitFieldSExtract, TypeI32(state), extended, Unary(state, spv::OpBitcast, TypeI32(state), value), ConstantU32(state, 0u), ConstantU32(state, bits));
            value = Unary(state, spv::OpBitcast, u32, extended);
        }
        const auto converted = memory.d16 ? EmitD16FormatComponent(state, info, component, value) : NormalizeFormatComponent(state, info, component, value);
        result.values[component] = Select(state, u32, raw.valid[component], converted, ConstantU32(state, 0u));
    }
    const bool integer = info.type == SpirvFormatComponentType::Uint || info.type == SpirvFormatComponentType::Sint;
    result.one = ConstantU32(state, integer ? 1u : memory.d16 ? 0x3c00u : 0x3f800000u);
    return result;
}

// A load of a runtime format: a switch on the format whose cases share one read per layout and switch on the formats of
// the layout only to convert; the destination selects run once, after both. One case per format that read and selected
// on its own (Wolverine's 0x11d049aa00, 126 such loads of material buffers) put 16M words through NVIDIA's compiler,
// past 30 GB.
std::uint32_t formatLoadSwitch(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t components) {
    auto& state = context.state;
    const auto& memory = context.Memory(instruction);
    const auto u32 = TypeU32(state);
    const auto format = field(state, resource.word3, 12u, 7u);
    constexpr std::uint32_t count = static_cast<std::uint32_t>(IrBufferFormat::Format32_32_32_32Float);
    std::vector<std::pair<FormatLayout, std::vector<std::uint32_t>>> groups;
    for (std::uint32_t index = 1; index <= count; ++index) {
        const auto layout = layoutOf(GetFormatInfo(static_cast<IrBufferFormat>(index)));
        const auto found = std::find_if(groups.begin(), groups.end(), [&](const auto& group) { return group.first == layout; });
        if (found == groups.end()) groups.push_back({layout, {index}});
        else found->second.push_back(index);
    }
    // The values the cases hand to the selects: four components, the component count and the format's one; per
    // predecessor of a merge.
    struct Incoming {
        ConvertedComponents converted;
        std::uint32_t componentCount = 0;
        std::uint32_t label = 0;
    };
    const auto phis = [&](const std::vector<Incoming>& incoming) {
        Incoming merged;
        const auto phi = [&](auto&& value) {
            const auto result = state.module.AllocateId();
            std::vector<std::uint32_t> words{spv::OpPhi, u32, result};
            for (const auto& entry : incoming) words.insert(words.end(), {value(entry), entry.label});
            state.module.AddFunction(words);
            return result;
        };
        for (std::uint32_t component = 0; component < 4u; ++component) merged.converted.values[component] = phi([component](const Incoming& entry) { return entry.converted.values[component]; });
        merged.converted.one = phi([](const Incoming& entry) { return entry.converted.one; });
        merged.componentCount = phi([](const Incoming& entry) { return entry.componentCount; });
        return merged;
    };
    const auto leave = [&](std::uint32_t merge) {
        const auto exit = state.module.AllocateId();
        state.module.AddFunction(spv::OpBranch, exit);
        EmitLabel(state, exit);
        state.module.AddFunction(spv::OpBranch, merge);
        return exit;
    };
    const auto merge = state.module.AllocateId();
    const auto invalid = state.module.AllocateId();
    std::vector<std::uint32_t> groupLabels;
    std::vector<std::uint32_t> branch{spv::OpSwitch, format, invalid};
    for (const auto& group : groups) {
        groupLabels.push_back(state.module.AllocateId());
        for (const auto index : group.second) branch.insert(branch.end(), {index, groupLabels.back()});
    }
    std::vector<Incoming> outer;
    SharedReads shared;
    if (formatSharedReads()) {
        std::vector<SpirvBufferFormatInfo> infos;
        std::vector<std::vector<std::uint32_t>> members;
        for (const auto& group : groups) {
            infos.push_back(GetFormatInfo(static_cast<IrBufferFormat>(group.second.front())));
            members.push_back(group.second);
        }
        shared = readSharedComponents(context, instruction, resource, infos, members, format, components);
    }
    state.module.AddFunction(spv::OpSelectionMerge, merge, spv::SelectionControlMaskNone);
    state.module.AddFunction(branch);
    for (std::size_t g = 0; g < groups.size(); ++g) {
        EmitLabel(state, groupLabels[g]);
        const auto& members = groups[g].second;
        const auto first = GetFormatInfo(static_cast<IrBufferFormat>(members.front()));
        const auto raw = formatSharedReads() ? sharedRawComponents(context, shared, g, first) : readRawComponents(context, instruction, resource, first, components);
        Incoming value;
        if (members.size() == 1u) {
            value.converted = convertRawComponents(context, instruction, first, raw);
        } else {
            // The outer switch only enters with a member's format: the first member is the default.
            const auto innerMerge = state.module.AllocateId();
            std::vector<std::uint32_t> memberLabels;
            for (std::size_t m = 0; m < members.size(); ++m) memberLabels.push_back(state.module.AllocateId());
            std::vector<std::uint32_t> inner{spv::OpSwitch, format, memberLabels.front()};
            for (std::size_t m = 1; m < members.size(); ++m) inner.insert(inner.end(), {members[m], memberLabels[m]});
            state.module.AddFunction(spv::OpSelectionMerge, innerMerge, spv::SelectionControlMaskNone);
            state.module.AddFunction(inner);
            std::vector<Incoming> converted;
            for (std::size_t m = 0; m < members.size(); ++m) {
                EmitLabel(state, memberLabels[m]);
                Incoming entry;
                entry.converted = convertRawComponents(context, instruction, GetFormatInfo(static_cast<IrBufferFormat>(members[m])), raw);
                entry.componentCount = ConstantU32(state, first.componentCount);
                entry.label = leave(innerMerge);
                converted.push_back(entry);
            }
            EmitLabel(state, innerMerge);
            value.converted = phis(converted).converted;
        }
        value.componentCount = ConstantU32(state, first.componentCount);
        value.label = leave(merge);
        outer.push_back(value);
    }
    EmitLabel(state, invalid);
    if (memory.gpuDescriptor) RecordBdaFault(state, resource.base, ConstantU32(state, 0u), BdaInstructionPc(state, instruction), BdaAbi::FaultReason::InvalidDescriptor);
    Incoming none;
    for (auto& component : none.converted.values) component = ConstantU32(state, 0u);
    none.converted.one = ConstantU32(state, 0u);
    none.componentCount = ConstantU32(state, 1u);
    none.label = leave(merge);
    outer.push_back(none);
    EmitLabel(state, merge);
    const auto merged = phis(outer);
    // An invalid format reads zeros whatever the selectors.
    const auto valid = Binary(state, spv::OpLogicalAnd, TypeBool(state), Binary(state, spv::OpINotEqual, TypeBool(state), format, ConstantU32(state, 0u)), Binary(state, spv::OpULessThanEqual, TypeBool(state), format, ConstantU32(state, count)));
    std::array<std::uint32_t, 4> outputs{};
    for (std::uint32_t output = 0; output < components; ++output) {
        const auto selector = field(state, resource.word3, output * 3u, 3u);
        const auto selected = Binary(state, spv::OpUMod, u32, Binary(state, spv::OpISub, u32, selector, ConstantU32(state, 4u)), merged.componentCount);
        auto value = merged.converted.values[0];
        for (std::uint32_t component = 1; component < 4u; ++component) value = Select(state, u32, equal(state, selected, ConstantU32(state, component)), merged.converted.values[component], value);
        value = Select(state, u32, equal(state, selector, ConstantU32(state, 1u)), merged.converted.one, value);
        value = Select(state, u32, equal(state, selector, ConstantU32(state, 0u)), ConstantU32(state, 0u), value);
        outputs[output] = Select(state, u32, valid, value, ConstantU32(state, 0u));
    }
    const auto result = composite(state, components, outputs);
    if (memory.gpuDescriptor) StopBdaInvocationIf(state, Unary(state, spv::OpLogicalNot, TypeBool(state), valid));
    return result;
}

// The runtime format table of formatLoadTable: per format (entry 0 the invalid one) the component count, number
// type and packing, then a byte per component of bit width, bit offset in the read word (packed formats) and byte
// offset in the element.
std::uint32_t bufferFormatTable(SpirvEmitterState& state) {
    if (state.bufferFormatTableVariable != 0) return state.bufferFormatTableVariable;
    constexpr std::uint32_t count = static_cast<std::uint32_t>(IrBufferFormat::Format32_32_32_32Float);
    const auto entryType = TypeU32Vector(state, 4);
    std::vector<std::uint32_t> entries;
    for (std::uint32_t index = 0; index <= count; ++index) {
        std::array<std::uint32_t, 4> words{};
        if (index != 0u) {
            const auto info = GetFormatInfo(static_cast<IrBufferFormat>(index));
            words[0] = info.componentCount | (static_cast<std::uint32_t>(info.type) << 3u) | (info.packedBitfield ? 1u << 7u : 0u);
            for (std::uint32_t component = 0; component < info.componentCount; ++component) {
                words[1] |= info.componentBits[component] << (8u * component);
                words[2] |= (info.packedBitfield ? info.componentBitOffset[component] : 0u) << (8u * component);
                words[3] |= GetFormatComponentByteOffset(info, component) << (8u * component);
            }
        }
        entries.push_back(state.module.Constant(spv::OpConstantComposite, entryType, ConstantU32(state, words[0]), ConstantU32(state, words[1]), ConstantU32(state, words[2]), ConstantU32(state, words[3])));
    }
    const auto arrayType = state.module.Type(spv::OpTypeArray, entryType, ConstantU32(state, count + 1u));
    const auto initializer = state.module.Constant(spv::OpConstantComposite, arrayType, std::span<const std::uint32_t>(entries));
    state.bufferFormatTableVariable = state.module.DefineInitializedGlobalVariable(TypePointer(state, spv::StorageClassPrivate, arrayType), spv::StorageClassPrivate, initializer);
    return state.bufferFormatTableVariable;
}

// UnormToF32Bits (SpirvFormatConvert.cpp) of a width known at run time (1 to 16 bits): the same steps, the replication
// loop unrolled to its five possible doublings.
std::uint32_t unormBitsRuntime(SpirvEmitterState& state, std::uint32_t value, std::uint32_t bits) {
    const auto u32 = TypeU32(state);
    const auto boolean = TypeBool(state);
    const auto constant = [&](std::uint32_t number) { return ConstantU32(state, number); };
    const auto op = [&](spv::Op code, std::uint32_t left, std::uint32_t right) { return Binary(state, code, u32, left, right); };
    const auto maximum = op(spv::OpISub, op(spv::OpShiftLeftLogical, constant(1u), bits), constant(1u));
    const auto zero = Binary(state, spv::OpIEqual, boolean, value, constant(0u));
    const auto safe = Select(state, u32, zero, constant(1u), value);
    const auto msb = state.module.AllocateId();
    state.module.AddFunction(spv::OpExtInst, u32, msb, GlslStd450(state), GLSLstd450FindUMsb, safe);
    const auto leading = op(spv::OpISub, op(spv::OpISub, bits, constant(1u)), msb);
    const auto high = op(spv::OpShiftLeftLogical, safe, leading);
    const auto low = op(spv::OpShiftRightLogical, safe, op(spv::OpISub, bits, leading));
    const auto rotated = op(spv::OpBitwiseAnd, op(spv::OpBitwiseOr, high, low), maximum);
    auto pattern = op(spv::OpShiftLeftLogical, rotated, op(spv::OpISub, constant(32u), bits));
    for (std::uint32_t doubling = 0; doubling < 5u; ++doubling) {
        const auto shift = op(spv::OpShiftLeftLogical, bits, constant(doubling));
        const auto spread = op(spv::OpBitwiseOr, pattern, op(spv::OpShiftRightLogical, pattern, shift));
        pattern = Select(state, u32, Binary(state, spv::OpULessThan, boolean, shift, constant(32u)), spread, pattern);
    }
    const auto round = op(spv::OpBitwiseAnd, op(spv::OpShiftRightLogical, pattern, constant(7u)), constant(1u));
    const auto rounded = op(spv::OpIAdd, op(spv::OpShiftRightLogical, pattern, constant(8u)), round);
    const auto exponent = op(spv::OpShiftLeftLogical, op(spv::OpISub, op(spv::OpIAdd, msb, constant(126u)), bits), constant(23u));
    return Select(state, u32, zero, constant(0u), op(spv::OpIAdd, exponent, rounded));
}

// EmitUFloatToF32Bits of an 11- or 10-bit float chosen at run time.
std::uint32_t ufloatBitsRuntime(SpirvEmitterState& state, std::uint32_t raw, std::uint32_t bits) {
    const auto u32 = TypeU32(state);
    const auto f32 = TypeF32(state);
    const auto boolean = TypeBool(state);
    const auto constant = [&](std::uint32_t number) { return ConstantU32(state, number); };
    const auto op = [&](spv::Op code, std::uint32_t left, std::uint32_t right) { return Binary(state, code, u32, left, right); };
    const auto eleven = Binary(state, spv::OpIEqual, boolean, bits, constant(11u));
    const auto mantissaBits = Select(state, u32, eleven, constant(6u), constant(5u));
    const auto mantissa = op(spv::OpBitwiseAnd, raw, op(spv::OpISub, op(spv::OpShiftLeftLogical, constant(1u), mantissaBits), constant(1u)));
    const auto exponent = op(spv::OpBitwiseAnd, op(spv::OpShiftRightLogical, raw, mantissaBits), constant(0x1fu));
    const auto exponentBits = op(spv::OpShiftLeftLogical, op(spv::OpIAdd, exponent, constant(127u - 15u)), constant(23u));
    const auto mantissaBits32 = op(spv::OpShiftLeftLogical, mantissa, op(spv::OpISub, constant(23u), mantissaBits));
    const auto normal = op(spv::OpBitwiseOr, exponentBits, mantissaBits32);
    const auto special = op(spv::OpBitwiseOr, constant(0x7f800000u), mantissaBits32);
    const auto mantissaF32 = state.module.AllocateId();
    state.module.AddFunction(spv::OpConvertUToF, f32, mantissaF32, mantissa);
    const auto scale = Select(state, f32, eleven, ConstantF32Value(state, std::ldexp(1.0f, 1 - 15 - 6)), ConstantF32Value(state, std::ldexp(1.0f, 1 - 15 - 5)));
    const auto subnormal = EmitBitcastF32ToU32(state, Binary(state, spv::OpFMul, f32, mantissaF32, scale));
    const auto finite = Select(state, u32, Binary(state, spv::OpIEqual, boolean, exponent, constant(0u)), subnormal, normal);
    return Select(state, u32, Binary(state, spv::OpIEqual, boolean, exponent, constant(31u)), special, finite);
}

// NormalizeFormatComponent (and the sign extension before it) of a component whose number type and width come from
// the runtime format table; `raw` holds the component's bits, zero-extended.
std::uint32_t convertComponentRuntime(SpirvEmitterState& state, std::uint32_t type, std::uint32_t bits, std::uint32_t raw) {
    const auto u32 = TypeU32(state);
    const auto i32 = TypeI32(state);
    const auto f32 = TypeF32(state);
    const auto boolean = TypeBool(state);
    const auto constant = [&](std::uint32_t number) { return ConstantU32(state, number); };
    const auto is = [&](SpirvFormatComponentType kind) { return Binary(state, spv::OpIEqual, boolean, type, constant(static_cast<std::uint32_t>(kind))); };
    const auto isSigned = Binary(state, spv::OpLogicalOr, boolean, is(SpirvFormatComponentType::Sint), Binary(state, spv::OpLogicalOr, boolean, is(SpirvFormatComponentType::Snorm), is(SpirvFormatComponentType::Sscaled)));
    const auto extended = state.module.AllocateId();
    state.module.AddFunction(spv::OpBitFieldSExtract, i32, extended, Unary(state, spv::OpBitcast, i32, raw), constant(0u), bits);
    const auto value = Select(state, u32, isSigned, Unary(state, spv::OpBitcast, u32, extended), raw);
    const auto unsignedFloat = state.module.AllocateId();
    state.module.AddFunction(spv::OpConvertUToF, f32, unsignedFloat, value);
    const auto signedFloat = state.module.AllocateId();
    state.module.AddFunction(spv::OpConvertSToF, f32, signedFloat, Unary(state, spv::OpBitcast, i32, value));
    const auto unorm = unormBitsRuntime(state, value, bits);
    const auto snormBits = Binary(state, spv::OpISub, u32, bits, constant(1u));
    const auto negative = Binary(state, spv::OpSLessThan, boolean, Unary(state, spv::OpBitcast, i32, value), Unary(state, spv::OpBitcast, i32, constant(0u)));
    const auto magnitude = Select(state, u32, negative, Unary(state, spv::OpSNegate, u32, value), value);
    const auto clamped = state.module.AllocateId();
    state.module.AddFunction(spv::OpExtInst, u32, clamped, GlslStd450(state), GLSLstd450UMin, magnitude, Binary(state, spv::OpISub, u32, Binary(state, spv::OpShiftLeftLogical, u32, constant(1u), snormBits), constant(1u)));
    const auto snorm = Binary(state, spv::OpBitwiseOr, u32, Select(state, u32, negative, constant(0x80000000u), constant(0u)), unormBitsRuntime(state, clamped, snormBits));
    const auto half = EmitBitcastF32ToU32(state, EmitF16BitsToF32(state, value));
    const auto floating = Select(state, u32, Binary(state, spv::OpIEqual, boolean, bits, constant(32u)), value, Select(state, u32, Binary(state, spv::OpIEqual, boolean, bits, constant(16u)), half, ufloatBitsRuntime(state, value, bits)));
    auto result = value;
    result = Select(state, u32, is(SpirvFormatComponentType::Uscaled), EmitBitcastF32ToU32(state, unsignedFloat), result);
    result = Select(state, u32, is(SpirvFormatComponentType::Sscaled), EmitBitcastF32ToU32(state, signedFloat), result);
    result = Select(state, u32, is(SpirvFormatComponentType::Unorm), unorm, result);
    result = Select(state, u32, is(SpirvFormatComponentType::Snorm), snorm, result);
    return Select(state, u32, is(SpirvFormatComponentType::Float), floating, result);
}

// A load of a runtime format through a GPU-selected V#: formatLoadSwitch's reads, conversions and selects done once,
// with each component's layout and number type read from bufferFormatTable instead of a case per layout and format.
// Each component reads at most once (a dword, or one or two bytes), as the layout's case did. Wolverine's material
// lighting 0x11d049aa00 inlines 126 such loads; one case per layout made each 27k SPIR-V words.
std::uint32_t formatLoadTable(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t components) {
    auto& state = context.state;
    const auto u32 = TypeU32(state);
    const auto boolean = TypeBool(state);
    const auto constant = [&](std::uint32_t number) { return ConstantU32(state, number); };
    const auto both = [&](std::uint32_t left, std::uint32_t right) { return Binary(state, spv::OpLogicalAnd, boolean, left, right); };
    constexpr std::uint32_t count = static_cast<std::uint32_t>(IrBufferFormat::Format32_32_32_32Float);
    const auto format = field(state, resource.word3, 12u, 7u);
    const auto valid = both(Binary(state, spv::OpINotEqual, boolean, format, constant(0u)), Binary(state, spv::OpULessThanEqual, boolean, format, constant(count)));
    const auto entryType = TypeU32Vector(state, 4);
    const auto pointer = state.module.AllocateId();
    state.module.AddFunction(spv::OpAccessChain, TypePointer(state, spv::StorageClassPrivate, entryType), pointer, bufferFormatTable(state), Select(state, u32, valid, format, constant(0u)));
    const auto entry = state.module.AllocateId();
    state.module.AddFunction(spv::OpLoad, entryType, entry, pointer);
    std::array<std::uint32_t, 4> words{};
    for (std::uint32_t word = 0; word < 4u; ++word) {
        words[word] = state.module.AllocateId();
        state.module.AddFunction(spv::OpCompositeExtract, u32, words[word], entry, word);
    }
    const auto componentCount = field(state, words[0], 0u, 3u);
    const auto type = field(state, words[0], 3u, 4u);
    const auto packed = nonzero(state, field(state, words[0], 7u, 1u));
    const auto divisor = Select(state, u32, valid, componentCount, constant(1u));
    std::array<std::uint32_t, 4> selectors{};
    std::array<std::uint32_t, 4> selected{};
    for (std::uint32_t output = 0; output < components; ++output) {
        selectors[output] = field(state, resource.word3, output * 3u, 3u);
        selected[output] = Binary(state, spv::OpUMod, u32, Binary(state, spv::OpISub, u32, selectors[output], constant(4u)), divisor);
    }
    std::array<std::uint32_t, 4> bits{};
    std::array<std::uint32_t, 4> sizes{};
    std::array<std::uint32_t, 4> required{};
    std::array<RuntimeAddress, 4> addresses{};
    auto inBounds = ConstantBool(state, true);
    for (std::uint32_t component = 0; component < 4u; ++component) {
        bits[component] = field(state, words[1], 8u * component, 8u);
        sizes[component] = Select(state, u32, packed, constant(4u), Binary(state, spv::OpShiftRightLogical, u32, bits[component], constant(3u)));
        required[component] = ConstantBool(state, false);
        for (std::uint32_t output = 0; output < components; ++output) {
            const auto fromMemory = Binary(state, spv::OpUGreaterThanEqual, boolean, selectors[output], constant(4u));
            required[component] = Binary(state, spv::OpLogicalOr, boolean, required[component], both(fromMemory, equal(state, selected[output], constant(component))));
        }
        required[component] = both(both(valid, Binary(state, spv::OpULessThan, boolean, constant(component), componentCount)), required[component]);
        addresses[component] = addressRuntime(context, instruction, resource, field(state, words[3], 8u * component, 8u), sizes[component]);
        inBounds = both(inBounds, Binary(state, spv::OpLogicalOr, boolean, Unary(state, spv::OpLogicalNot, boolean, required[component]), addresses[component].inBounds));
    }
    std::array<std::uint32_t, 4> values{};
    for (std::uint32_t component = 0; component < 4u; ++component) {
        const auto read = both(inBounds, required[component]);
        const auto raw = EmitValueOrZeroIfCondition(state, read, [&] {
            const auto loaded = EmitBdaReadSized(context, instruction, addresses[component].guest, sizes[component]);
            const auto extracted = state.module.AllocateId();
            state.module.AddFunction(spv::OpBitFieldUExtract, u32, extracted, loaded, field(state, words[2], 8u * component, 8u), bits[component]);
            return extracted;
        });
        values[component] = Select(state, u32, read, convertComponentRuntime(state, type, bits[component], raw), constant(0u));
    }
    const auto integer = Binary(state, spv::OpLogicalOr, boolean, equal(state, type, constant(static_cast<std::uint32_t>(SpirvFormatComponentType::Uint))), equal(state, type, constant(static_cast<std::uint32_t>(SpirvFormatComponentType::Sint))));
    const auto one = Select(state, u32, integer, constant(1u), constant(0x3f800000u));
    EmitIfCondition(state, Unary(state, spv::OpLogicalNot, boolean, valid), [&] { RecordBdaFault(state, resource.base, constant(0u), BdaInstructionPc(state, instruction), BdaAbi::FaultReason::InvalidDescriptor); });
    std::array<std::uint32_t, 4> outputs{};
    for (std::uint32_t output = 0; output < components; ++output) {
        auto value = values[0];
        for (std::uint32_t component = 1; component < 4u; ++component) value = Select(state, u32, equal(state, selected[output], constant(component)), values[component], value);
        value = Select(state, u32, equal(state, selectors[output], constant(1u)), one, value);
        value = Select(state, u32, equal(state, selectors[output], constant(0u)), constant(0u), value);
        outputs[output] = Select(state, u32, valid, value, constant(0u));
    }
    const auto result = composite(state, components, outputs);
    StopBdaInvocationIf(state, Unary(state, spv::OpLogicalNot, boolean, valid));
    return result;
}

// APS5_FORMAT_LOAD_LAYOUT_SWITCH=1 loads runtime formats of GPU-selected V#s through formatLoadSwitch instead of the
// table (read at each load, so an execution test can compare the two in one process).
bool formatLoadLayoutSwitch() {
    const char* text = std::getenv("APS5_FORMAT_LOAD_LAYOUT_SWITCH");
    return text != nullptr && std::strcmp(text, "0") != 0;
}

std::uint32_t formatSwitch(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t components, bool store, std::uint32_t data) {
    auto& state = context.state;
    const auto& memory = context.Memory(instruction);
    if (!store && !memory.typed && !memory.d16 && memory.gpuDescriptor && !formatLoadLayoutSwitch() && !formatSharedReads()) return formatLoadTable(context, instruction, resource, components);
    // APS5_FORMAT_SWITCH_PER_FORMAT=1: a load reads per format, as before the layout groups.
    static const bool perFormat = std::getenv("APS5_FORMAT_SWITCH_PER_FORMAT") != nullptr;
    if (!store && !memory.typed && !perFormat) return formatLoadSwitch(context, instruction, resource, components);
    const auto format = field(state, resource.word3, 12u, 7u);
    const auto merge = state.module.AllocateId();
    const auto invalid = state.module.AllocateId();
    constexpr std::uint32_t count = static_cast<std::uint32_t>(IrBufferFormat::Format32_32_32_32Float);
    std::array<std::uint32_t, count> labels{};
    std::vector<std::uint32_t> branch{spv::OpSwitch, format, invalid};
    for (std::uint32_t index = 0; index < count; ++index) {
        labels[index] = state.module.AllocateId();
        branch.insert(branch.end(), {index + 1u, labels[index]});
    }
    const auto type = store || components == 1u ? TypeU32(state) : TypeU32Composite(state, components);
    const auto result = state.module.AllocateId();
    std::vector<std::uint32_t> phi{spv::OpPhi, type, result};
    state.module.AddFunction(spv::OpSelectionMerge, merge, spv::SelectionControlMaskNone);
    state.module.AddFunction(branch);
    for (std::uint32_t index = 0; index < count; ++index) {
        EmitLabel(state, labels[index]);
        const auto value = formatted(context, instruction, resource, GetFormatInfo(static_cast<IrBufferFormat>(index + 1u)), components, store, data);
        const auto exit = state.module.AllocateId();
        state.module.AddFunction(spv::OpBranch, exit);
        EmitLabel(state, exit);
        state.module.AddFunction(spv::OpBranch, merge);
        phi.insert(phi.end(), {value, exit});
    }
    EmitLabel(state, invalid);
    if (memory.gpuDescriptor) RecordBdaFault(state, resource.base, ConstantU32(state, 0u), BdaInstructionPc(state, instruction), BdaAbi::FaultReason::InvalidDescriptor);
    const auto zero = store || components == 1u ? ConstantU32(state, 0u) : ConstantU32CompositeZero(state, components);
    const auto invalidExit = state.module.AllocateId();
    state.module.AddFunction(spv::OpBranch, invalidExit);
    EmitLabel(state, invalidExit);
    state.module.AddFunction(spv::OpBranch, merge);
    phi.insert(phi.end(), {zero, invalidExit});
    EmitLabel(state, merge);
    state.module.AddFunction(phi);
    const auto unsupported = Binary(state, spv::OpLogicalOr, TypeBool(state), equal(state, format, ConstantU32(state, 0u)), Binary(state, spv::OpUGreaterThan, TypeBool(state), format, ConstantU32(state, count)));
    if (memory.gpuDescriptor) StopBdaInvocationIf(state, unsupported);
    return result;
}

std::uint32_t formattedCall(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t components, bool store, std::uint32_t data) {
    auto& state = context.state;
    const auto& memory = context.Memory(instruction);
    const auto u32 = TypeU32(state);
    const auto valueType = components == 1u ? u32 : TypeU32Composite(state, components);
    const auto resultType = store ? state.module.Type(spv::OpTypeVoid) : valueType;
    const auto element = ResourceForDescriptor(state, DescriptorBindingKind::Buffers, memory.resource);
    const std::array<std::uint32_t, 6> key{memory.resource, components, store ? 1u : 0u, memory.d16 ? 1u : 0u, memory.coherent ? 1u : 0u, static_cast<std::uint32_t>(memory.kind)};
    auto function = state.formattedBufferFunctions.find(key);
    if (function == state.formattedBufferFunctions.end()) {
        const auto id = state.module.AllocateId();
        const auto functionType = store ? state.module.Type(spv::OpTypeFunction, resultType, u32, u32, u32, u32, u32, valueType) : state.module.Type(spv::OpTypeFunction, resultType, u32, u32, u32, u32, u32);
        const auto label = state.currentLabel;
        const auto byteOffset = state.memoryByteOffsets.at(element);
        struct Restore {
            SpirvEmitterState& state;
            std::uint32_t element;
            std::uint32_t label;
            std::uint32_t byteOffset;
            ~Restore() {
                state.module.EndHelperFunction();
                state.memoryByteOffsets.at(element) = byteOffset;
                state.currentLabel = label;
            }
        };
        state.module.BeginHelperFunction();
        const Restore restore{state, element, label, byteOffset};
        state.module.AddFunction(spv::OpFunction, resultType, id, spv::FunctionControlDontInlineMask, functionType);
        std::array<std::uint32_t, 6> parameters{};
        for (std::uint32_t index = 0; index < (store ? 6u : 5u); ++index) {
            parameters[index] = state.module.AllocateId();
            state.module.AddFunction(spv::OpFunctionParameter, index == 5u ? valueType : u32, parameters[index]);
        }
        EmitLabel(state, state.module.AllocateId());
        state.memoryByteOffsets.at(element) = parameters[4];
        const auto first = PipelineSpecialization::BufferBase + memory.resource * PipelineSpecialization::BufferWords;
        const auto word1 = state.module.SpecializationConstant(u32, first, 0u);
        const auto word3 = state.module.SpecializationConstant(u32, first + 1u, 0u);
        const auto stride = field(state, word1, 16u, 14u);
        const auto swizzle = both(state, nonzero(state, stride), nonzero(state, field(state, word1, 31u, 1u)));
        const auto valid = nonzero(state, state.module.SpecializationConstant(u32, first + 2u, 0u));
        const RuntimeDescriptor shared{ConstantU32(state, 0u), parameters[3], stride, word3, swizzle, parameters[0], valid, parameters[1], parameters[2]};
        const auto value = formatSwitch(context, instruction, shared, components, store, store ? parameters[5] : 0u);
        if (store) state.module.AddFunction(spv::OpReturn);
        else state.module.AddFunction(spv::OpReturnValue, value);
        state.module.AddFunction(spv::OpFunctionEnd);
        function = state.formattedBufferFunctions.emplace(key, id).first;
    }
    const auto offset = Binary(state, spv::OpIAdd, u32, context.Arg(instruction, 2u), ConstantU32(state, memory.offset));
    const auto result = state.module.AllocateId();
    if (store) state.module.AddFunction(spv::OpFunctionCall, resultType, result, function->second, resource.index, offset, context.Arg(instruction, 3u), resource.records, state.memoryByteOffsets.at(element), data);
    else state.module.AddFunction(spv::OpFunctionCall, resultType, result, function->second, resource.index, offset, context.Arg(instruction, 3u), resource.records, state.memoryByteOffsets.at(element));
    return store ? ConstantU32(state, 0u) : result;
}

std::uint32_t formattedGpuCall(SpirvValueEmitContext& context, const IrValue& instruction, const RuntimeDescriptor& resource, std::uint32_t components, bool store, std::uint32_t data) {
    auto& state = context.state;
    const auto& memory = context.Memory(instruction);
    const auto u32 = TypeU32(state);
    const auto u64 = TypeScalarU64(state);
    const auto boolean = TypeBool(state);
    const bool stops = state.bdaStopsInvocations;
    const auto valueType = components == 1u ? u32 : TypeU32Composite(state, components);
    const auto resultType = stops ? (store ? u32 : state.module.Type(spv::OpTypeStruct, valueType, u32)) : (store ? state.module.Type(spv::OpTypeVoid) : valueType);
    const std::array<std::uint32_t, 6> key{components, store ? 1u : 0u, memory.d16 ? 1u : 0u, memory.coherent ? 1u : 0u, static_cast<std::uint32_t>(memory.kind), stops ? 1u : 0u};
    auto function = state.formattedGpuBufferFunctions.find(key);
    if (function == state.formattedGpuBufferFunctions.end()) {
        const auto id = state.module.AllocateId();
        const auto functionType = store ? state.module.Type(spv::OpTypeFunction, resultType, u64, u32, u32, u32, boolean, u32, boolean, u32, u32, u32, valueType)
                                        : state.module.Type(spv::OpTypeFunction, resultType, u64, u32, u32, u32, boolean, u32, boolean, u32, u32, u32);
        struct Restore {
            SpirvEmitterState& state;
            std::uint32_t label;
            std::uint32_t stopValue;
            ~Restore() {
                state.module.EndHelperFunction();
                state.currentLabel = label;
                state.bdaStopValue = stopValue;
                state.bdaPcOverride = 0u;
            }
        };
        state.module.BeginHelperFunction();
        const Restore restore{state, state.currentLabel, state.bdaStopValue};
        state.module.AddName(id, std::string(store ? "store" : "load") + "_formatted_gpu_buffer_x" + std::to_string(components));
        state.module.AddFunction(spv::OpFunction, resultType, id, spv::FunctionControlDontInlineMask, functionType);
        const std::array<std::uint32_t, 11> types{u64, u32, u32, u32, boolean, u32, boolean, u32, u32, u32, valueType};
        std::array<std::uint32_t, 11> parameters{};
        for (std::uint32_t index = 0; index < (store ? 11u : 10u); ++index) {
            parameters[index] = state.module.AllocateId();
            state.module.AddFunction(spv::OpFunctionParameter, types[index], parameters[index]);
        }
        EmitLabel(state, state.module.AllocateId());
        const auto zero = components == 1u ? ConstantU32(state, 0u) : ConstantU32CompositeZero(state, components);
        if (stops) state.bdaStopValue = store ? ConstantU32(state, 1u) : state.module.Constant(spv::OpConstantComposite, resultType, zero, ConstantU32(state, 1u));
        state.bdaPcOverride = parameters[9];
        const RuntimeDescriptor shared{parameters[0], parameters[1], parameters[2], parameters[3], parameters[4], parameters[5], parameters[6], parameters[7], parameters[8]};
        const auto value = formatSwitch(context, instruction, shared, components, store, store ? parameters[10] : 0u);
        if (!stops && store) {
            state.module.AddFunction(spv::OpReturn);
        } else {
            auto returned = stops && store ? ConstantU32(state, 0u) : value;
            if (stops && !store) {
                returned = state.module.AllocateId();
                state.module.AddFunction(spv::OpCompositeConstruct, resultType, returned, value, ConstantU32(state, 0u));
            }
            state.module.AddFunction(spv::OpReturnValue, returned);
        }
        state.module.AddFunction(spv::OpFunctionEnd);
        function = state.formattedGpuBufferFunctions.emplace(key, id).first;
    }
    const auto offset = Binary(state, spv::OpIAdd, u32, context.Arg(instruction, 2u), ConstantU32(state, memory.offset));
    std::vector<std::uint32_t> call{spv::OpFunctionCall, resultType, state.module.AllocateId(), function->second, resource.base, resource.records, resource.stride, resource.word3, resource.swizzle, resource.index, resource.valid, offset, context.Arg(instruction, 3u), BdaInstructionPc(state, instruction)};
    if (store) call.push_back(data);
    state.module.AddFunction(call);
    const auto result = call[2];
    if (!stops) return store ? ConstantU32(state, 0u) : result;
    if (store) {
        StopBdaInvocationIf(state, Binary(state, spv::OpINotEqual, boolean, result, ConstantU32(state, 0u)));
        return ConstantU32(state, 0u);
    }
    const auto stopped = state.module.AllocateId();
    state.module.AddFunction(spv::OpCompositeExtract, u32, stopped, result, 1u);
    StopBdaInvocationIf(state, Binary(state, spv::OpINotEqual, boolean, stopped, ConstantU32(state, 0u)));
    const auto value = state.module.AllocateId();
    state.module.AddFunction(spv::OpCompositeExtract, valueType, value, result, 0u);
    return value;
}

}

std::uint32_t EmitRuntimeBufferLoad(SpirvValueEmitContext& context, const IrValue& instruction, std::uint32_t components) {
    auto& state = context.state;
    const auto& memory = context.Memory(instruction);
    const auto type = components == 1u ? TypeU32(state) : TypeU32Composite(state, components);
    const auto zero = components == 1u ? ConstantU32(state, 0u) : ConstantU32CompositeZero(state, components);
    return EmitValueOrDefaultIfCondition(state, context.Arg(instruction, instruction.ArgumentCount() - 1u), type, zero, [&] {
        const auto resource = descriptor(context, instruction);
        if (memory.formatted) return EmitValueOrDefaultIfCondition(state, resource.valid, type, zero, [&] { return formattedDispatch(context, instruction, resource, components, false, 0u); });
        const auto readComponents = [&] {
            std::array<std::uint32_t, 4> values{};
            for (std::uint32_t component = 0; component < components; ++component) {
                const auto access = address(context, instruction, resource, component * 4u, memory.dataBits / 8u);
                values[component] = EmitValueOrZeroIfCondition(state, access.inBounds, [&] { return readBuffer(context, instruction, access.guest, memory.dataBits); });
            }
            return composite(state, components, values);
        };
        if (components != 2u || !memory.coherent || memory.gpuDescriptor || state.storageBufferU64Variable == 0u) return readComponents();
        const auto runtime = address(context, instruction, resource, 0u, 8u);
        const auto wide = directAccess(context, instruction, true);
        const auto byte = EmitAddU32(state, runtime.guest, wide.byteOffset);
        const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), byte, ConstantU32(state, 3u));
        const auto aligned = both(state, equal(state, baseMisalignment(context, instruction), ConstantU32(state, 0u)), equal(state, EmitAndConstant(state, byte, 7u), ConstantU32(state, 0u)));
        const auto inBounds = both(state, runtime.inBounds, EmitMemoryElementInBounds(state, wide, index));
        return EmitValueIfElse(state, both(state, aligned, inBounds), type, [&] {
            const auto pointer = EmitStorageBufferElementPointer(state, wide, index, TypeStorageBufferU64ElementPointer(state));
            const auto value = state.module.AllocateId();
            state.module.AddFunction(spv::OpAtomicLoad, TypeScalarU64(state), value, pointer, ConstantU32(state, spv::ScopeDevice), ConstantU32(state, spv::MemorySemanticsMaskNone));
            const auto high = Binary(state, spv::OpShiftRightLogical, TypeScalarU64(state), value, BdaConstant(state, 32u));
            return composite(state, 2u, {Unary(state, spv::OpUConvert, TypeU32(state), value), Unary(state, spv::OpUConvert, TypeU32(state), high), 0u, 0u});
        }, readComponents);
    });
}

void EmitRuntimeBufferStore(SpirvValueEmitContext& context, const IrValue& instruction, std::uint32_t components) {
    auto& state = context.state;
    const auto& memory = context.Memory(instruction);
    EmitIfCondition(state, context.Arg(instruction, instruction.ArgumentCount() - 1u), [&] {
        const auto resource = descriptor(context, instruction);
        if (memory.formatted) {
            EmitIfCondition(state, resource.valid, [&] { static_cast<void>(formattedDispatch(context, instruction, resource, components, true, storeData(context, instruction))); });
            return;
        }
        const auto values = storeValues(state, storeData(context, instruction), components);
        for (std::uint32_t component = 0; component < components; ++component) {
            const auto access = address(context, instruction, resource, component * 4u, memory.dataBits / 8u);
            EmitIfCondition(state, access.inBounds, [&] { writeBuffer(context, instruction, access.guest, values[component], memory.dataBits); });
        }
    });
}

std::uint32_t EmitRuntimeScalarBufferLoad(SpirvValueEmitContext& context, const IrValue& instruction) {
    auto& state = context.state;
    if (std::ranges::find(state.supportedCapabilities, spv::CapabilityInt64) == state.supportedCapabilities.end()) context.Fail(instruction, "scalar buffer bounds require shaderInt64");
    state.module.EmitCapability(spv::CapabilityInt64);
    const auto resource = descriptor(context, instruction);
    const auto u64 = TypeScalarU64(state);
    const auto size = Select(state, u64, equal(state, resource.stride, ConstantU32(state, 0u)), Unary(state, spv::OpUConvert, u64, resource.records), Binary(state, spv::OpIMul, u64, Unary(state, spv::OpUConvert, u64, resource.stride), Unary(state, spv::OpUConvert, u64, resource.records)));
    const auto address = EmitAddU32(state, context.Arg(instruction, 1u), ConstantU32(state, context.Memory(instruction).offset));
    const auto noCarry = Binary(state, spv::OpUGreaterThanEqual, TypeBool(state), address, context.Arg(instruction, 1u));
    const auto byte = EmitAndConstant(state, address, ~3u);
    const auto offset = Unary(state, spv::OpUConvert, u64, byte);
    const auto inBounds = both(state, noCarry, both(state, resource.valid, Binary(state, spv::OpULessThanEqual, TypeBool(state), Binary(state, spv::OpIAdd, u64, offset, BdaConstant(state, 4u)), size)));
    if (!context.Memory(instruction).gpuDescriptor) return EmitValueOrZeroIfCondition(state, inBounds, [&] { return readBuffer(context, instruction, byte, 32u); });
    return EmitValueOrZeroIfCondition(state, inBounds, [&] { return readBuffer(context, instruction, AddBdaAddress(context, instruction, resource.base, offset, false), 32u); });
}

std::uint32_t EmitRuntimeBufferAtomic(SpirvValueEmitContext& context, const IrValue& instruction, std::uint32_t bits, std::uint32_t active, const std::function<std::uint32_t(std::uint32_t)>& operation) {
    auto& state = context.state;
    if (bits != 32u && bits != 64u) context.Fail(instruction, "unsupported runtime buffer atomic width");
    if (context.Memory(instruction).gpuDescriptor && (state.bdaAtomicPointerFunction == 0u || state.bdaNoteWriteFunction == 0u)) context.Fail(instruction, "BDA atomic functions are missing");
    const auto type = bits == 64u ? TypeU64(state) : TypeU32(state);
    const auto scalar = bits == 64u ? TypeScalarU64(state) : TypeU32(state);
    const auto zero = bits == 64u ? ConstantU64(state, 0u) : ConstantU32(state, 0u);
    return EmitValueOrDefaultIfCondition(state, active, type, zero, [&] {
        const auto resource = descriptor(context, instruction);
        const auto access = address(context, instruction, resource, 0u, bits / 8u);
        return EmitValueOrDefaultIfCondition(state, access.inBounds, type, zero, [&] {
            if (!context.Memory(instruction).gpuDescriptor) {
                const auto storage = directAccess(context, instruction, bits == 64u);
                const auto byte = EmitAddU32(state, access.guest, storage.byteOffset);
                const auto unaligned = nonzero(state, EmitAndConstant(state, byte, bits / 8u - 1u));
                const auto guest = Binary(state, spv::OpIAdd, TypeScalarU64(state), resource.base, Unary(state, spv::OpUConvert, TypeScalarU64(state), access.guest));
                faultIf(context, instruction, unaligned, guest, bits / 8u, BdaAbi::FaultReason::Unaligned);
                const auto index = Binary(state, spv::OpShiftRightLogical, TypeU32(state), byte, ConstantU32(state, bits == 64u ? 3u : 2u));
                return EmitValueOrDefaultIfCondition(state, both(state, Unary(state, spv::OpLogicalNot, TypeBool(state), unaligned), EmitMemoryElementInBounds(state, storage, index)), type, zero, [&] {
                    const auto pointer = EmitStorageBufferElementPointer(state, storage, index, bits == 64u ? TypeStorageBufferU64ElementPointer(state) : TypeStorageBufferElementPointer(state));
                    return operation(pointer);
                });
            }
            const auto unaligned = Binary(state, spv::OpINotEqual, TypeBool(state), Binary(state, spv::OpBitwiseAnd, TypeScalarU64(state), access.guest, BdaConstant(state, bits / 8u - 1u)), BdaConstant(state, 0u));
            faultIf(context, instruction, unaligned, access.guest, bits / 8u, BdaAbi::FaultReason::Unaligned);
            return EmitValueOrDefaultIfCondition(state, Unary(state, spv::OpLogicalNot, TypeBool(state), unaligned), type, zero, [&] {
                const auto physical = state.module.AllocateId();
                state.module.AddFunction(spv::OpFunctionCall, TypeScalarU64(state), physical, state.bdaAtomicPointerFunction, access.guest, ConstantU32(state, bits / 8u), BdaInstructionPc(state, instruction));
                const auto mapped = Binary(state, spv::OpINotEqual, TypeBool(state), physical, BdaConstant(state, 0u));
                return EmitValueOrDefaultIfCondition(state, mapped, type, zero, [&] {
                    const auto pointer = state.module.AllocateId();
                    state.module.AddFunction(spv::OpConvertUToPtr, TypePointer(state, spv::StorageClassPhysicalStorageBuffer, scalar), pointer, physical);
                    const auto result = operation(pointer);
                    state.module.AddFunction(spv::OpFunctionCall, state.module.Type(spv::OpTypeVoid), state.module.AllocateId(), state.bdaNoteWriteFunction, access.guest);
                    return result;
                });
            });
        });
    });
}

}
