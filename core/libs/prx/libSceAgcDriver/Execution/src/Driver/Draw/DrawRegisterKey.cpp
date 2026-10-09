#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace AgcDriver::DriverDetail {

// APS5_DRAW_KEY_MEMO skips the context ranges as a prefix of the table.
static_assert([] {
    bool other = false;
    for (const auto& range : Graphics::DrawKeyRegisters) {
        if (range.bank != Graphics::RegisterBank::Context) other = true;
        else if (other) return false;
    }
    return true;
}(), "DrawKeyRegisters lists the context ranges first");

std::uint64_t Driver::drawRegisterKey(const QueueState& queue, const ShaderRegistry& registry, std::uint64_t deviceSerial, std::uint64_t* shape) {
    static const bool allUserWords = std::getenv("APS5_DRAW_KEY_ALL_USER_WORDS") != nullptr;
    std::uint64_t key = 0xcbf29ce484222325ull;
    std::uint64_t shapeKey = 0xcbf29ce484222325ull;
    const auto mixKey = [&](std::uint64_t value) {
        key ^= value;
        key *= 0x100000001b3ull;
    };
    const auto mix = [&](std::uint64_t value) {
        mixKey(value);
        shapeKey ^= value;
        shapeKey *= 0x100000001b3ull;
    };
    const auto userEnd = [&](std::uint32_t base) {
        const auto resources = queue.shader.find(base - 1);
        if (allUserWords) return base + 32u;
        if (resources == queue.shader.end()) return base;
        const auto count = ((resources->second >> 1u) & 0x1fu) | (((resources->second >> 27u) & 1u) << 5u);
        return base + std::min(count, 32u);
    };
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 3> users{{{0x00cu, userEnd(0x00cu)}, {0x08cu, userEnd(0x08cu)}, {0x10cu, userEnd(0x10cu)}}};
    const auto skipped = [&](std::uint32_t offset) {
        for (const auto& [first, end] : users) {
            if (offset >= first && offset < first + 32u) return offset >= end ? 2 : 1;
        }
        return offset == 0x082u || offset == 0x083u || offset == 0x102u || offset == 0x103u ? 1 : 0;
    };
    mix(deviceSerial);
    // APS5_DRAW_KEY_MEMO=1: the context ranges come first in DrawKeyRegisters (bank order) and are most of it, so the
    // hash state after them is kept per thread with the context bank's version; an unchanged context bank skips
    // them. "verify" also hashes them and aborts on a difference.
    static const char* memoSetting = std::getenv("APS5_DRAW_KEY_MEMO");
    static const bool memo = memoSetting == nullptr || (memoSetting[0] != '\0' && memoSetting[0] != '0');
    static const bool verify = memo && memoSetting != nullptr && std::strcmp(memoSetting, "verify") == 0;
    struct ContextMemo {
        const Registers* bank = nullptr;
        std::uint64_t version = 0;
        std::uint64_t deviceSerial = 0;
        std::uint64_t key = 0;
        std::uint64_t shapeKey = 0;
    };
    thread_local ContextMemo contextMemo;
    std::size_t firstRange = 0;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> remembered;
    if (memo && contextMemo.bank == &queue.context && contextMemo.version == queue.context.Version() && contextMemo.deviceSerial == deviceSerial) {
        remembered.emplace(contextMemo.key, contextMemo.shapeKey);
        if (!verify) {
            key = contextMemo.key;
            shapeKey = contextMemo.shapeKey;
            while (firstRange < Graphics::DrawKeyRegisters.size() && Graphics::DrawKeyRegisters[firstRange].bank == Graphics::RegisterBank::Context) ++firstRange;
        }
    }
    for (std::size_t index = firstRange; index < Graphics::DrawKeyRegisters.size(); ++index) {
        const auto& range = Graphics::DrawKeyRegisters[index];
        if (memo && firstRange == 0 && range.bank != Graphics::RegisterBank::Context && (index == 0 || Graphics::DrawKeyRegisters[index - 1].bank == Graphics::RegisterBank::Context)) {
            if (remembered && (remembered->first != key || remembered->second != shapeKey)) {
                std::fprintf(stderr, "[draw key] APS5_DRAW_KEY_MEMO verify: the context bank's hash changed at an unchanged version\n");
                std::abort();
            }
            contextMemo = {&queue.context, queue.context.Version(), deviceSerial, key, shapeKey};
        }
        const auto& bank = range.bank == Graphics::RegisterBank::Context ? queue.context : range.bank == Graphics::RegisterBank::Shader ? queue.shader : queue.userConfig;
        mix((static_cast<std::uint64_t>(range.bank) << 32u) | range.first);
        const auto end = range.first + range.count;
        const bool shader = range.bank == Graphics::RegisterBank::Shader;
        for (auto it = bank.lower_bound(range.first); it != bank.end() && it->first < end; ++it) {
            const auto skip = shader ? skipped(it->first) : 0;
            if (skip == 2) continue;
            if (skip == 1) {
                mixKey(it->first);
                mixKey(it->second);
                continue;
            }
            mix(it->first);
            mix(it->second);
        }
    }
    for (const auto base : {0x008u, 0x088u, 0x0c8u, 0x108u, 0x148u}) {
        const auto low = queue.shader.find(base);
        const auto high = queue.shader.find(base + 1);
        if (low == queue.shader.end() || high == queue.shader.end()) {
            mix(0);
            continue;
        }
        const auto address = (static_cast<std::uint64_t>(low->second) << 8u) | (static_cast<std::uint64_t>(high->second & 0xffu) << 40u);
        auto it = registry.upper_bound(address);
        if (it == registry.begin()) {
            mix(1);
            continue;
        }
        --it;
        mix(reinterpret_cast<std::uintptr_t>(it->second.get()));
        mix(address - it->second->codeAddress);
    }
    if (shape != nullptr) *shape = shapeKey;
    return key;
}

bool Driver::sameVertexInfo(const ShaderRecompiler::ShaderVertexStageInfo& a, const ShaderRecompiler::ShaderVertexStageInfo& b) {
    if (a.resourcesNum != b.resourcesNum || a.fetchAttribReg != b.fetchAttribReg || a.fetchBufferReg != b.fetchBufferReg || a.fetchEmbedded != b.fetchEmbedded || a.paClVsOutCntl != b.paClVsOutCntl) return false;
    for (std::uint32_t i = 0; i < a.resourcesNum && i < a.resources.size(); ++i) {
        if (a.resources[i].fields != b.resources[i].fields) return false;
        const auto& x = a.resourcesDst[i];
        const auto& y = b.resourcesDst[i];
        if (x.registerStart != y.registerStart || x.registersNum != y.registersNum || x.attrId != y.attrId || x.fetchIndex != y.fetchIndex) return false;
    }
    return true;
}

bool Driver::sameDecode(const DrawDecode& a, const DrawDecode& b) {
    const auto& s = a.state;
    const auto& t = b.state;
    const auto sameColor = [](const Graphics::ColorTarget& x, const Graphics::ColorTarget& y) {
        return x.address == y.address && x.extent.width == y.extent.width && x.extent.height == y.extent.height && x.format == y.format && x.bytes == y.bytes && x.componentMapping == y.componentMapping && x.tileMode == y.tileMode && x.elementBytes == y.elementBytes && x.dccAddress == y.dccAddress && x.dccAlphaOnMsb == y.dccAlphaOnMsb && x.slot == y.slot && x.exportIndex == y.exportIndex;
    };
    const auto sameBlend = [](const VkPipelineColorBlendAttachmentState& x, const VkPipelineColorBlendAttachmentState& y) {
        return x.blendEnable == y.blendEnable && x.srcColorBlendFactor == y.srcColorBlendFactor && x.dstColorBlendFactor == y.dstColorBlendFactor && x.colorBlendOp == y.colorBlendOp && x.srcAlphaBlendFactor == y.srcAlphaBlendFactor && x.dstAlphaBlendFactor == y.dstAlphaBlendFactor && x.alphaBlendOp == y.alphaBlendOp && x.colorWriteMask == y.colorWriteMask;
    };
    const auto sameMesh = [](const std::optional<ShaderRecompiler::MeshConfiguration>& x, const std::optional<ShaderRecompiler::MeshConfiguration>& y) {
        if (x.has_value() != y.has_value()) return false;
        if (!x) return true;
        return x->inputPrimitive == y->inputPrimitive && x->primitivesPerGroup == y->primitivesPerGroup && x->verticesPerGroup == y->verticesPerGroup && x->maxVertices == y->maxVertices && x->maxPrimitives == y->maxPrimitives && x->threadsPerGroup == y->threadsPerGroup && x->ldsSizeDwords == y->ldsSizeDwords && x->provokingVertex == y->provokingVertex && x->esgsItemSize == y->esgsItemSize;
    };
    const auto sameTess = [](const std::optional<ShaderRecompiler::TessellationConfiguration>& x, const std::optional<ShaderRecompiler::TessellationConfiguration>& y) {
        if (x.has_value() != y.has_value()) return false;
        if (!x) return true;
        return x->inputControlPoints == y->inputControlPoints && x->outputControlPoints == y->outputControlPoints && x->domain == y->domain && x->partitioning == y->partitioning && x->outputTopology == y->outputTopology;
    };
    if (s.stages.path != t.stages.path || s.stages.registerValue != t.stages.registerValue || s.stages.vertexWaveSize != t.stages.vertexWaveSize || s.stages.fragmentWaveSize != t.stages.fragmentWaveSize || !sameMesh(s.stages.mesh, t.stages.mesh) || !sameTess(s.stages.tessellation, t.stages.tessellation)) return false;
    if (!sameColor(s.color, t.color) || s.colors.size() != t.colors.size() || s.blends.size() != t.blends.size()) return false;
    for (std::size_t i = 0; i < s.colors.size(); ++i) {
        if (!sameColor(s.colors[i], t.colors[i])) return false;
    }
    for (std::size_t i = 0; i < s.blends.size(); ++i) {
        if (!sameBlend(s.blends[i], t.blends[i])) return false;
    }
    if (s.hasColorTarget != t.hasColorTarget || s.rectList != t.rectList || s.renderExtent.width != t.renderExtent.width || s.renderExtent.height != t.renderExtent.height || s.topology != t.topology || s.negativeOneToOne != t.negativeOneToOne || s.depthClamp != t.depthClamp || s.cullMode != t.cullMode || s.frontFace != t.frontFace || !sameBlend(s.blend, t.blend) || s.blendConstants != t.blendConstants) return false;
    if (std::memcmp(&s.viewport, &t.viewport, sizeof(VkViewport)) != 0 || std::memcmp(&s.scissor, &t.scissor, sizeof(VkRect2D)) != 0) return false;
    const auto& p = a.pixel;
    const auto& q = b.pixel;
    if (p.interpolatorCount != q.interpolatorCount || p.interpolatorSettings != q.interpolatorSettings || p.wave32 != q.wave32 || p.inputAddr != q.inputAddr || p.hasPerspectiveCenterVgpr != q.hasPerspectiveCenterVgpr || p.perspectiveCentroid != q.perspectiveCentroid || p.posX != q.posX || p.posY != q.posY || p.posZ != q.posZ || p.posW != q.posW || p.frontFace != q.frontFace || p.ancillary != q.ancillary || p.sampleShading != q.sampleShading || p.noPerspective != q.noPerspective || p.linearCentroid != q.linearCentroid || p.pixelKillEnable != q.pixelKillEnable || p.depthExportEnable != q.depthExportEnable || p.sampleMaskExportEnable != q.sampleMaskExportEnable || p.earlyZ != q.earlyZ || p.executeOnNoop != q.executeOnNoop || p.targetOutputMode != q.targetOutputMode || p.targetExportMapping != q.targetExportMapping) return false;
    if (a.roles != b.roles || a.programs.size() != b.programs.size()) return false;
    for (std::size_t i = 0; i < a.programs.size(); ++i) {
        const auto& x = a.programs[i];
        const auto& y = b.programs[i];
        if (x.binary.stage != y.binary.stage || x.binary.codeAddress != y.binary.codeAddress || x.userDataBase != y.userDataBase || x.firstUserSgpr != y.firstUserSgpr || x.userData != y.userData || x.snapshot != y.snapshot || x.codeOffset != y.codeOffset) return false;
    }
    return true;
}

}
