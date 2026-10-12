#ifndef CORE_SHADER_RECOMPILER_DEEPSTACK_HPP
#define CORE_SHADER_RECOMPILER_DEEPSTACK_HPP

#include <functional>

namespace ShaderRecompiler {

// Runs work on the calling thread when its stack has room for a compile, else on a helper thread with a large stack,
// waiting for it and rethrowing what it threw. Shaders are prepared on the game's threads, and games run jobs on
// fibers whose stacks they size themselves (Wolverine: 128 KiB), which the recompiler's recursive passes overflow.
void RunWithDeepStack(const std::function<void()>& work);

}

#endif
