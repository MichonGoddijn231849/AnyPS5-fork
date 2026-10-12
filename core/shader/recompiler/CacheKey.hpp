#ifndef CORE_SHADER_RECOMPILER_CACHEKEY_HPP
#define CORE_SHADER_RECOMPILER_CACHEKEY_HPP

#include "Recompiler.hpp"
#include "RdnaDecoder/RdnaDescriptorFormat.hpp"
#include "prx/libc/include/HostThreadLocal.hpp"
#include "ControlFlow/SubroutineCalls.hpp"
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <type_traits>

namespace ShaderRecompiler {

class RecompileCacheKey {
public:
    static void Build(const RecompileRequest& request, std::vector<std::uint64_t>& key) {
        key.clear();
        append(key, RuntimeAbi::Version);
        append(key, request.shader.stage);
        // The code enters as a hash rather than word by word: the key is built, hashed and compared
        // on every dispatch and draw. The cache verifies a match against the code it stored.
        // Debug aid: APS5_NO_CODE_HASH_KEY=1 appends every code word, as before.
        static const bool hashedCode = std::getenv("APS5_NO_CODE_HASH_KEY") == nullptr;
        if (hashedCode) {
            append(key, request.shader.code.size());
            append(key, request.shader.codeHash != 0 ? request.shader.codeHash : HashCode(request.shader.code));
        } else {
            append(key, request.shader.code);
        }
        appendInterface(key, request);
    }

    static void BuildInterface(const RecompileRequest& request, std::vector<std::uint64_t>& key) {
        key.clear();
        append(key, RuntimeAbi::Version);
        append(key, request.shader.stage);
        appendInterface(key, request);
    }

    // A hash over every field Build appends except the code, the target and the probe flag: the
    // key of a source memo whose owner fixes the code (a registered shader at an offset) and the
    // device itself, and which is bypassed while the probe is active.
    static std::uint64_t ContextHash(const RecompileRequest& request) {
        struct ContextKeyStorage {};
        auto& key = HostThreadLocal<std::vector<std::uint64_t>, ContextKeyStorage>();
        key.clear();
        append(key, RuntimeAbi::Version);
        append(key, request.shader.stage);
        append(key, request.context.waveSize);
        append(key, request.context.userDataBaseRegister);
        append(key, request.context.userData.size());
        append(key, request.context.compute);
        append(key, request.context.pixel);
        append(key, request.context.vertex);
        append(key, request.context.floatMode);
        appendMesh(key, request);
        appendTessellation(key, request);
        appendCalls(key, request);
        std::uint64_t hash = 0xcbf29ce484222325ull;
        for (const auto value : key) {
            hash ^= value;
            hash *= 0x100000001b3ull;
        }
        return hash;
    }

    // A 64-bit hash of the code, two dwords per step; collisions are resolved by comparing the code.
    static std::uint64_t HashCode(std::span<const std::uint32_t> code) {
        std::uint64_t hash = 0x9e3779b97f4a7c15ull ^ (static_cast<std::uint64_t>(code.size()) * 0x100000001b3ull);
        const auto mix = [&](std::uint64_t chunk) {
            hash = (hash ^ chunk) * 0x9e3779b97f4a7c15ull;
            hash ^= hash >> 29u;
        };
        std::size_t index = 0;
        for (; index + 2 <= code.size(); index += 2) mix(code[index] | (static_cast<std::uint64_t>(code[index + 1]) << 32u));
        if (index < code.size()) mix(code[index]);
        return hash;
    }

private:
    static void appendInterface(std::vector<std::uint64_t>& key, const RecompileRequest& request) {
        append(key, request.context.waveSize);
        append(key, request.context.userDataBaseRegister);
        append(key, request.context.userData.size());
        append(key, request.context.compute);
        append(key, request.context.pixel);
        append(key, request.context.vertex);
        append(key, request.context.floatMode);
        appendMesh(key, request);
        appendTessellation(key, request);
        appendCalls(key, request);
        append(key, request.target);
        append(key, DebugProbeActive());
        append(key, RayTracingStrict());
        append(key, RayTracingMiss());
    }

    // Subroutine calls (InlineSubroutineCalls, compute programs under APS5_SUBROUTINE_CALLS=1): a program with an
    // s_swappc_b64 word inlines the callees its user data and function tables point at, so its key holds those values.
    // Once the code was inlined, only the user data words and memory words its call targets were traced to
    // (CallUserData); a word the calls never read must not make each dispatch a new program. Until then, all its user
    // data. The callees are registered code (the driver adds them to the request), fixed while registered.
    static void appendCalls(std::vector<std::uint64_t>& key, const RecompileRequest& request) {
        if (request.shader.stage != ShaderStage::Compute || !SubroutineCallsEnabled()) return;
        bool calls = false;
        for (const auto word : request.shader.code) {
            if ((word & 0xff80ff00u) == 0xbe802100u) {
                calls = true;
                break;
            }
        }
        append(key, calls);
        if (!calls) return;
        struct CallIndexStorage {};
        struct CallAddressStorage {};
        auto& indices = HostThreadLocal<std::vector<std::uint32_t>, CallIndexStorage>();
        auto& addresses = HostThreadLocal<std::vector<std::uint64_t>, CallAddressStorage>();
        if (!CallUserData(request.shader.codeAddress, request.shader.code, indices, addresses)) {
            for (const auto value : request.context.userData) append(key, value);
            return;
        }
        // The marker keeps the two key forms apart.
        append(key, 0xca11u);
        append(key, indices.size());
        for (const auto index : indices) {
            append(key, index);
            append(key, index < request.context.userData.size() ? static_cast<std::uint64_t>(request.context.userData[index]) : (1ull << 32u));
        }
        // The memory words a call's target was read from (absent from the request: a marker).
        append(key, addresses.size());
        for (const auto address : addresses) {
            std::uint64_t value = 1ull << 32u;
            for (const auto& region : request.context.memory) {
                if (address < region.guestAddress || address - region.guestAddress + sizeof(std::uint32_t) > region.bytes.size()) continue;
                std::uint32_t word = 0;
                std::memcpy(&word, region.bytes.data() + (address - region.guestAddress), sizeof(word));
                value = word;
                break;
            }
            append(key, address);
            append(key, value);
        }
    }

    static void appendMesh(std::vector<std::uint64_t>& key, const RecompileRequest& request) {
        if (request.shader.stage != ShaderStage::Mesh) return;
        const auto* mesh = request.graphics && request.graphics->mesh ? &*request.graphics->mesh : nullptr;
        append(key, mesh != nullptr);
        if (mesh == nullptr) return;
        for (const auto value : {mesh->inputPrimitive, mesh->primitivesPerGroup, mesh->verticesPerGroup, mesh->maxVertices, mesh->maxPrimitives, mesh->threadsPerGroup, mesh->ldsSizeDwords, mesh->provokingVertex, mesh->esgsItemSize}) append(key, value);
    }

    static void appendTessellation(std::vector<std::uint64_t>& key, const RecompileRequest& request) {
        if (request.shader.stage != ShaderStage::Local && request.shader.stage != ShaderStage::TessellationControl && request.shader.stage != ShaderStage::TessellationEvaluation) return;
        const auto* tessellation = request.graphics && request.graphics->tessellation ? &*request.graphics->tessellation : nullptr;
        append(key, tessellation != nullptr);
        if (tessellation == nullptr) return;
        for (const auto value : {tessellation->inputControlPoints, tessellation->outputControlPoints, tessellation->domain, tessellation->partitioning, tessellation->outputTopology}) append(key, value);
    }

    template<typename TValue>
    static void append(std::vector<std::uint64_t>& key, TValue value) requires (std::is_integral_v<TValue> || std::is_enum_v<TValue>) {
        key.push_back(static_cast<std::uint64_t>(value));
    }

    template<typename TValue, std::size_t TSize>
    static void append(std::vector<std::uint64_t>& key, const std::array<TValue, TSize>& values) {
        for (const auto value : values) append(key, value);
    }

    template<typename TValue>
    static void append(std::vector<std::uint64_t>& key, std::span<TValue> values) {
        append(key, values.size());
        for (const auto value : values) append(key, value);
    }

    template<typename TValue>
    static void append(std::vector<std::uint64_t>& key, const std::optional<TValue>& value) {
        append(key, value.has_value());
        if (value) append(key, *value);
    }

    static void append(std::vector<std::uint64_t>& key, std::string_view value) {
        append(key, value.size());
        for (const unsigned char byte : value) append(key, byte);
    }

    static void append(std::vector<std::uint64_t>& key, const ShaderFloatMode& value) {
        append(key, value.floatMode);
        append(key, value.dx10Clamp);
        append(key, value.ieeeMode);
        append(key, value.fp16Overflow);
    }

    static void append(std::vector<std::uint64_t>& key, const ShaderComputeStageInfo& value) {
        append(key, value.numThreads);
        append(key, value.ldsSizeDwords);
        append(key, value.groupIdEnable);
        append(key, value.tgSizeEnable);
        append(key, value.threadIdComponentCount);
        append(key, value.PartialGroups());
        append(key, value.scratchDwords);
    }

    static void append(std::vector<std::uint64_t>& key, const ShaderPixelStageInfo& value) {
        append(key, value.interpolatorCount);
        if (value.interpolatorCount > value.interpolatorSettings.size()) throw std::runtime_error("Shader cache: invalid interpolator count");
        for (std::uint32_t i = 0; i < value.interpolatorCount; ++i) append(key, value.interpolatorSettings[i]);
        append(key, value.wave32);
        append(key, value.inputAddr);
        append(key, value.hasPerspectiveCenterVgpr);
        append(key, value.perspectiveCentroid);
        append(key, value.posX);
        append(key, value.posY);
        append(key, value.posZ);
        append(key, value.posW);
        append(key, value.frontFace);
        append(key, value.ancillary);
        append(key, value.sampleShading);
        append(key, value.noPerspective);
        append(key, value.linearCentroid);
        append(key, value.pixelKillEnable);
        append(key, value.depthExportEnable);
        append(key, value.sampleMaskExportEnable);
        append(key, value.earlyZ);
        append(key, value.executeOnNoop);
        append(key, value.quadPixelMask);
        append(key, value.stencilExportEnable);
        append(key, value.conservativeZExport);
        append(key, value.orderedPixelShader);
        append(key, value.targetOutputMode);
    }

    static void append(std::vector<std::uint64_t>& key, const ShaderVertexResourceDestination& value) {
        append(key, value.registerStart);
        append(key, value.registersNum);
        append(key, value.attrId);
        append(key, value.fetchIndex);
    }

    static void append(std::vector<std::uint64_t>& key, const ShaderVertexStageInfo& value) {
        append(key, value.fetchAttribReg);
        append(key, value.fetchBufferReg);
        append(key, value.fetchEmbedded);
        append(key, value.paClVsOutCntl);
        if (value.resourcesNum > value.resources.size()) throw std::runtime_error("Shader cache: invalid vertex resource count");
        append(key, value.resourcesNum);
        for (std::uint32_t i = 0; i < value.resourcesNum; ++i) {
            if (!value.fetchEmbedded) append(key, VertexInputNumericClass(static_cast<IrBufferFormat>((value.resources[i].fields[3] >> 12u) & 0x7fu)));
            auto destination = value.resourcesDst[i];
            if (value.fetchEmbedded) destination.fetchIndex = 0;
            append(key, destination);
        }
    }

    static void append(std::vector<std::uint64_t>& key, const MeshTargetLimits& value) {
        append(key, value.maxWorkgroupSize);
        append(key, value.maxWorkgroupInvocations);
        append(key, value.maxSharedMemoryBytes);
        append(key, value.maxOutputVertices);
        append(key, value.maxOutputPrimitives);
        append(key, value.maxOutputComponents);
        append(key, value.maxOutputMemoryBytes);
        append(key, value.outputPerVertexGranularity);
        append(key, value.outputPerPrimitiveGranularity);
    }

    static void append(std::vector<std::uint64_t>& key, const TessellationTargetLimits& value) {
        append(key, value.maxPatchSize);
        append(key, value.maxControlPerVertexInputComponents);
        append(key, value.maxControlPerVertexOutputComponents);
        append(key, value.maxControlPerPatchOutputComponents);
        append(key, value.maxControlTotalOutputComponents);
        append(key, value.maxEvaluationInputComponents);
        append(key, value.maxEvaluationOutputComponents);
    }

    static void append(std::vector<std::uint64_t>& key, const SpirvTarget& value) {
        append(key, value.vulkanVersion);
        append(key, value.spirvVersion);
        append(key, value.subgroupSize);
        append(key, value.bdaAbiVersion);
        append(key, value.supportedCapabilities);
        append(key, value.supportedExtensions);
        append(key, value.fragmentShaderBarycentricEnabled);
        append(key, value.maxWorkgroupSize);
        append(key, value.maxWorkgroupInvocations);
        append(key, value.maxWorkgroupSharedMemoryBytes);
        append(key, value.mesh);
        append(key, value.tessellation);
        append(key, value.nonConstantImageOffsets);
        append(key, value.srgbDecodeFormats);
        append(key, value.narrowSubgroupClock);
    }
};

}

#endif
