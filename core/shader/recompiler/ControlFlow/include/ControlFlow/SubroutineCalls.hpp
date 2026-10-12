#ifndef CORE_SHADER_RECOMPILIER_CONTROLFLOW_INCLUDE_CONTROLFLOW_SUBROUTINECALLS_HPP
#define CORE_SHADER_RECOMPILIER_CONTROLFLOW_INCLUDE_CONTROLFLOW_SUBROUTINECALLS_HPP

#include "RdnaDecoder/RdnaProgram.hpp"
#include "Recompiler.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace ShaderRecompiler {

// Subroutine calls (s_swappc_b64, SOP1 opcode 33): each call site gets its own copy of the callee, appended after the
// caller's code, so the control flow graph sees a jump to the copy and a jump back from the copy's return
// (s_setpc_b64 on the call's return register) to the instruction after the call.
//
// The callee address is what the call's source registers hold at the call: user data (a function pointer the game
// passes, the Wolverine form), immediates, or the caller's own code (s_getpc_b64 + s_add_u32/s_addc_u32 of a
// constant), followed back through scalar moves in the caller. Its code comes from the request's memory regions (the
// driver adds the registered shader code that user data points at). Unsupported, with an error naming the call: a
// target that is not such a constant, code that is not in the request, recursion and nested calls, a dynamic jump
// inside the callee, and a callee that writes its return address registers.
//
// Function tables (a target pair loaded per wave from a table the request holds): a dispatcher compares the loaded target
// with each entry the table had at compile time and jumps to that entry's copy; another target skips the call.
// Switch: APS5_SUBROUTINE_CALLS=1, compute programs only (off: the control flow graph's own call model, which refuses
// targets it cannot resolve statically).
bool SubroutineCallsEnabled();

// `codeAddress` is the caller's guest address (RecompileRequest::shader.codeAddress); a program without calls is left
// as it is.
void InlineSubroutineCalls(RdnaProgram& program, std::uint64_t codeAddress, std::span<const std::uint32_t> userData, std::uint32_t userDataBaseRegister, std::span<const MemoryRegion> memory, std::uint32_t waveSize = 64);

// The user data words the call targets of `code` were traced to, over every inlining of it so far (sorted): which
// registers the targets come from is a property of the code and of the callees, and the callees are fixed by those
// words' values. A cache key of a program with calls holds these words instead of all its user data, so a word the
// calls never read (a per-dispatch ring pointer) does not make every dispatch a new program. False until the code
// was inlined once (the key then holds all user data). `addresses` are the request memory words the targets were read
// from (a function table and its pointer); the key holds their values too. APS5_CALL_KEY_ALL_USER_DATA=1 keys all user
// data always.
bool CallUserData(std::uint64_t codeAddress, std::span<const std::uint32_t> code, std::vector<std::uint32_t>& indices, std::vector<std::uint64_t>& addresses);

}

#endif
