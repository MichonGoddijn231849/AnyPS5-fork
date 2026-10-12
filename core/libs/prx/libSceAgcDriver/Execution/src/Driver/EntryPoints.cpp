#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/ShaderPreparation.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include <atomic>
#include <cstdio>
#include <exception>
#include <mutex>

namespace AgcDriver {

void Submit(const Packet* packet, std::uint32_t queue) try {
    DriverDetail::Driver::Get().Submit(packet, queue);
} catch (const ProcessShutdown&) {
    LibcAwaitExit_nid_postfix();
}

void WaitIdle() {
    DriverDetail::Driver::Get().WaitIdle();
}

void Shutdown() {
    DriverDetail::Driver::Get().Shutdown();
}

void RegisterShader(const Shader* shader) {
    const bool profile = DriverDetail::PreparationProfile::Enabled();
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    DriverDetail::Driver::Get().RegisterShader(shader);
    if (profile) DriverDetail::PreparationProfile::Add(DriverDetail::PreparationProfile::Phase::Registration, start);
}

void SuspendPoint() {
    DriverDetail::Driver::Get().SuspendPoint();
}

void RegisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    DriverDetail::Driver::Get().RegisterVideoOutput(handle, output);
}

void UnregisterVideoOutput(std::uint32_t handle, const std::shared_ptr<IVideoOutput>& output) {
    DriverDetail::Driver::Get().UnregisterVideoOutput(handle, output);
}

void AttachWindow(const PresentationWindow& window) {
    DriverDetail::Driver::Get().AttachWindow(window);
}

void PresentClear(const PresentationWindow& window, bool opaque, void (*gpuReady)(void*), void* context) {
    DriverDetail::Driver::Get().Present(window, nullptr, opaque, gpuReady, context);
}

void PresentBuffer(const PresentationWindow& window, const DisplayBuffer& buffer, void (*gpuReady)(void*), void* context) {
    DriverDetail::Driver::Get().Present(window, &buffer, true, gpuReady, context);
}

void ReleaseWindow(void* window) {
    DriverDetail::Driver::Get().ReleaseWindow(window);
}

void ReportFailure(std::exception_ptr error) {
    DriverDetail::Driver::Get().ReportFailure(error);
}

}

extern "C" void AgcDriverWaitIdle_nid_postfix() try {
    AgcDriver::WaitIdle();
} catch (const ProcessShutdown&) {
    LibcAwaitExit_nid_postfix();
}

static std::mutex& VulkanLoaderMutex() {
    static std::mutex mutex;
    return mutex;
}

extern "C" void AgcDriverLockVulkanLoader_nid_postfix() {
    VulkanLoaderMutex().lock();
}

extern "C" void AgcDriverUnlockVulkanLoader_nid_postfix() {
    VulkanLoaderMutex().unlock();
}

extern "C" void AgcDriverShutdown_nid_postfix() {
    AgcDriver::Shutdown();
}

extern "C" void AgcDriverRegisterShader_nid_postfix(const Shader* shader) try {
    AgcDriver::RegisterShader(shader);
} catch (const ProcessShutdown&) {
    LibcAwaitExit_nid_postfix();
}

extern "C" void AgcDriverSuspendPoint_nid_postfix() try {
    AgcDriver::SuspendPoint();
} catch (const ProcessShutdown&) {
    LibcAwaitExit_nid_postfix();
}

extern "C" void AgcDriverRegisterVideoOutput_nid_postfix(std::uint32_t handle, const std::shared_ptr<AgcDriver::IVideoOutput>& output) {
    AgcDriver::RegisterVideoOutput(handle, output);
}

extern "C" void AgcDriverUnregisterVideoOutput_nid_postfix(std::uint32_t handle, const std::shared_ptr<AgcDriver::IVideoOutput>& output) {
    AgcDriver::UnregisterVideoOutput(handle, output);
}

extern "C" void AgcDriverAttachWindow_nid_postfix(const AgcDriver::PresentationWindow& window) {
    AgcDriver::AttachWindow(window);
}

extern "C" void AgcDriverPresentClear_nid_postfix(const AgcDriver::PresentationWindow& window, bool opaque, void (*gpuReady)(void*), void* context) {
    AgcDriver::PresentClear(window, opaque, gpuReady, context);
}

extern "C" void AgcDriverPresentBuffer_nid_postfix(const AgcDriver::PresentationWindow& window, const AgcDriver::DisplayBuffer& buffer, void (*gpuReady)(void*), void* context) {
    AgcDriver::PresentBuffer(window, buffer, gpuReady, context);
}

extern "C" void AgcDriverReleaseWindow_nid_postfix(void* window) {
    AgcDriver::ReleaseWindow(window);
}

extern "C" void AgcDriverReportFailure_nid_postfix(std::exception_ptr error) {
    AgcDriver::ReportFailure(error);
}

namespace {

// The ABI hints prepare shaders ahead of their draws. GTA V links stages whose headers or primitive state do not
// match yet; a hint that fails is reported and dropped, and the draw checks its own state when it runs.
template<typename TWork>
void ResolveHint(const char* what, TWork&& work) {
    using AgcDriver::DriverDetail::PreparationProfile::Phase;
    const bool profile = AgcDriver::DriverDetail::PreparationProfile::Enabled();
    const auto start = profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    try {
        work();
        if (profile) AgcDriver::DriverDetail::PreparationProfile::Add(Phase::AbiHint, start);
    } catch (const std::exception& error) {
        if (profile) AgcDriver::DriverDetail::PreparationProfile::Add(Phase::AbiHint, start, 0, true);
        static std::atomic<std::uint32_t> reported{0};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 16) std::fprintf(stderr, "[shader-registry] %s hint dropped: %s\n", what, error.what());
        AgcDriver::DriverDetail::ShaderPreparationTransaction::ForgiveFailure();
    }
}

}

extern "C" void AgcDriverResolveShaderAbi_nid_postfix(const Shader* shader, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) {
    ResolveHint("shader ABI", [&] { AgcDriver::DriverDetail::Driver::Get().ResolveShaderAbi(shader, context, primitive); });
}

extern "C" void AgcDriverResolveGraphicsAbi_nid_postfix(const Shader* vertex, const Shader* pixel, std::uint32_t primitiveType) {
    ResolveHint("graphics ABI", [&] { AgcDriver::DriverDetail::Driver::Get().ResolveGraphicsAbi(vertex, pixel, primitiveType); });
}
extern "C" void AgcDriverResolveGraphicsStagesAbi_nid_postfix(std::span<const Shader* const> stages, std::span<const ShaderRegister> context, std::span<const ShaderRegister> primitive) {
    ResolveHint("graphics stages ABI", [&] { AgcDriver::DriverDetail::Driver::Get().ResolveGraphicsStagesAbi(stages, context, primitive); });
}
