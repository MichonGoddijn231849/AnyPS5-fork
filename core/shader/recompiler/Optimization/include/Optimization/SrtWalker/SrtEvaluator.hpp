#ifndef CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP
#define CORE_SHADER_RECOMPILIER_OPTIMIZATION_SRTWALKER_SRTEVALUATOR_HPP

#include "IntermediateRepresentation/IrProgram.hpp"
#include "Optimization/SrtWalker.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace ShaderRecompiler::Detail {

class EvaluatedValues {
public:
    bool Find(const IrValue* key, std::uint64_t& value) const {
        if (_slots.empty()) {
            return false;
        }
        for (std::size_t index = Home(key);; index = (index + 1u) & (_slots.size() - 1u)) {
            const auto& slot = _slots[index];
            if (slot.key == key) {
                value = slot.value;
                return true;
            }
            if (slot.key == nullptr) {
                return false;
            }
        }
    }
    void Insert(const IrValue* key, std::uint64_t value) {
        if ((_count + 1u) * 2u > _slots.size()) {
            Grow();
        }
        for (std::size_t index = Home(key);; index = (index + 1u) & (_slots.size() - 1u)) {
            auto& slot = _slots[index];
            if (slot.key == key) {
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
    std::size_t Home(const IrValue* key) const {
        return static_cast<std::size_t>((reinterpret_cast<std::uintptr_t>(key) >> 4u) * 0x9e3779b97f4a7c15ull >> 32u) & (_slots.size() - 1u);
    }
    void Grow() {
        std::vector<Slot> previous(_slots.empty() ? 64u : _slots.size() * 2u);
        previous.swap(_slots);
        _count = 0;
        for (const auto& slot : previous) {
            if (slot.key != nullptr) {
                Insert(slot.key, slot.value);
            }
        }
    }
    std::vector<Slot> _slots;
    std::size_t _count = 0;
};

// An evaluator's memo over a plan with dense value ids (IrResourcePlan::denseValueIds): one slot
// per value, indexed by its id. The arrays are the calling thread's, reused by later evaluators
// and walks: a slot belongs to this memo only while it carries this memo's stamp (Done) or the
// stamp plus one (the value is being evaluated: reaching it again is a cycle), so a fresh memo
// needs no clearing.
class DenseValues {
public:
    struct Slot {
        std::uint64_t stamp = 0;
        std::uint64_t value = 0;
    };
    explicit DenseValues(std::size_t values);
    ~DenseValues();
    DenseValues(const DenseValues&) = delete;
    DenseValues& operator=(const DenseValues&) = delete;
    Slot& At(std::uint32_t id) { return _slots[id]; }
    [[nodiscard]] std::size_t Size() const { return _size; }
    [[nodiscard]] std::uint64_t Done() const { return _stamp; }
    [[nodiscard]] std::uint64_t Visiting() const { return _stamp + 1u; }

private:
    std::vector<Slot>* _array = nullptr;
    Slot* _slots = nullptr;
    std::size_t _size = 0;
    std::uint64_t _stamp = 0;
};

class Evaluator {
public:
    Evaluator(const IrResourcePlan& program, const SrtRuntime& runtime, std::span<const std::uint8_t> cleanFlatSlots = {}, Evaluator* cleanEvaluator = nullptr, IrValue* activeMask = nullptr) : _program(program), _runtime(runtime), _cleanFlatSlots(cleanFlatSlots), _cleanEvaluator(cleanEvaluator), _activeMask(activeMask != nullptr ? activeMask->Resolve() : nullptr), _dense(program.denseValueIds ? program.valueStorage.size() : 0u) {}
    Evaluator(const Evaluator&) = delete;
    Evaluator& operator=(const Evaluator&) = delete;

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
    EvaluatedValues _cache;
    std::vector<IrValue*> _visiting;
    DenseValues _dense;
};

}

#endif
