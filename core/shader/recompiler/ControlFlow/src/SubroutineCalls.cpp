#include "ControlFlow/SubroutineCalls.hpp"
#include "RdnaDecoder/RdnaInstructionDecoder.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace ShaderRecompiler {

namespace {

std::string Hex(std::uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

// The registers of a lane mask in the program being inlined: one in wave32, two in wave64 (InlineSubroutineCalls sets
// it for the program's wave size).
thread_local std::uint32_t laneMaskRegisters = 2u;

// The scalar registers an instruction writes (destination and the VOP3B carry-out), as [first, first + count).
std::vector<std::uint32_t> WrittenScalars(const RdnaInstruction& instruction) {
    std::vector<std::uint32_t> written;
    const auto add = [&](const RdnaOperand& operand, std::uint32_t count) {
        if (operand.kind != RdnaOperandKind::ScalarRegister) return;
        for (std::uint32_t i = 0; i < count; ++i) written.push_back(operand.reg + i);
    };
    switch (instruction.family) {
        case RdnaInstructionFamily::SOP1:
        case RdnaInstructionFamily::SOP2:
        case RdnaInstructionFamily::SOPK:
        case RdnaInstructionFamily::SMEM:
            add(instruction.destination, std::max<std::uint32_t>(instruction.dataDwordCount, 1u));
            break;
        default:
            // v_readlane / v_readfirstlane write one register; any other vector instruction's scalar destination is a
            // lane mask (one register in wave32, two in wave64).
            if (instruction.op == RdnaOpcode::VReadlaneB32 || instruction.op == RdnaOpcode::VReadfirstlaneB32) {
                add(instruction.destination, 1u);
                break;
            }
            add(instruction.destination, laneMaskRegisters);
            add(instruction.destination2, laneMaskRegisters);
            break;
    }
    return written;
}

bool Writes(const RdnaInstruction& instruction, std::uint32_t reg) {
    const auto written = WrittenScalars(instruction);
    return std::find(written.begin(), written.end(), reg) != written.end();
}

struct Callee {
    std::uint64_t address = 0;
    std::vector<std::uint32_t> words;
    // Reachable instructions, program counters from 0 at the callee's first word.
    std::vector<RdnaInstruction> instructions;
    std::uint32_t endProgramCounter = 0;
    std::set<std::uint32_t> written;
    // The callee calls further subroutines itself (inlined in turn); what those write is not in `written`.
    bool makesCalls = false;
};

// Nested calls are inlined this deep at most; a longer chain is refused.
constexpr std::uint32_t MaxCallDepth = 8;

// The caller and its copies together hold this many words at most (1 MiB of code); more is refused.
constexpr std::size_t MaxInlinedWords = 256u * 1024u;

// s_swappc_b64 with a null destination jumps without keeping a return address: through the return pair it is the
// callee's return (the compiler emits it in place of s_setpc_b64).
bool IsNullSwap(const RdnaInstruction& instruction) {
    return instruction.op == RdnaOpcode::SSwappcB64 && instruction.destination.kind == RdnaOperandKind::Null;
}

class Inliner {
public:
    Inliner(RdnaProgram& program, std::uint64_t codeAddress, std::span<const std::uint32_t> userData, std::uint32_t userDataBaseRegister, std::span<const MemoryRegion> memory)
        : program(program), codeAddress(codeAddress), userData(userData), userDataBase(userDataBaseRegister), memory(memory) {
        for (const auto& instruction : program.instructions) {
            if (IsDirectBranchOpcode(instruction.op)) branchTargets.insert(instruction.branchTarget);
        }
    }

    void Run() {
        for (std::size_t i = 0; i < program.instructions.size(); ++i) {
            if (program.instructions[i].op != RdnaOpcode::SSwappcB64) continue;
            // A null-destination swap is a return (of a callee inside this code); it is resolved like s_setpc_b64.
            if (IsNullSwap(program.instructions[i])) {
                program.instructions[i].op = RdnaOpcode::SSetpcB64;
                program.instructions[i].destination = RdnaOperand{};
                continue;
            }
            work.push_back(Call{i, 0, NoParent, {}});
        }
        if (work.empty()) return;
        if (!SubroutineCallsEnabled()) throw std::invalid_argument("s_swappc_b64 at program counter " + Hex(program.instructions[work.front().index].programCounter) + ": subroutine calls are off (APS5_SUBROUTINE_CALLS=1 inlines them)");
        // The copies follow the whole code (data after s_endpgm, such as jump tables, stays where its loads read it);
        // the graph skips the gap between the last decoded instruction and the first copy. A copy's own calls are
        // inlined right after it (Inline), so the worklist grows while it runs.
        std::vector<std::uint32_t> code(program.code.begin(), program.code.end());
        mainCount = program.instructions.size();
        const auto originalEnd = program.code.size() * sizeof(std::uint32_t);
        for (std::size_t w = 0, mainCalls = work.size(); w < mainCalls; ++w) Inline(w, code, originalEnd);
        program.ownedCode = std::move(code);
        program.code = program.ownedCode;
    }

private:
    // Inlines work[w]'s callee after the code so far, then the calls inside that copy, depth first, so that every
    // register the whole call writes is known before a later call's target is traced back past it.
    void Inline(std::size_t w, std::vector<std::uint32_t>& code, std::size_t originalEnd) {
        const auto call = program.instructions[work[w].index];
        const auto where = "s_swappc_b64 at program counter " + Hex(GuestOffset(call));
        if (call.source0.kind != RdnaOperandKind::ScalarRegister || call.destination.kind != RdnaOperandKind::ScalarRegister) throw std::invalid_argument(where + ": the target and return address must be scalar register pairs");
        if (work[w].depth >= MaxCallDepth) throw std::invalid_argument(where + ": calls nested deeper than " + std::to_string(MaxCallDepth));
        const auto low = Value(call.source0.reg, work[w].index, w);
        const auto high = Value(call.source0.reg + 1u, work[w].index, w);
        const auto returnTo = call.programCounter + call.wordCount * static_cast<std::uint32_t>(sizeof(std::uint32_t));
        if (low.has_value() && high.has_value()) {
            const auto target = static_cast<std::uint64_t>(*low) | (static_cast<std::uint64_t>(*high) << 32u);
            program.instructions[work[w].index].resolvedJump = true;
            program.instructions[work[w].index].branchTarget = InlineTarget(w, target, code, originalEnd, where);
            CheckAssumptions(w, where);
            return;
        }
        // A function table (an entry loaded per wave from a table at a constant address): every distinct target of
        // its entries gets a copy, and a dispatch on the loaded target picks the copy (an unknown target skips the
        // call).
        const auto targets = TableTargets(w, call.source0.reg);
        if (targets.empty()) throw std::invalid_argument(where + ": indirect call (the target in s" + std::to_string(call.source0.reg) + " is not user data, an immediate, a program counter offset or a function table entry the request holds)");
        const auto dispatcher = static_cast<std::uint32_t>(code.size() * sizeof(std::uint32_t));
        // An else-if chain, which the structurizer takes as it is (two compares per target that both branched to the
        // next target made it duplicate the copies): per group of targets with one high word, s_cmp_eq_u32 sHi, lit;
        // s_cbranch_scc0 next group; then per target s_cmp_eq_u32 sLo, lit; s_cbranch_scc0 next target (the next group
        // after the group's last); s_getpc_b64 return pair; s_branch copy. After the last group, s_getpc_b64 return
        // pair; s_branch to the instruction after the call. The compares read the target before the return address is
        // written (the call leaves it to the dispatcher: the two pairs may be one); each s_getpc_b64 gets the call's
        // return address through its guestPcDelta. The branch targets are set after decoding.
        const auto link = 0xbe801f00u | (call.destination.reg << 16u);
        std::map<std::uint32_t, std::vector<std::size_t>> groups;
        for (std::size_t t = 0; t < targets.size(); ++t) groups[static_cast<std::uint32_t>(targets[t] >> 32u)].push_back(t);
        // A branch word and where it goes: a word of the dispatcher, or a target's copy.
        constexpr std::size_t None = static_cast<std::size_t>(-1);
        struct Fixup {
            std::size_t word;
            std::size_t label;
            std::size_t copy;
        };
        std::vector<std::uint32_t> words;
        std::vector<Fixup> fixups;
        const auto branch = [&](std::uint32_t opcode, std::size_t copy) {
            fixups.push_back({words.size(), None, copy});
            words.push_back(opcode);
            return fixups.size() - 1;
        };
        std::vector<std::size_t> toNextGroup;
        for (const auto& [high, members] : groups) {
            for (const auto f : toNextGroup) fixups[f].label = words.size();
            toNextGroup.clear();
            words.insert(words.end(), {0xbf06ff00u | (call.source0.reg + 1u), high});
            toNextGroup.push_back(branch(0xbf840000u, None));
            std::optional<std::size_t> toNextTarget;
            for (const auto t : members) {
                if (toNextTarget.has_value()) fixups[*toNextTarget].label = words.size();
                words.insert(words.end(), {0xbf06ff00u | call.source0.reg, static_cast<std::uint32_t>(targets[t])});
                toNextTarget = branch(0xbf840000u, None);
                words.push_back(link);
                branch(0xbf820000u, t);
            }
            toNextGroup.push_back(*toNextTarget);
        }
        for (const auto f : toNextGroup) fixups[f].label = words.size();
        words.push_back(link);
        const auto returnJump = branch(0xbf820000u, None);
        code.insert(code.end(), words.begin(), words.end());
        std::map<std::size_t, std::size_t> instructionAt;
        for (std::size_t word = 0; word < words.size();) {
            auto instruction = DecodeRdnaInstruction(dispatcher + static_cast<std::uint32_t>(word * sizeof(std::uint32_t)), code, static_cast<std::uint32_t>(dispatcher / sizeof(std::uint32_t) + word));
            instruction.guestPcDelta = instruction.op == RdnaOpcode::SGetpcB64 ? GuestOffset(call) - instruction.programCounter : 0;
            program.instructions.push_back(instruction);
            instructionAt[word] = program.instructions.size() - 1;
            word += instruction.wordCount;
        }
        const auto branchAt = [&](const Fixup& fixup) -> RdnaInstruction& {
            const auto found = instructionAt.find(fixup.word);
            if (found == instructionAt.end() || !IsDirectBranchOpcode(program.instructions[found->second].op)) throw std::logic_error(where + ": the function table dispatcher decoded unexpectedly");
            return program.instructions[found->second];
        };
        program.instructions[work[w].index].resolvedJump = true;
        program.instructions[work[w].index].linkInDispatcher = true;
        program.instructions[work[w].index].branchTarget = dispatcher;
        for (std::size_t f = 0; f < fixups.size(); ++f) {
            if (fixups[f].copy != None || f == returnJump) continue;
            const auto target = dispatcher + static_cast<std::uint32_t>(fixups[f].label * sizeof(std::uint32_t));
            branchAt(fixups[f]).branchTarget = target;
            branchTargets.insert(target);
        }
        std::set<std::uint32_t> written;
        for (const auto& fixup : fixups) {
            if (fixup.copy == None) continue;
            const auto copy = InlineTarget(w, targets[fixup.copy], code, originalEnd, where);
            branchAt(fixup).branchTarget = copy;
            branchTargets.insert(copy);
            const auto& inner = callWritten.at(work[w].index);
            written.insert(inner.begin(), inner.end());
        }
        branchAt(fixups[returnJump]).branchTarget = returnTo;
        branchTargets.insert(returnTo);
        callWritten[work[w].index] = std::move(written);
        CheckAssumptions(w, where);
    }

    // A register the call's target was traced to on the assumption that the call leaves it alone must not be one its
    // copies write (the call may run again, a loop around it, and see the value its callee left).
    void CheckAssumptions(std::size_t w, const std::string& where) const {
        const auto assumed = assumedUnwritten.find(work[w].index);
        if (assumed == assumedUnwritten.end()) return;
        const auto& written = callWritten.at(work[w].index);
        for (const auto reg : assumed->second) {
            if (written.contains(reg)) throw std::invalid_argument(where + ": its callee writes s" + std::to_string(reg) + ", which the call's target was traced to");
        }
    }

    // The distinct targets of the function table the call's target pair was loaded from: both registers come (through
    // moves) from one scalar load whose base this scan resolves and whose offset register it does not (the per-wave
    // entry index), so each entry of the table (the load's size apart) is a candidate. The table's words must be in the
    // request (the driver captures it); entries whose target is not code the request holds are left out.
    std::vector<std::uint64_t> TableTargets(std::size_t w, std::uint32_t lowRegister) {
        struct Locating {
            bool& flag;
            bool saved;
            ~Locating() { flag = saved; }
        } locating{locatingTable, locatingTable};
        locatingTable = true;
        const auto origin = [&](std::uint32_t reg) -> std::optional<std::pair<std::size_t, std::uint32_t>> {
            for (std::size_t j = work[w].index; j-- > work[w].regionStart;) {
                const auto& instruction = program.instructions[j];
                if (branchTargets.contains(program.instructions[j + 1].programCounter)) {
                    if (std::getenv("APS5_DEBUG_TABLE_CALLS")) std::fprintf(stderr, "[table-debug] s%u: branch target at pc 0x%x\n", reg, program.instructions[j + 1].programCounter);
                    return std::nullopt;
                }
                if (!Writes(instruction, reg)) continue;
                if (std::getenv("APS5_DEBUG_TABLE_CALLS")) std::fprintf(stderr, "[table-debug] s%u: written at pc 0x%x op %d\n", reg, instruction.programCounter, static_cast<int>(instruction.op));
                if (instruction.op == RdnaOpcode::SMovB32 && instruction.source0.kind == RdnaOperandKind::ScalarRegister) {
                    reg = instruction.source0.reg;
                    continue;
                }
                if (instruction.op == RdnaOpcode::SLoadDwordx2 || instruction.op == RdnaOpcode::SLoadDwordx4 || instruction.op == RdnaOpcode::SLoadDwordx8 || instruction.op == RdnaOpcode::SBufferLoadDwordx2 || instruction.op == RdnaOpcode::SBufferLoadDwordx4 || instruction.op == RdnaOpcode::SBufferLoadDwordx8) return std::make_pair(j, reg - instruction.destination.reg);
                return std::nullopt;
            }
            return std::nullopt;
        };
        const auto lowOrigin = origin(lowRegister);
        const auto highOrigin = origin(lowRegister + 1u);
        if (std::getenv("APS5_DEBUG_TABLE_CALLS")) std::fprintf(stderr, "[table-debug] origins %d %d\n", lowOrigin.has_value() ? static_cast<int>(lowOrigin->first) : -1, highOrigin.has_value() ? static_cast<int>(highOrigin->first) : -1);
        if (!lowOrigin.has_value() || !highOrigin.has_value() || lowOrigin->first != highOrigin->first || highOrigin->second != lowOrigin->second + 1u) return {};
        const auto& load = program.instructions[lowOrigin->first];
        if (load.source0.kind != RdnaOperandKind::ScalarRegister || load.source1.kind == RdnaOperandKind::Null) return {};
        const auto baseLow = Value(load.source0.reg, lowOrigin->first, w);
        const auto baseHigh = Value(load.source0.reg + 1u, lowOrigin->first, w);
        if (std::getenv("APS5_DEBUG_TABLE_CALLS")) std::fprintf(stderr, "[table-debug] load at pc 0x%x op %d base s%u (%d) soffset kind %d, base values %d %d\n", load.programCounter, static_cast<int>(load.op), load.source0.reg, static_cast<int>(load.source0.kind), static_cast<int>(load.source1.kind), baseLow.has_value() ? 1 : 0, baseHigh.has_value() ? 1 : 0);
        if (!baseLow.has_value() || !baseHigh.has_value()) return {};
        // An s_buffer_load reads through the V# in its base registers: its base address is the first word and the low 16 bits
        // of the second, as for an s_load base pair.
        const auto stride = static_cast<std::uint64_t>(load.op == RdnaOpcode::SLoadDwordx2 || load.op == RdnaOpcode::SBufferLoadDwordx2 ? 8u : load.op == RdnaOpcode::SLoadDwordx4 || load.op == RdnaOpcode::SBufferLoadDwordx4 ? 16u : 32u);
        const auto table = ((static_cast<std::uint64_t>(*baseHigh & 0xffffu) << 32u) | *baseLow) + static_cast<std::int64_t>(static_cast<std::int32_t>(load.memoryOffset)) + lowOrigin->second * sizeof(std::uint32_t);
        std::vector<std::uint64_t> targets;
        for (std::uint64_t entry = 0; entry < MaxTableEntries; ++entry) {
            const auto lowWord = ReadMemory(table + entry * stride);
            const auto highWord = ReadMemory(table + entry * stride + sizeof(std::uint32_t));
            if (!lowWord.has_value() || !highWord.has_value()) break;
            const auto target = static_cast<std::uint64_t>(*lowWord) | (static_cast<std::uint64_t>(*highWord) << 32u);
            if (std::find(targets.begin(), targets.end(), target) != targets.end() || !HoldsCode(target)) continue;
            targets.push_back(target);
            if (targets.size() > MaxTableTargets) throw std::invalid_argument("function table at " + Hex(table) + " has more than " + std::to_string(MaxTableTargets) + " distinct targets");
        }
        return targets;
    }

    bool HoldsCode(std::uint64_t target) const {
        if (target == 0 || target % sizeof(std::uint32_t) != 0) return false;
        return std::any_of(memory.begin(), memory.end(), [&](const MemoryRegion& region) { return target >= region.guestAddress && target - region.guestAddress + sizeof(std::uint32_t) <= region.bytes.size(); });
    }

    static constexpr std::uint64_t MaxTableEntries = 256;
    static constexpr std::size_t MaxTableTargets = 16;

    // Inlines `target` as work[w]'s callee after the code so far and returns where the copy starts; the calls inside the
    // copy are inlined right after it. Sets callWritten for work[w] to what the copy and its calls write.
    std::uint32_t InlineTarget(std::size_t w, std::uint64_t target, std::vector<std::uint32_t>& code, std::size_t originalEnd, const std::string& where) {
        std::vector<std::size_t> nested;
        const auto call = program.instructions[work[w].index];
        if (std::find(work[w].chain.begin(), work[w].chain.end(), target) != work[w].chain.end()) throw std::invalid_argument(where + ": recursive call of " + Hex(target));
        const auto& callee = Decode(target, call.destination.reg, where);
        // The copy follows everything before it, word-aligned like the caller's code.
        const auto base = static_cast<std::uint32_t>(code.size() * sizeof(std::uint32_t));
        // APS5_TRACE_CALL_TABLES=1: each copy, with its depth and size.
        static const bool trace = std::getenv("APS5_TRACE_CALL_TABLES") != nullptr;
        if (trace) std::fprintf(stderr, "[subroutine] 0x%llx: copy of 0x%llx at depth %u, %u words (code now %zu words, %zu instructions)\n", static_cast<unsigned long long>(codeAddress), static_cast<unsigned long long>(target), work[w].depth, callee.endProgramCounter / static_cast<std::uint32_t>(sizeof(std::uint32_t)), code.size(), program.instructions.size());
        if (code.size() + callee.endProgramCounter / sizeof(std::uint32_t) > MaxInlinedWords) throw std::invalid_argument(where + ": the inlined callees exceed " + std::to_string(MaxInlinedWords) + " words (copy of " + Hex(target) + " at depth " + std::to_string(work[w].depth) + ")");
        code.insert(code.end(), callee.words.begin(), callee.words.begin() + callee.endProgramCounter / sizeof(std::uint32_t));
        const auto returnTo = call.programCounter + call.wordCount * static_cast<std::uint32_t>(sizeof(std::uint32_t));
        const auto delta = (target - codeAddress) - base;
        const auto copyStart = program.instructions.size();
        auto chain = work[w].chain;
        chain.push_back(target);
        for (auto instruction : callee.instructions) {
            instruction.programCounter += base;
            if (IsDirectBranchOpcode(instruction.op)) {
                instruction.branchTarget += base;
                branchTargets.insert(instruction.branchTarget);
            }
            instruction.guestPcDelta = delta;
            if (IsNullSwap(instruction)) {
                instruction.op = RdnaOpcode::SSetpcB64;
                instruction.destination = RdnaOperand{};
            }
            if (instruction.op == RdnaOpcode::SSetpcB64) {
                instruction.resolvedJump = true;
                instruction.branchTarget = returnTo;
            }
            program.instructions.push_back(instruction);
            if (instruction.op == RdnaOpcode::SSwappcB64) {
                nested.push_back(work.size());
                work.push_back(Call{program.instructions.size() - 1, work[w].depth + 1, w, chain, copyStart});
            }
        }
        // A callee inside the caller's code (reached by a program counter offset) is also decoded in place, after
        // the caller's end: its returns there are unreachable, but the graph needs a target for them.
        if (work[w].parent == NoParent && target >= codeAddress && target - codeAddress < originalEnd) {
            const auto first = static_cast<std::uint32_t>(target - codeAddress);
            for (std::size_t i = 0; i < copyStart; ++i) {
                auto& original = program.instructions[i];
                if (original.programCounter < first || original.programCounter >= first + callee.endProgramCounter) continue;
                if (original.op == RdnaOpcode::SSetpcB64 && !original.resolvedJump) {
                    original.resolvedJump = true;
                    original.branchTarget = returnTo;
                }
            }
        }
        auto written = callee.written;
        for (const auto n : nested) {
            Inline(n, code, originalEnd);
            const auto& inner = callWritten.at(work[n].index);
            written.insert(inner.begin(), inner.end());
        }
        callWritten[work[w].index] = std::move(written);
        return base;
    }

    static constexpr std::size_t NoParent = static_cast<std::size_t>(-1);

    // A call to inline: its instruction, how deep it sits, the call whose copy holds it (with that copy's first
    // instruction) and the targets of the calls around it, for the recursion check.
    struct Call {
        std::size_t index;
        std::uint32_t depth;
        std::size_t parent;
        std::vector<std::uint64_t> chain;
        std::size_t regionStart = 0;
    };

    // The instruction's offset from the program's code address as the guest sees it (a copy's own offset is not).
    std::uint64_t GuestOffset(const RdnaInstruction& instruction) const {
        return instruction.programCounter + instruction.guestPcDelta;
    }

    // What the scalar register holds before program.instructions[before], if a constant: user data at the program's
    // start, moves, immediates, s_getpc_b64 and the s_add_u32/s_addc_u32 pair that offsets it. The scan stays in
    // straight-line code (no branch target in between; past one only user data, see Unwritten) and steps over an
    // earlier call whose callee leaves the register alone. In a callee's copy it continues before the call that
    // entered the copy.
    std::optional<std::uint32_t> Value(std::uint32_t reg, std::size_t before, std::size_t call, int depth = 0) {
        if (depth > 16) return std::nullopt;
        const auto regionStart = work[call].regionStart;
        for (std::size_t j = before; j-- > regionStart;) {
            const auto& instruction = program.instructions[j];
            if (branchTargets.contains(program.instructions[j + 1].programCounter)) return Unwritten(reg, j + 1, call);
            if (instruction.op == RdnaOpcode::SSwappcB64) {
                if (instruction.destination.kind == RdnaOperandKind::ScalarRegister && (reg == instruction.destination.reg || reg == instruction.destination.reg + 1u)) {
                    const auto returnAddress = codeAddress + GuestOffset(instruction) + instruction.wordCount * sizeof(std::uint32_t);
                    return static_cast<std::uint32_t>(reg == instruction.destination.reg ? returnAddress : returnAddress >> 32u);
                }
                const auto found = callWritten.find(j);
                if (found == callWritten.end()) return std::nullopt;
                if (found->second.contains(reg)) return std::nullopt;
                continue;
            }
            if (!Writes(instruction, reg)) continue;
            const auto& destination = instruction.destination;
            switch (instruction.op) {
                case RdnaOpcode::SMovB32:
                    return Operand(instruction.source0, j, call, depth);
                case RdnaOpcode::SMovB64:
                    if (instruction.source0.kind == RdnaOperandKind::ScalarRegister) return Value(instruction.source0.reg + (reg - destination.reg), j, call, depth + 1);
                    if (reg == destination.reg) return Operand(instruction.source0, j, call, depth);
                    return std::nullopt;
                case RdnaOpcode::SGetpcB64: {
                    const auto pc = codeAddress + GuestOffset(instruction) + instruction.wordCount * sizeof(std::uint32_t);
                    return static_cast<std::uint32_t>(reg == destination.reg ? pc : pc >> 32u);
                }
                case RdnaOpcode::SAddU32: {
                    const auto a = Operand(instruction.source0, j, call, depth);
                    const auto b = Operand(instruction.source1, j, call, depth);
                    if (!a.has_value() || !b.has_value()) return std::nullopt;
                    return *a + *b;
                }
                case RdnaOpcode::SAddcU32: {
                    // The carry of the s_add_u32 right before it.
                    if (j == regionStart || program.instructions[j - 1].op != RdnaOpcode::SAddU32) return std::nullopt;
                    const auto& add = program.instructions[j - 1];
                    const auto a = Operand(instruction.source0, j, call, depth);
                    const auto b = Operand(instruction.source1, j, call, depth);
                    const auto addA = Operand(add.source0, j - 1, call, depth);
                    const auto addB = Operand(add.source1, j - 1, call, depth);
                    if (!a.has_value() || !b.has_value() || !addA.has_value() || !addB.has_value()) return std::nullopt;
                    const auto carry = static_cast<std::uint32_t>((static_cast<std::uint64_t>(*addA) + *addB) >> 32u);
                    return *a + *b + carry;
                }
                case RdnaOpcode::SLoadDword:
                case RdnaOpcode::SLoadDwordx2:
                case RdnaOpcode::SLoadDwordx4:
                case RdnaOpcode::SLoadDwordx8: {
                    // A scalar load at a constant address (a base pair this scan resolves, an immediate offset and
                    // no offset register) from memory the request holds: a function table's pointer.
                    if (std::getenv("APS5_DEBUG_TABLE_CALLS")) std::fprintf(stderr, "[table-debug] value s%u: load at pc 0x%x base kind %d s%u soffset kind %d\n", reg, instruction.programCounter, static_cast<int>(instruction.source0.kind), instruction.source0.reg, static_cast<int>(instruction.source1.kind));
                    if (instruction.source0.kind != RdnaOperandKind::ScalarRegister || instruction.source1.kind != RdnaOperandKind::Null) return std::nullopt;
                    const auto low = Value(instruction.source0.reg, j, call, depth + 1);
                    const auto high = Value(instruction.source0.reg + 1u, j, call, depth + 1);
                    if (std::getenv("APS5_DEBUG_TABLE_CALLS")) std::fprintf(stderr, "[table-debug] value s%u: base %d %d\n", reg, low.has_value() ? 1 : 0, high.has_value() ? 1 : 0);
                    if (!low.has_value() || !high.has_value()) return std::nullopt;
                    const auto base = (static_cast<std::uint64_t>(*high & 0xffffu) << 32u) | *low;
                    return ReadMemory((base + static_cast<std::int64_t>(static_cast<std::int32_t>(instruction.memoryOffset)) + (reg - destination.reg) * sizeof(std::uint32_t)) & ~std::uint64_t{3});
                }
                default:
                    return std::nullopt;
            }
        }
        // The start of a copy: the register as it was at the call that entered it (the call itself only writes its
        // return address, handled by the caller's scan).
        if (work[call].parent != NoParent) {
            const auto& parent = work[work[call].parent];
            const auto& entry = program.instructions[parent.index];
            if (entry.destination.kind == RdnaOperandKind::ScalarRegister && (reg == entry.destination.reg || reg == entry.destination.reg + 1u)) {
                const auto returnAddress = codeAddress + GuestOffset(entry) + entry.wordCount * sizeof(std::uint32_t);
                return static_cast<std::uint32_t>(reg == entry.destination.reg ? returnAddress : returnAddress >> 32u);
            }
            return Value(reg, parent.index, work[call].parent, depth + 1);
        }
        if (reg >= userDataBase && reg - userDataBase < userData.size()) return readUserData(reg - userDataBase);
        return std::nullopt;
    }

    // User data in the main program, past control flow: the register still holds it at program.instructions[at] when
    // nothing before that point writes it (calls by what their copies write) and no branch from there on jumps back
    // before it, so later writes cannot reach it either.
    std::optional<std::uint32_t> Unwritten(std::uint32_t reg, std::size_t at, std::size_t call) {
        if (work[call].regionStart != 0 || reg < userDataBase || reg - userDataBase >= userData.size()) return std::nullopt;
        // A register nothing in the main program writes (nor any call made from it, nor a call's return pair) holds its
        // user data everywhere, whatever the control flow (a waterfall loop around a function table call).
        const bool neverWritten = std::none_of(program.instructions.begin(), program.instructions.begin() + static_cast<std::ptrdiff_t>(mainCount), [&](const RdnaInstruction& instruction) {
            if (Writes(instruction, reg)) return true;
            if (instruction.op == RdnaOpcode::SSetpcB64 && !instruction.resolvedJump) return true;
            if (instruction.op != RdnaOpcode::SSwappcB64) return false;
            if (instruction.destination.kind == RdnaOperandKind::ScalarRegister && (reg == instruction.destination.reg || reg == instruction.destination.reg + 1u)) return true;
            const auto index = static_cast<std::size_t>(&instruction - program.instructions.data());
            // The call being resolved has no copies yet: assumed to leave the register alone, which Inline checks
            // against what its copies write once they exist.
            if (index == work[call].index) {
                assumedUnwritten[index].insert(reg);
                return false;
            }
            const auto found = callWritten.find(index);
            return found == callWritten.end() || found->second.contains(reg);
        });
        if (neverWritten) return readUserData(reg - userDataBase);
        const auto pc = program.instructions[at].programCounter;
        for (std::size_t i = 0; i < mainCount; ++i) {
            const auto& instruction = program.instructions[i];
            if (IsDirectBranchOpcode(instruction.op) && instruction.programCounter >= pc && instruction.branchTarget <= pc) return std::nullopt;
            if (instruction.op == RdnaOpcode::SSetpcB64 && !instruction.resolvedJump) return std::nullopt;
            if (i >= at) continue;
            if (Writes(instruction, reg)) return std::nullopt;
            if ((instruction.op == RdnaOpcode::SSwappcB64 || instruction.op == RdnaOpcode::SGetpcB64) && instruction.destination.kind == RdnaOperandKind::ScalarRegister && reg == instruction.destination.reg + 1u) return std::nullopt;
            if (instruction.op == RdnaOpcode::SSwappcB64) {
                const auto found = callWritten.find(i);
                if (found == callWritten.end() || found->second.contains(reg)) return std::nullopt;
            }
        }
        return readUserData(reg - userDataBase);
    }

    // Every user data word a call target was traced to: the inlined program depends on these values only.
    std::uint32_t readUserData(std::uint32_t index) {
        (locatingTable ? tableUserData : usedUserData).insert(index);
        return userData[index];
    }

    // A word of the request's memory (a function table or its pointer); every word read is recorded, since the inlined
    // program depends on it as on the user data its targets come from.
    std::optional<std::uint32_t> ReadMemory(std::uint64_t address) {
        for (const auto& region : memory) {
            if (address < region.guestAddress || address - region.guestAddress + sizeof(std::uint32_t) > region.bytes.size()) continue;
            std::uint32_t word = 0;
            std::memcpy(&word, region.bytes.data() + (address - region.guestAddress), sizeof(word));
            (locatingTable ? tableMemory : usedMemory).insert(address);
            return word;
        }
        return std::nullopt;
    }

public:
    std::set<std::uint32_t> usedUserData;
    std::set<std::uint64_t> usedMemory;
    // Inputs read only to locate a function table (its pointer, V# and entries): the dispatcher compares the loaded
    // target at run time, so the program does not depend on where the table is and they stay out of the key.
    std::set<std::uint32_t> tableUserData;
    std::set<std::uint64_t> tableMemory;
    bool locatingTable = false;

private:

    std::optional<std::uint32_t> Operand(const RdnaOperand& operand, std::size_t before, std::size_t call, int depth) {
        switch (operand.kind) {
            case RdnaOperandKind::ScalarRegister: return Value(operand.reg, before, call, depth + 1);
            case RdnaOperandKind::LiteralConstant:
            case RdnaOperandKind::IntegerInlineConstant: return operand.value;
            default: return std::nullopt;
        }
    }

    // The callee at `target`: its reachable code from the request's memory, up to its returns (s_setpc_b64 of the
    // return register pair, or a null-destination s_swappc_b64 of it) and s_endpgm. A call inside it continues after the
    // call (the callee it reaches is inlined separately). Only a callee that calls further may write its return pair
    // (the nested call's target and return address go there, and the saved pair is put back); its own returns are
    // resolved statically either way.
    const Callee& Decode(std::uint64_t target, std::uint32_t returnRegister, const std::string& where) {
        const auto key = std::make_pair(target, returnRegister);
        if (const auto found = decoded.find(key); found != decoded.end()) return found->second;
        if (target % sizeof(std::uint32_t) != 0) throw std::invalid_argument(where + ": callee " + Hex(target) + " is not word-aligned");
        const MemoryRegion* region = nullptr;
        for (const auto& candidate : memory) {
            if (target >= candidate.guestAddress && target - candidate.guestAddress + sizeof(std::uint32_t) <= candidate.bytes.size()) region = &candidate;
        }
        if (region == nullptr) throw std::invalid_argument(where + ": the code of callee " + Hex(target) + " is not in the request (the driver adds registered shader code that user data points at)");
        Callee callee;
        callee.address = target;
        const auto offset = static_cast<std::size_t>(target - region->guestAddress);
        callee.words.resize((region->bytes.size() - offset) / sizeof(std::uint32_t));
        std::memcpy(callee.words.data(), region->bytes.data() + offset, callee.words.size() * sizeof(std::uint32_t));
        std::map<std::uint32_t, RdnaInstruction> reached;
        std::vector<std::uint32_t> pending{0};
        bool returns = false;
        std::optional<std::uint32_t> returnWrite;
        while (!pending.empty()) {
            const auto pc = pending.back();
            pending.pop_back();
            if (reached.contains(pc)) continue;
            const auto wordIndex = pc / static_cast<std::uint32_t>(sizeof(std::uint32_t));
            if (wordIndex >= callee.words.size()) throw std::invalid_argument(where + ": callee " + Hex(target) + " runs past its code region");
            const auto instruction = DecodeRdnaInstruction(pc, callee.words, wordIndex);
            reached.emplace(pc, instruction);
            if (reached.size() > 65536) throw std::invalid_argument(where + ": callee " + Hex(target) + " is too large");
            const auto next = pc + instruction.wordCount * static_cast<std::uint32_t>(sizeof(std::uint32_t));
            callee.endProgramCounter = std::max(callee.endProgramCounter, next);
            if (IsNullSwap(instruction)) {
                if (instruction.source0.kind != RdnaOperandKind::ScalarRegister || instruction.source0.reg != returnRegister) throw std::invalid_argument(where + ": callee " + Hex(target) + " has a dynamic jump at its offset " + Hex(pc));
                returns = true;
                continue;
            }
            if (instruction.op == RdnaOpcode::SSwappcB64) callee.makesCalls = true;
            if (!returnWrite.has_value() && (Writes(instruction, returnRegister) || Writes(instruction, returnRegister + 1u))) returnWrite = pc;
            for (const auto reg : WrittenScalars(instruction)) {
                if (reg != returnRegister && reg != returnRegister + 1u) callee.written.insert(reg);
            }
            if (instruction.op == RdnaOpcode::SSetpcB64) {
                if (instruction.source0.kind != RdnaOperandKind::ScalarRegister || instruction.source0.reg != returnRegister) throw std::invalid_argument(where + ": callee " + Hex(target) + " has a dynamic jump at its offset " + Hex(pc));
                returns = true;
                continue;
            }
            if (instruction.op == RdnaOpcode::SEndpgm) continue;
            if (IsDirectBranchOpcode(instruction.op)) {
                pending.push_back(instruction.branchTarget);
                if (instruction.op == RdnaOpcode::SBranch) continue;
            }
            pending.push_back(next);
        }
        if (!returns) throw std::invalid_argument(where + ": callee " + Hex(target) + " never returns through s" + std::to_string(returnRegister));
        if (returnWrite.has_value() && !callee.makesCalls) throw std::invalid_argument(where + ": callee " + Hex(target) + " overwrites its return address in s" + std::to_string(returnRegister) + " at its offset " + Hex(*returnWrite));
        for (auto& [pc, instruction] : reached) callee.instructions.push_back(instruction);
        return decoded.emplace(key, std::move(callee)).first->second;
    }

    RdnaProgram& program;
    std::uint64_t codeAddress;
    std::span<const std::uint32_t> userData;
    std::uint32_t userDataBase;
    std::span<const MemoryRegion> memory;
    std::set<std::uint32_t> branchTargets;
    std::map<std::pair<std::uint64_t, std::uint32_t>, Callee> decoded;
    std::map<std::size_t, const Callee*> callees;
    std::vector<Call> work;
    // Per inlined call: the scalar registers its callee and the calls inside it write (return pairs aside).
    std::map<std::size_t, std::set<std::uint32_t>> callWritten;
    // Per call: registers its target was traced to on the assumption that the call itself leaves them alone.
    std::map<std::size_t, std::set<std::uint32_t>> assumedUnwritten;
    // The decoded program's own instructions (the copies follow them).
    std::size_t mainCount = 0;
};

}

bool SubroutineCallsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_SUBROUTINE_CALLS");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

namespace {

struct CallCodeKey {
    std::uint64_t address;
    std::size_t words;
    std::uint64_t hash;
    auto operator<=>(const CallCodeKey&) const = default;
};

CallCodeKey callCodeKey(std::uint64_t codeAddress, std::span<const std::uint32_t> code) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto word : code) hash = (hash ^ word) * 0x100000001b3ull;
    return {codeAddress, code.size(), hash};
}

struct CallUserDataRegistry {
    std::mutex mutex;
    struct Inputs {
        std::set<std::uint32_t> userData;
        std::set<std::uint64_t> memory;
    };
    std::map<CallCodeKey, Inputs> indices;
};

CallUserDataRegistry& callUserDataRegistry() {
    static CallUserDataRegistry registry;
    return registry;
}

}

void InlineSubroutineCalls(RdnaProgram& program, std::uint64_t codeAddress, std::span<const std::uint32_t> userData, std::uint32_t userDataBaseRegister, std::span<const MemoryRegion> memory, std::uint32_t waveSize) {
    struct LaneMaskScope {
        std::uint32_t saved = laneMaskRegisters;
        ~LaneMaskScope() { laneMaskRegisters = saved; }
    } laneMaskScope;
    laneMaskRegisters = waveSize == 32u ? 1u : 2u;
    // Keyed by the caller's own code: once callees are inlined, program.code views the caller and their copies.
    const auto key = callCodeKey(codeAddress, program.code);
    Inliner inliner(program, codeAddress, userData, userDataBaseRegister, memory);
    inliner.Run();
    // Recorded after a successful inlining only: a failed one keeps the code keyed by all its user data.
    auto& registry = callUserDataRegistry();
    std::lock_guard lock(registry.mutex);
    auto& inputs = registry.indices[key];
    inputs.userData.insert(inliner.usedUserData.begin(), inliner.usedUserData.end());
    inputs.memory.insert(inliner.usedMemory.begin(), inliner.usedMemory.end());
}

bool CallUserData(std::uint64_t codeAddress, std::span<const std::uint32_t> code, std::vector<std::uint32_t>& indices, std::vector<std::uint64_t>& addresses) {
    static const bool all = [] {
        const char* value = std::getenv("APS5_CALL_KEY_ALL_USER_DATA");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    if (all) return false;
    auto& registry = callUserDataRegistry();
    std::lock_guard lock(registry.mutex);
    const auto found = registry.indices.find(callCodeKey(codeAddress, code));
    if (found == registry.indices.end()) return false;
    indices.assign(found->second.userData.begin(), found->second.userData.end());
    addresses.assign(found->second.memory.begin(), found->second.memory.end());
    return true;
}

}
