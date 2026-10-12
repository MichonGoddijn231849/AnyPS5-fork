#ifndef CORE_SHADER_RECOMPILER_RUNTIMEABI_HPP
#define CORE_SHADER_RECOMPILER_RUNTIMEABI_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <stdexcept>

namespace ShaderRecompiler::RuntimeAbi {

inline constexpr std::uint32_t Version = 16u;
inline constexpr std::uint32_t DescriptorSet = 0u;
inline constexpr std::uint32_t StageCount = 4u;
inline constexpr std::uint32_t PushConstantDwords = 32u;
inline constexpr std::uint32_t FirstImageBinding = 1u;
inline constexpr std::uint32_t FirstComparisonImageBinding = 22u;
inline constexpr std::uint32_t FirstStorageImageBinding = 29u;
inline constexpr std::uint32_t ImageBindingCount = 56u;

enum class Binding : std::uint32_t {
    Buffers = 0u,
    Samplers = 57u,
    Gds = 58u,
    BdaPagetable = 59u,
    FaultBuffer = 60u,
    FlattenedSrt = 61u,
    ShaderData = 62u,
    // APS5_WIDE_BINDLESS: the bound entries of the wide bindless image tables, one array per image dimension and
    // numeric class (WideImages + dimension index * 3 + class, float, uint, sint), each entry at its own element
    // (null in the other arrays); a table's key map in the flattened SRT names its entries' class and element.
    WideImages = 63u,
    // The flags of the wide table entries the shader samples (see WideFeedback).
    WideFeedback = 84u,
    Count = 85u
};

inline constexpr std::uint32_t WideImageClasses = 3u;
inline constexpr std::uint32_t WideImageDimensions = 7u;
inline constexpr std::uint32_t WideImageKinds = WideImageClasses * WideImageDimensions;

inline bool IsWideImages(Binding binding) {
    const auto value = static_cast<std::uint32_t>(binding);
    return value >= static_cast<std::uint32_t>(Binding::WideImages) && value < static_cast<std::uint32_t>(Binding::WideImages) + WideImageKinds;
}

enum class Stage : std::uint32_t { Main, Fragment, TessellationControl, TessellationEvaluation };

inline constexpr std::uint32_t UserDataCapacity = 128u;
inline constexpr std::uint32_t BufferCapacity = 128u;
inline constexpr std::uint32_t ImageCapacity = 256u;
// GTA V's shaders combine a bindless table (BindlessSlots) with further images of the same group.
inline constexpr std::uint32_t SampledHeapCapacity = ImageCapacity;
inline constexpr std::uint32_t BindlessTableSlots = 16u;
inline constexpr std::uint32_t StorageMipSlots = 4u;
inline constexpr std::uint32_t StorageHeapCapacity = 16u * StorageMipSlots;
inline constexpr std::uint32_t SamplerHeapCapacity = 64u;
inline constexpr std::uint32_t WideImageCapacity = 16384u;

struct ResourceMetadata {
    std::uint32_t binding;
    std::uint32_t firstElement;
    std::uint32_t elementCount;
    std::uint32_t flags;
    std::array<std::uint32_t, 8> descriptor;
};

struct ShaderData {
    std::uint32_t version;
    std::uint32_t imageCount;
    std::uint32_t samplerCount;
    std::uint32_t reserved;
    std::array<std::uint32_t, UserDataCapacity> userData;
    std::array<std::uint32_t, BufferCapacity / 4u> bufferOffsets;
    std::array<std::uint32_t, 4> dispatchThreadLimit;
    std::array<ResourceMetadata, ImageCapacity> images;
    std::array<ResourceMetadata, SamplerHeapCapacity> samplers;
    std::array<std::uint32_t, 8> exportMappings;
};

inline constexpr std::uint32_t UserDataDword = offsetof(ShaderData, userData) / sizeof(std::uint32_t);
inline constexpr std::uint32_t BufferOffsetsDword = offsetof(ShaderData, bufferOffsets) / sizeof(std::uint32_t);
inline constexpr std::uint32_t DispatchThreadLimitDword = offsetof(ShaderData, dispatchThreadLimit) / sizeof(std::uint32_t);
inline constexpr std::uint32_t ExportMappingsDword = offsetof(ShaderData, exportMappings) / sizeof(std::uint32_t);
inline constexpr std::uint32_t ShaderDataDwords = sizeof(ShaderData) / sizeof(std::uint32_t);

inline std::uint32_t HeapCapacity(Binding binding) {
    const auto value = static_cast<std::uint32_t>(binding);
    if (value >= FirstImageBinding && value < FirstStorageImageBinding) return SampledHeapCapacity;
    if (value >= FirstStorageImageBinding && value < static_cast<std::uint32_t>(Binding::Samplers)) return StorageHeapCapacity;
    if (binding == Binding::Samplers) return SamplerHeapCapacity;
    if (IsWideImages(binding)) return WideImageCapacity;
    throw std::runtime_error("Shader runtime ABI: binding is not a typed heap");
}

static_assert(std::is_standard_layout_v<ResourceMetadata> && std::is_trivially_copyable_v<ResourceMetadata> && sizeof(ResourceMetadata) == 48u);
static_assert(std::is_standard_layout_v<ShaderData> && std::is_trivially_copyable_v<ShaderData> && sizeof(ShaderData) == 16064u);
static_assert(UserDataDword == 4u && BufferOffsetsDword == 132u && DispatchThreadLimitDword == 164u);
static_assert(ExportMappingsDword == 4008u);
static_assert(offsetof(ResourceMetadata, descriptor) == 16u && offsetof(ShaderData, images) == 672u && offsetof(ShaderData, samplers) == 12960u);

inline void RequireVersion(std::uint32_t version) {
    if (version != Version) throw std::runtime_error("Shader runtime ABI: incompatible version");
}

inline std::uint32_t BindingNumber(Stage stage, Binding binding) {
    const auto group = static_cast<std::uint32_t>(stage);
    const auto index = static_cast<std::uint32_t>(binding);
    const auto count = static_cast<std::uint32_t>(Binding::Count);
    if (group >= StageCount || index >= count) throw std::runtime_error("Shader runtime ABI: invalid stage or binding");
    return group * count + index;
}

static_assert(FirstImageBinding + ImageBindingCount == static_cast<std::uint32_t>(Binding::Samplers));

}

#endif
