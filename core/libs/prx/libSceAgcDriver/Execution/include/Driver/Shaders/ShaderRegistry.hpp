#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERREGISTRY_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_SHADERREGISTRY_HPP

#include "prx/libSceAgcDriver/Execution/include/ShaderMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/QueueState.hpp"
#include <array>
#include <optional>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ShaderRecompiler {
class ShaderPreparationContext;
}

namespace AgcDriver {
class VulkanDevice;
}

namespace AgcDriver::DriverDetail {

struct ShaderSnapshot;
using ShaderRegistry = std::map<std::uint64_t, std::shared_ptr<const ShaderSnapshot>>;

struct PreparedShaderState {
    struct Entry {
        std::size_t codeOffset;
        std::shared_ptr<const ShaderRecompiler::SourceHandle> handle;
    };
    struct Failure {
        std::size_t codeOffset;
        std::vector<std::uint64_t> key;
        std::string message;
    };
    struct Rectangle {
        std::uint64_t vertexId;
        std::uint64_t fragmentId;
        std::shared_ptr<const ShaderRecompiler::RectListShaders> shaders;
    };
    struct RectangleProgress {
        std::weak_ptr<const ShaderSnapshot> fragment;
        std::size_t vertexCount;
        std::size_t fragmentCount;
    };
    struct GraphicsAbi {
        std::vector<std::uint64_t> key;
        std::weak_ptr<const ShaderRegistry> registry;
        std::vector<std::weak_ptr<const ShaderSnapshot>> stages;
    };
    std::vector<Entry> entries;
    std::vector<Failure> failures;
    std::vector<std::vector<std::uint64_t>> registeredAbis;
    std::vector<GraphicsAbi> graphicsAbis;
    std::vector<Rectangle> rectangles;
    std::vector<RectangleProgress> rectangleProgress;
    std::vector<std::weak_ptr<const ShaderSnapshot>> fragments;
    bool rectangleRequested = false;
    bool deferred = false;
};
struct PreparedShaders : PreparedShaderState {
    std::mutex mutex;
    std::condition_variable settled;
    std::exception_ptr failure;
    bool pending = false;
};

struct RegisteredShaderState {
    Registers shader;
    Registers context;
    Registers userConfig;
};

// The code hash per program offset (RecompileCacheKey::HashCode of the code from that offset on): the
// snapshot's code never changes, so a draw's cache key need not hash kilobytes of it again.
struct CodeHashMemo {
    struct CodeHash {
        std::size_t offset = ~std::size_t{0};
        std::uint64_t hash = 0;
    };
    std::mutex mutex;
    std::array<CodeHash, 4> codeHashes;
    std::size_t nextCodeHash = 0;
};

struct ShaderSnapshot {
    std::uint64_t codeAddress;
    std::uint64_t headerAddress;
    std::uint8_t type;
    std::vector<std::uint32_t> code;
    std::vector<std::byte> header;
    std::shared_ptr<PreparedShaders> prepared = std::make_shared<PreparedShaders>();
    std::shared_ptr<const RegisteredShaderState> registeredState;
    std::shared_ptr<CodeHashMemo> codeHashes = std::make_shared<CodeHashMemo>();
};


std::shared_ptr<const ShaderSnapshot> ReadRawComputeShader(std::uint64_t address);

std::shared_ptr<const ShaderRecompiler::SourceHandle> PrepareShaderWithDiagnostics(const ShaderRecompiler::RecompileRequest& request, ShaderRecompiler::ShaderPreparationContext* preparation = nullptr);

std::uint64_t NullPixelProgramAddress();
// RecompileCacheKey::HashCode of the snapshot's code from `codeOffset` on, computed once per offset.
std::uint64_t SnapshotCodeHash(const ShaderSnapshot& snapshot, std::size_t codeOffset);
ShaderSnapshot PrepareNullPixelProgram(const VulkanDevice& device);
std::optional<ShaderRecompiler::ShaderFloatMode> RegisteredFloatMode(const ShaderSnapshot& snapshot);
void PublishRegisteredShader(std::shared_ptr<ShaderRegistry>& registry, const std::shared_ptr<const ShaderSnapshot>& snapshot);

void ResolvePreparedGraphics(const ShaderSnapshot& front, const std::shared_ptr<const ShaderSnapshot>& fragment, std::uint32_t primitiveType, const ShaderRecompiler::SpirvTarget& target);

ShaderRecompiler::RectListShaders PreparedRectangle(const ShaderSnapshot& snapshot, std::uint64_t vertexId, std::uint64_t fragmentId);
// The draw's rect-list stages: the pair's prepared artifacts, or (a pair the registration did not link, a stage
// prepared at first use) built from the draw's own results and kept. APS5_NO_LAZY_PREPARE=1 keeps the error.
ShaderRecompiler::RectListShaders PreparedRectangle(const ShaderSnapshot& snapshot, const ShaderRecompiler::RecompileResult& vertex, const ShaderRecompiler::RecompileResult& fragment, const ShaderRecompiler::SpirvTarget& target);
ShaderRecompiler::RectListShaders DrawRectangle(const ShaderSnapshot& front, const std::shared_ptr<const ShaderSnapshot>& fragment, std::uint64_t vertexId, std::uint64_t fragmentId, const ShaderRecompiler::SpirvTarget& target);

std::shared_ptr<const ShaderRecompiler::SourceHandle> SourceHandleFor(const ShaderSnapshot& snapshot, std::size_t codeOffset, const ShaderRecompiler::RecompileRequest& request);
ShaderRecompiler::PreparedShaderInvocation InvocationFor(const ShaderSnapshot& snapshot, std::size_t codeOffset, const ShaderRecompiler::RecompileRequest& request);

namespace PreparationProfile {
enum class Phase : std::size_t { Registration, AbiHint, FirstUse, Invocation, SourceHandle, Count };
bool Enabled();
void Add(Phase phase, std::chrono::steady_clock::time_point start, std::size_t scanned = 0, bool failed = false);
}

}

#endif
