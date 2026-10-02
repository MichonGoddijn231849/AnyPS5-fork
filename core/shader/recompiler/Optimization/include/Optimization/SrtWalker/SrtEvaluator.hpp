#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace ShaderRecompiler::Detail {

class ValueCache {
public:
    const std::uint64_t* Find(const IrValue* key) const {
        if (_slots.empty()) return nullptr;
        for (auto index = slotOf(key);; index = (index + 1) & (_slots.size() - 1)) {
            const auto& slot = _slots[index];
            if (slot.key == key) return &slot.value;
            if (slot.key == nullptr) return nullptr;
        }
    }
    void Insert(const IrValue* key, std::uint64_t value) {
        if ((_count + 1) * 4 > _slots.size() * 3) grow();
        for (auto index = slotOf(key);; index = (index + 1) & (_slots.size() - 1)) {
            auto& slot = _slots[index];
            if (slot.key == key) {
                slot.value = value;
                return;
            }
            if (slot.key == nullptr) {
                slot = {key, value};
                ++_count;
                return;
            }
        }
    }

private:
    struct Slot {
        const IrValue* key = nullptr;
        std::uint64_t value = 0;
    };
    std::size_t slotOf(const IrValue* key) const {
        auto bits = reinterpret_cast<std::uintptr_t>(key);
        bits ^= bits >> 17u;
        bits *= 0x9e3779b97f4a7c15ull;
        return static_cast<std::size_t>(bits >> 32u) & (_slots.size() - 1);
    }
    void grow() {
        auto old = std::move(_slots);
        _slots.assign(old.empty() ? 64 : old.size() * 2, Slot{});
        _count = 0;
        for (const auto& slot : old) {
            if (slot.key != nullptr) Insert(slot.key, slot.value);
        }
    }
    std::vector<Slot> _slots;
    std::size_t _count = 0;
};

class Evaluator {
public:
    Evaluator(const IrResourcePlan& program, const SrtRuntime& runtime, std::span<const std::uint8_t> cleanFlatSlots = {}, Evaluator* cleanEvaluator = nullptr, IrValue* activeMask = nullptr) : _program(program), _runtime(runtime), _cleanFlatSlots(cleanFlatSlots), _cleanEvaluator(cleanEvaluator), _activeMask(activeMask != nullptr ? activeMask->Resolve() : nullptr) {}

    bool Evaluate(IrValue* value, std::uint32_t& result);
    bool EvaluateWide(IrValue* raw, std::uint64_t& result);

private:
    static float Float32(std::uint64_t bits);
    static std::uint64_t Float32Bits(float value);

    bool Arg(IrValue& inst, std::size_t index, std::uint64_t& result);
    bool EvaluatePhi(IrValue& inst, std::uint64_t& result);
    bool EvaluateExtract(IrValue& inst, std::uint64_t& result);
    bool EvaluateRawRead(IrValue& inst, std::uint64_t& result);
    bool EvaluateInst(IrValue& inst, std::uint64_t& result);

    const IrResourcePlan& _program;
    const SrtRuntime& _runtime;
    std::span<const std::uint8_t> _cleanFlatSlots;
    Evaluator* _cleanEvaluator = nullptr;
    IrValue* _activeMask = nullptr;
    ValueCache _cache;
    std::vector<IrValue*> _visiting;
};

}

#endif
