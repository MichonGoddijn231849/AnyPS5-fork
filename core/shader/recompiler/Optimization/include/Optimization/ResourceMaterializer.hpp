#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_RESOURCEMATERIALIZER_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_INCLUDE_OPTIMIZATION_RESOURCEMATERIALIZER_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"
#include "RuntimeAbi.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace ShaderRecompiler {

// Why a bindless image table (a T# loaded from a table buffer at a runtime key) was not bound;
// counted on the [bindless] line (APS5_PROFILE_DRAW).
enum class BindlessRejection { Capacity, MaterialScan, NoEntry, Storage, NonUniform, ImageSlots, Count };

class ResourceMaterializer {
public:
    static std::uint32_t EmulatedCompareState(const ShaderInfo& info, const ResourceSnapshot& snapshot, std::uint32_t index);
    void ApplyStaticInterface(IrProgram& program, bool nativeSampleOffsets = true) const;
    static std::vector<ImageResource> RuntimeImageModes(const ImageResource& image);
    static std::uint32_t RuntimeImageMode(const ImageResource& image, const DescriptorValue& descriptor, std::span<const ImageResource> modes);
    // RuntimeImageMode, with UINT32_MAX instead of a throw when `noMatchIsValue` and no mode takes the descriptor.
    static std::uint32_t MatchRuntimeImageMode(const ImageResource& image, const DescriptorValue& descriptor, std::span<const ImageResource> modes, bool noMatchIsValue);
    static void PrepareImageModes(ShaderInfo& info);
    [[nodiscard]] IrResourcePlan ExtractPlan(const IrProgram& program) const;
    void Materialize(const IrResourcePlan& program, const SrtRuntime& runtime, ResourceSnapshot& snapshot) const;
    static std::uint64_t SpecializationNanoseconds();
    static std::uint32_t BindlessSlots();
    // APS5_WIDE_BINDLESS=1: bindless image tables bind every entry through the WideImages binding.
    static bool WideBindless();
    static bool BindlessStorageTables();
    static bool WideTableImage(const ImageResource& image);
    // The runtime mode indices of a wide table root's native float, uint and sint modes, in that class order, or
    // UINT32_MAX where the root has no such mode. A wide entry's class is its position here.
    static std::array<std::uint32_t, RuntimeAbi::WideImageClasses> WideModes(const ImageResource& image);
    // A wide table root's WideImages binding offset for `wideClass` (dimension index * 3 + class).
    static std::uint32_t WideKind(const ImageResource& image, std::uint32_t wideClass);
    static void CountBindlessRejection(BindlessRejection reason);
};

}

#endif
