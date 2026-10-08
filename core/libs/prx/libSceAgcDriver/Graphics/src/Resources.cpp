#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Recorder.hpp"
#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/NullSubmit.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace AgcDriver::Graphics {

bool NullSubmit() {
    static const bool null = std::getenv("APS5_NULL_SUBMIT") != nullptr;
    return null;
}

namespace {

// The stub pipeline: `void main() {}` for GLCompute, local size 1.
constexpr std::array<std::uint32_t, 35> NullPipelineCode{
    0x07230203u, 0x00010300u, 0x00070000u, 0x00000005u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0003000eu, 0x00000000u,
    0x00000001u, 0x0005000fu, 0x00000005u, 0x00000001u, 0x6e69616du, 0x00000000u, 0x00060010u, 0x00000001u, 0x00000011u,
    0x00000001u, 0x00000001u, 0x00000001u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00050036u,
    0x00000002u, 0x00000001u, 0x00000000u, 0x00000003u, 0x000200f8u, 0x00000004u, 0x000100fdu, 0x00010038u,
};

struct NullPipelineObjects {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

std::mutex& nullPipelineMutex() {
    static std::mutex mutex;
    return mutex;
}

std::unordered_map<VkDevice, NullPipelineObjects>& nullPipelines() {
    static std::unordered_map<VkDevice, NullPipelineObjects> pipelines;
    return pipelines;
}

}

VkPipeline NullPipeline(const Context& context) {
    std::lock_guard lock(nullPipelineMutex());
    auto& objects = nullPipelines()[context.device];
    if (objects.pipeline != VK_NULL_HANDLE) return objects.pipeline;
    VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    moduleInfo.codeSize = NullPipelineCode.size() * sizeof(std::uint32_t);
    moduleInfo.pCode = NullPipelineCode.data();
    VkShaderModule module = VK_NULL_HANDLE;
    Check(context.Function<PFN_vkCreateShaderModule>("vkCreateShaderModule")(context.device, &moduleInfo, nullptr, &module), "vkCreateShaderModule null pipeline");
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    Check(context.Function<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout")(context.device, &layoutInfo, nullptr, &objects.layout), "vkCreatePipelineLayout null pipeline");
    VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pipelineInfo.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pipelineInfo.stage.module = module;
    pipelineInfo.stage.pName = "main";
    pipelineInfo.layout = objects.layout;
    const auto result = context.Function<PFN_vkCreateComputePipelines>("vkCreateComputePipelines")(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &objects.pipeline);
    context.Function<PFN_vkDestroyShaderModule>("vkDestroyShaderModule")(context.device, module, nullptr);
    Check(result, "vkCreateComputePipelines null pipeline");
    return objects.pipeline;
}

void ReleaseNullPipeline(const Context& context) {
    std::lock_guard lock(nullPipelineMutex());
    const auto found = nullPipelines().find(context.device);
    if (found == nullPipelines().end()) return;
    if (found->second.pipeline != VK_NULL_HANDLE) context.Function<PFN_vkDestroyPipeline>("vkDestroyPipeline")(context.device, found->second.pipeline, nullptr);
    if (found->second.layout != VK_NULL_HANDLE) context.Function<PFN_vkDestroyPipelineLayout>("vkDestroyPipelineLayout")(context.device, found->second.layout, nullptr);
    nullPipelines().erase(found);
}

namespace {

std::atomic<PFN_vkGetDeviceProcAddr> realDeviceProc{nullptr};
std::atomic<PFN_vkCreateImage> realCreateImage{nullptr};
std::atomic<PFN_vkQueueSubmit> realQueueSubmit{nullptr};

VKAPI_ATTR VkResult VKAPI_CALL nullCreateImage(VkDevice device, const VkImageCreateInfo* info, const VkAllocationCallbacks* allocator, VkImage* image) {
    auto patched = *info;
    if (patched.samples == VK_SAMPLE_COUNT_2_BIT || patched.samples == VK_SAMPLE_COUNT_8_BIT || patched.samples == VK_SAMPLE_COUNT_16_BIT) patched.samples = VK_SAMPLE_COUNT_4_BIT;
    return realCreateImage.load(std::memory_order_relaxed)(device, &patched, allocator, image);
}

VKAPI_ATTR VkResult VKAPI_CALL nullQueueSubmit(VkQueue queue, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    std::vector<VkSubmitInfo> empty(submits, submits + count);
    for (auto& info : empty) {
        info.commandBufferCount = 0;
        info.pCommandBuffers = nullptr;
    }
    return realQueueSubmit.load(std::memory_order_relaxed)(queue, count, empty.data(), fence);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL nullDeviceProc(VkDevice device, const char* name) {
    const auto real = realDeviceProc.load(std::memory_order_relaxed);
    const auto function = real(device, name);
    if (function == nullptr) return function;
    if (std::strcmp(name, "vkCreateImage") == 0) {
        realCreateImage.store(reinterpret_cast<PFN_vkCreateImage>(function), std::memory_order_relaxed);
        return reinterpret_cast<PFN_vkVoidFunction>(&nullCreateImage);
    }
    if (std::strcmp(name, "vkQueueSubmit") == 0) {
        realQueueSubmit.store(reinterpret_cast<PFN_vkQueueSubmit>(function), std::memory_order_relaxed);
        return reinterpret_cast<PFN_vkVoidFunction>(&nullQueueSubmit);
    }
    return function;
}

}

PFN_vkGetDeviceProcAddr NullDeviceProc(PFN_vkGetDeviceProcAddr real) {
    PFN_vkGetDeviceProcAddr expected = nullptr;
    realDeviceProc.compare_exchange_strong(expected, real, std::memory_order_relaxed);
    return &nullDeviceProc;
}

bool BatchSubmits() {
    static const bool enabled = [] {
        const char* value = std::getenv("APS5_BATCH_SUBMITS");
        return value == nullptr || (value[0] != '\0' && value[0] != '0');
    }();
    return enabled;
}

namespace {

VkResult SubmitToQueue(VkQueue queue, PFN_vkQueueSubmit submit, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    if (!NullSubmit()) return submit(queue, count, submits, fence);
    // The same submissions with their waits and signals, without the command buffers.
    std::vector<VkSubmitInfo> empty(submits, submits + count);
    for (auto& info : empty) {
        info.commandBufferCount = 0;
        info.pCommandBuffers = nullptr;
    }
    return submit(queue, count, empty.data(), fence);
}

// The submit thread (BatchSubmits): a FIFO of recorded batches, submitted in order. `queueMutex` is held for each
// vkQueueSubmit the thread makes and by AcquireQueue's callers; `mutex` guards the FIFO and the counts.
class SubmitThread {
public:
    // Never destroyed: a static's destructor would join the worker during DLL detach at process exit, after Windows
    // ended the thread, and the winpthreads join then waits forever (every test exited only after a ctest timeout).
    static SubmitThread& Get() {
        static auto* thread = new SubmitThread();
        return *thread;
    }

    void Enqueue(const Context& context, PFN_vkQueueSubmit submit, const QueuedSubmit& work) {
        {
            std::lock_guard lock(mutex);
            rethrowFailure();
            if (!worker.joinable()) worker = std::thread([this] { run(); });
            fifo.push_back(Item{context.queue, submit, work});
            ++enqueued;
        }
        wake.notify_one();
    }

    std::unique_lock<std::mutex> Acquire() {
        {
            std::unique_lock lock(mutex);
            const auto target = enqueued;
            drained.wait(lock, [&] { return submitted >= target; });
            rethrowFailure();
        }
        return std::unique_lock(queueMutex);
    }

    SubmitThreadCounts TakeCounts() {
        std::lock_guard lock(mutex);
        return std::exchange(counts, SubmitThreadCounts{});
    }

private:
    struct Item {
        VkQueue queue;
        PFN_vkQueueSubmit submit;
        QueuedSubmit work;
    };

    ~SubmitThread() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        wake.notify_one();
        if (worker.joinable()) worker.join();
    }

    void rethrowFailure() {
        if (failure == VK_SUCCESS) return;
        Check(std::exchange(failure, VK_SUCCESS), "vkQueueSubmit recorder (submit thread)");
    }

    void run() {
        std::unique_lock lock(mutex);
        for (;;) {
            wake.wait(lock, [&] { return stopping || !fifo.empty(); });
            if (fifo.empty()) return;
            const auto item = fifo.front();
            fifo.pop_front();
            lock.unlock();
            VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submission.commandBufferCount = 1;
            submission.pCommandBuffers = &item.work.commands;
            VkTimelineSemaphoreSubmitInfoKHR timelineInfo{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO_KHR};
            timelineInfo.signalSemaphoreValueCount = 1;
            timelineInfo.pSignalSemaphoreValues = &item.work.value;
            if (item.work.timeline != VK_NULL_HANDLE) {
                submission.pNext = &timelineInfo;
                submission.signalSemaphoreCount = 1;
                submission.pSignalSemaphores = &item.work.timeline;
            }
            const auto start = std::chrono::steady_clock::now();
            VkResult result;
            {
                std::lock_guard queueLock(queueMutex);
                result = SubmitToQueue(item.queue, item.submit, 1, &submission, item.work.fence);
            }
            const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
            lock.lock();
            if (result != VK_SUCCESS && failure == VK_SUCCESS) failure = result;
            ++submitted;
            ++counts.submits;
            counts.submitUs += us;
            drained.notify_all();
        }
    }

    std::mutex mutex;
    std::mutex queueMutex;
    std::condition_variable wake;
    std::condition_variable drained;
    std::deque<Item> fifo;
    std::uint64_t enqueued = 0;
    std::uint64_t submitted = 0;
    VkResult failure = VK_SUCCESS;
    SubmitThreadCounts counts;
    bool stopping = false;
    std::thread worker;
};

}

void EnqueueSubmit(const Context& context, PFN_vkQueueSubmit submit, const QueuedSubmit& work) {
    SubmitThread::Get().Enqueue(context, submit, work);
}

std::unique_lock<std::mutex> AcquireQueue() {
    if (!BatchSubmits()) return {};
    return SubmitThread::Get().Acquire();
}

SubmitThreadCounts TakeSubmitThreadCounts() {
    if (!BatchSubmits()) return {};
    return SubmitThread::Get().TakeCounts();
}

VkResult QueueSubmit(const Context&, VkQueue queue, PFN_vkQueueSubmit submit, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    const auto queueLock = AcquireQueue();
    return SubmitToQueue(queue, submit, count, submits, fence);
}

namespace {

struct GpuMemoryCounts {
    std::array<std::atomic<std::int64_t>, static_cast<std::size_t>(GpuMemoryKind::Count)> bytes{};
    std::array<std::atomic<std::int64_t>, static_cast<std::size_t>(GpuMemoryKind::Count)> objects{};
    std::atomic<std::int64_t> lastReport{0};
};

GpuMemoryCounts& MemoryCounts() {
    static GpuMemoryCounts counts;
    return counts;
}

thread_local std::uint64_t outOfMemoryFailures = 0;

std::string describeGpuMemory() {
    static constexpr std::array<const char*, static_cast<std::size_t>(GpuMemoryKind::Count)> names{"host buffers", "device buffers", "host imports", "textures", "storage images", "depth surfaces", "render targets", "shadow slabs"};
    auto& counts = MemoryCounts();
    std::string text;
    std::int64_t objects = 0;
    for (std::size_t kind = 0; kind < names.size(); ++kind) {
        char item[96];
        const auto count = counts.objects[kind].load(std::memory_order_relaxed);
        objects += count;
        std::snprintf(item, sizeof(item), "%s%s %.0f MiB (%lld)", kind == 0 ? "" : ", ", names[kind], counts.bytes[kind].load(std::memory_order_relaxed) / 1048576.0, static_cast<long long>(count));
        text += item;
    }
    char tail[96];
    std::snprintf(tail, sizeof(tail), "; %lld allocations", static_cast<long long>(objects));
    return text + tail;
}

bool outOfMemory(VkResult result) {
    return result == VK_ERROR_OUT_OF_DEVICE_MEMORY || result == VK_ERROR_OUT_OF_HOST_MEMORY;
}

}

void CountGpuMemory(GpuMemoryKind kind, std::int64_t bytes) {
    auto& counts = MemoryCounts();
    const auto index = static_cast<std::size_t>(kind);
    counts.bytes[index].fetch_add(bytes, std::memory_order_relaxed);
    counts.objects[index].fetch_add(bytes < 0 ? -1 : 1, std::memory_order_relaxed);
    static const bool trace = std::getenv("APS5_TRACE_GPU_MEMORY") != nullptr;
    if (!trace) return;
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = counts.lastReport.load(std::memory_order_relaxed);
    if (now - last < 10 || !counts.lastReport.compare_exchange_strong(last, now)) return;
    std::fprintf(stderr, "[gpumem] live: %s\n", describeGpuMemory().c_str());
}

std::uint64_t LiveGpuMemory() {
    std::int64_t total = 0;
    for (const auto& bytes : MemoryCounts().bytes) total += bytes.load(std::memory_order_relaxed);
    return total > 0 ? static_cast<std::uint64_t>(total) : 0;
}

std::uint64_t OutOfMemoryFailures() {
    return outOfMemoryFailures;
}

VkResult AllocateGpuMemory(const Context& context, const VkMemoryAllocateInfo& allocation, VkDeviceMemory& memory, GpuMemoryKind kind, const char* what) {
    const auto allocate = context.Function<PFN_vkAllocateMemory>("vkAllocateMemory");
    auto result = allocate(context.device, &allocation, nullptr, &memory);
    VkDeviceSize trimmed = 0;
    if (outOfMemory(result) && context.bufferPool != nullptr && (trimmed = context.bufferPool->Trim()) != 0) result = allocate(context.device, &allocation, nullptr, &memory);
    if (result == VK_SUCCESS) {
        CountGpuMemory(kind, static_cast<std::int64_t>(allocation.allocationSize));
        return result;
    }
    memory = VK_NULL_HANDLE;
    if (!outOfMemory(result)) return result;
    ++outOfMemoryFailures;
    const auto& type = context.memory.memoryTypes[allocation.memoryTypeIndex];
    const auto& heap = context.memory.memoryHeaps[type.heapIndex];
    static std::atomic<std::uint64_t> failures{0};
    const auto failure = failures.fetch_add(1, std::memory_order_relaxed) + 1;
    if (failure > 4 && failure % 100 != 0) return result;
    const auto pool = context.bufferPool != nullptr ? context.bufferPool->RetainedBytes() : std::pair<VkDeviceSize, VkDeviceSize>{0, 0};
    const auto* recorder = GuestMemory::GpuMutex().HeldByThisThread() ? Recorder::Active() : nullptr;
    char batches[48] = "";
    if (recorder != nullptr) std::snprintf(batches, sizeof(batches), ", %zu batches in flight", recorder->InFlightBatches());
    std::fprintf(stderr, "[gpumem] vkAllocateMemory %s of %.1f MiB failed (%d, failure %llu): memory type %u (flags 0x%x) in heap %u (%.0f MiB, flags 0x%x); pool emptied %.0f MiB first; live: %s of %u allowed; pool retains %.0f MiB host, %.0f MiB device; host import limit %.0f MiB%s\n", what, allocation.allocationSize / 1048576.0, static_cast<int>(result), static_cast<unsigned long long>(failure), allocation.memoryTypeIndex, type.propertyFlags, type.heapIndex, heap.size / 1048576.0, heap.flags, trimmed / 1048576.0, describeGpuMemory().c_str(), context.limits.maxMemoryAllocationCount, pool.first / 1048576.0, pool.second / 1048576.0, HostImportLimit() / 1048576.0, batches);
    return result;
}

Buffer::Buffer(const Context& context, std::size_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) : context(context), size(size), capacity(BufferPool::Capacity(size)), usage(usage), properties(properties) {
    Require(size != 0, "zero-sized GPU buffer");
    const bool addressable = (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0;
    Require(!addressable || context.bufferDeviceAddress, "buffer device address is not enabled");
    cache = GetBufferPool(context);
    if (const auto allocation = cache->Take(size, usage, properties)) {
        buffer = allocation->buffer;
        memory = allocation->memory;
        mapping = allocation->mapping;
        deviceAddress = allocation->address;
        allocationBytes = allocation->allocationBytes;
        ready = true;
        return;
    }
    try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = capacity;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        const VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0};
        if (addressable) allocation.pNext = &flags;
        allocation.allocationSize = requirements.size;
        allocationBytes = requirements.size;
        // The CPU reads most of these buffers back (write-back, diffs), which is very slow from
        // write-combined memory, so the default host properties prefer cached host memory.
        constexpr VkMemoryPropertyFlags hostDefault = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        if (properties == hostDefault) {
            try {
                allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, hostDefault | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            } catch (const std::runtime_error&) {
                allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, hostDefault);
            }
        } else {
            allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, properties);
        }
        Check(AllocateGpuMemory(context, allocation, memory, (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 ? GpuMemoryKind::HostBuffer : GpuMemoryKind::DeviceBuffer, "buffer"), "vkAllocateMemory buffer");
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0), "vkBindBufferMemory");
        initializeAddress(usage);
        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) Check(context.Function<PFN_vkMapMemory>("vkMapMemory")(context.device, memory, 0, VK_WHOLE_SIZE, 0, &mapping), "vkMapMemory");
        ready = true;
    } catch (...) {
        release();
        throw;
    }
}

Buffer::~Buffer() {
    release();
}

void Buffer::release() noexcept {
    if (ready && cache) {
        cache->Put({buffer, memory, mapping, deviceAddress, allocationBytes, capacity, usage, properties});
        return;
    }
    if (mapping) context.Function<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, memory);
    if (buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
    if (memory) {
        context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
        CountGpuMemory((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0 ? GpuMemoryKind::HostBuffer : GpuMemoryKind::DeviceBuffer, -static_cast<std::int64_t>(allocationBytes));
    }
}

VkBuffer Buffer::Handle() const {
    return buffer;
}

std::span<std::byte> Buffer::Bytes() {
    Require(mapping != nullptr, "device-local buffer has no host mapping");
    return {static_cast<std::byte*>(mapping), size};
}

void Buffer::Invalidate() {
    Require(mapping != nullptr, "cannot invalidate an unmapped GPU buffer");
    VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
    range.memory = memory;
    range.size = VK_WHOLE_SIZE;
    Check(context.Function<PFN_vkInvalidateMappedMemoryRanges>("vkInvalidateMappedMemoryRanges")(context.device, 1, &range), "vkInvalidateMappedMemoryRanges");
}

DeviceBuffer::DeviceBuffer(const Context& context, std::size_t size, VkBufferUsageFlags usage) : context(context), size(size), capacity(BufferPool::Capacity(size)), usage(usage) {
    Require(size != 0, "zero-sized device buffer");
    cache = GetBufferPool(context);
    if (const auto allocation = cache->Take(size, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
        buffer = allocation->buffer;
        memory = allocation->memory;
        allocationBytes = allocation->allocationBytes;
        return;
    }
    try {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = capacity;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        Check(context.Function<PFN_vkCreateBuffer>("vkCreateBuffer")(context.device, &info, nullptr, &buffer), "vkCreateBuffer device");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(context.device, buffer, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocationBytes = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(AllocateGpuMemory(context, allocation, memory, GpuMemoryKind::DeviceBuffer, "device buffer"), "vkAllocateMemory device buffer");
        Check(context.Function<PFN_vkBindBufferMemory>("vkBindBufferMemory")(context.device, buffer, memory, 0), "vkBindBufferMemory device");
    } catch (...) {
        release();
        throw;
    }
}

DeviceBuffer::~DeviceBuffer() {
    release();
}

void DeviceBuffer::release() noexcept {
    if (buffer && memory && cache) {
        cache->Put({buffer, memory, nullptr, 0, allocationBytes, capacity, usage, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT});
        return;
    }
    if (buffer) context.Function<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer, nullptr);
    if (memory) {
        context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
        CountGpuMemory(GpuMemoryKind::DeviceBuffer, -static_cast<std::int64_t>(allocationBytes));
    }
}

VkBuffer DeviceBuffer::Handle() const {
    return buffer;
}

std::size_t DeviceBuffer::Size() const {
    return size;
}

void CopyBuffer(const Context& context, VkCommandBuffer commands, VkBuffer source, VkDeviceSize sourceOffset, VkBuffer destination, VkDeviceSize destinationOffset, VkDeviceSize bytes) {
    const VkBufferCopy region{sourceOffset, destinationOffset, bytes};
    context.Resolved(&DeviceFunctions::cmdCopyBuffer, "vkCmdCopyBuffer")(commands, source, destination, 1, &region);
}

void RecordMemoryBarrier(const Context& context, VkCommandBuffer commands, VkPipelineStageFlags sourceStage, VkPipelineStageFlags destinationStage, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) {
    const VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, sourceAccess, destinationAccess};
    context.Resolved(&DeviceFunctions::cmdPipelineBarrier, "vkCmdPipelineBarrier")(commands, sourceStage, destinationStage, 0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void FillDeviceFunctions(const Context& context, DeviceFunctions& functions) {
    functions.cmdPipelineBarrier = context.Function<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    functions.cmdCopyBuffer = context.Function<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    functions.cmdUpdateBuffer = context.Function<PFN_vkCmdUpdateBuffer>("vkCmdUpdateBuffer");
    functions.cmdFillBuffer = context.Function<PFN_vkCmdFillBuffer>("vkCmdFillBuffer");
    functions.cmdBindPipeline = context.Function<PFN_vkCmdBindPipeline>("vkCmdBindPipeline");
    functions.cmdBindDescriptorSets = context.Function<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
    functions.cmdPushConstants = context.Function<PFN_vkCmdPushConstants>("vkCmdPushConstants");
    functions.cmdDispatch = context.Function<PFN_vkCmdDispatch>("vkCmdDispatch");
    functions.cmdDispatchIndirect = context.Function<PFN_vkCmdDispatchIndirect>("vkCmdDispatchIndirect");
    functions.cmdBeginRenderPass = context.Function<PFN_vkCmdBeginRenderPass>("vkCmdBeginRenderPass");
    functions.cmdEndRenderPass = context.Function<PFN_vkCmdEndRenderPass>("vkCmdEndRenderPass");
    functions.cmdSetViewport = context.Function<PFN_vkCmdSetViewport>("vkCmdSetViewport");
    functions.cmdSetScissor = context.Function<PFN_vkCmdSetScissor>("vkCmdSetScissor");
    functions.cmdSetDepthBounds = context.Function<PFN_vkCmdSetDepthBounds>("vkCmdSetDepthBounds");
    functions.cmdSetDepthBias = context.Function<PFN_vkCmdSetDepthBias>("vkCmdSetDepthBias");
    functions.cmdBindVertexBuffers = context.Function<PFN_vkCmdBindVertexBuffers>("vkCmdBindVertexBuffers");
    functions.cmdBindIndexBuffer = context.Function<PFN_vkCmdBindIndexBuffer>("vkCmdBindIndexBuffer");
    functions.cmdDraw = context.Function<PFN_vkCmdDraw>("vkCmdDraw");
    functions.cmdDrawIndexed = context.Function<PFN_vkCmdDrawIndexed>("vkCmdDrawIndexed");
    functions.cmdDrawIndirect = context.Function<PFN_vkCmdDrawIndirect>("vkCmdDrawIndirect");
    functions.cmdDrawIndexedIndirect = context.Function<PFN_vkCmdDrawIndexedIndirect>("vkCmdDrawIndexedIndirect");
    functions.cmdCopyBufferToImage = context.Function<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    functions.cmdCopyImageToBuffer = context.Function<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    functions.cmdClearColorImage = context.Function<PFN_vkCmdClearColorImage>("vkCmdClearColorImage");
    functions.updateDescriptorSets = context.Function<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
    functions.allocateDescriptorSets = context.Function<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
    functions.getFenceStatus = context.Function<PFN_vkGetFenceStatus>("vkGetFenceStatus");
}

RenderTarget::RenderTarget(const Context& context, const ColorTarget& target, bool blending) : context(context) {
    VkFormatProperties properties{};
    context.formatProperties(context.physical, target.format, &properties);
    const VkFormatFeatureFlags required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | (blending ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT : 0u);
    Require((properties.optimalTilingFeatures & required) == required, "render-target format does not support required operations");
    constexpr auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageFormatProperties supported{};
    Check(context.imageFormatProperties(context.physical, target.format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, usage, 0, &supported), "vkGetPhysicalDeviceImageFormatProperties");
    Require(target.extent.width <= supported.maxExtent.width && target.extent.height <= supported.maxExtent.height && (supported.sampleCounts & VK_SAMPLE_COUNT_1_BIT) != 0 && target.bytes <= supported.maxResourceSize, "render target exceeds device image limits");
    Require(target.extent.width <= context.limits.maxFramebufferWidth && target.extent.height <= context.limits.maxFramebufferHeight, "render target exceeds framebuffer limits");
    try {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = target.format;
        info.extent = {target.extent.width, target.extent.height, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        Check(context.Function<PFN_vkCreateImage>("vkCreateImage")(context.device, &info, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements requirements{};
        context.Function<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(context.device, image, &requirements);
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = context.MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(AllocateGpuMemory(context, allocation, memory, GpuMemoryKind::RenderTarget, "render target"), "vkAllocateMemory render target");
        allocationBytes = allocation.allocationSize;
        Check(context.Function<PFN_vkBindImageMemory>("vkBindImageMemory")(context.device, image, memory, 0), "vkBindImageMemory");
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = target.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        Check(context.Function<PFN_vkCreateImageView>("vkCreateImageView")(context.device, &viewInfo, nullptr, &view), "vkCreateImageView");
    } catch (...) {
        release();
        throw;
    }
}

RenderTarget::~RenderTarget() {
    release();
}

void RenderTarget::release() noexcept {
    if (view) context.Function<PFN_vkDestroyImageView>("vkDestroyImageView")(context.device, view, nullptr);
    if (image) context.Function<PFN_vkDestroyImage>("vkDestroyImage")(context.device, image, nullptr);
    if (memory) {
        context.Function<PFN_vkFreeMemory>("vkFreeMemory")(context.device, memory, nullptr);
        CountGpuMemory(GpuMemoryKind::RenderTarget, -static_cast<std::int64_t>(allocationBytes));
    }
}

VkImage RenderTarget::Image() const {
    return image;
}

VkImageView RenderTarget::View() const {
    return view;
}

CommandBatch::CommandBatch(const Context& context) : context(context) {
    // Records into the device's one command pool and submits to its queue: device-lock work only.
    GuestMemory::AssertGpuLockHeld("CommandBatch");
    try {
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = context.pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1;
        Check(context.Function<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(context.device, &allocation, &commands), "vkAllocateCommandBuffers");
        VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Check(context.Function<PFN_vkCreateFence>("vkCreateFence")(context.device, &info, nullptr, &fence), "vkCreateFence");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Check(context.Function<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin), "vkBeginCommandBuffer");
        // Work recorded so far goes first in queue order, so this batch sees its results.
        if (auto* recorder = Recorder::Active(); recorder != nullptr && recorder->Recording()) recorder->Submit();
    } catch (...) {
        release();
        throw;
    }
}

CommandBatch::~CommandBatch() {
    release();
}

void CommandBatch::release() noexcept {
    if (pending) {
        auto result = context.Function<PFN_vkGetFenceStatus>("vkGetFenceStatus")(context.device, fence);
        if (result == VK_NOT_READY) {
            const auto queueLock = AcquireQueue();
            result = context.Function<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(context.queue);
        }
        if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) std::terminate();
    }
    if (commands) context.Function<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(context.device, context.pool, 1, &commands);
    if (fence) context.Function<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
}

VkCommandBuffer CommandBatch::Handle() const {
    return commands;
}

void CommandBatch::SubmitAndWait() {
    Submit();
    Wait();
}

void CommandBatch::Submit() {
    PerformanceTimer timing("Graphics.Submit");
    Require(!submitted, "command batch has already been submitted");
    Check(context.Function<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands), "vkEndCommandBuffer");
    VkSubmitInfo submission{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submission.commandBufferCount = 1;
    submission.pCommandBuffers = &commands;
    timing.Mark("command_end");
    Check(QueueSubmit(context, context.queue, context.Function<PFN_vkQueueSubmit>("vkQueueSubmit"), 1, &submission, fence), "vkQueueSubmit graphics");
    timing.Mark("queue_submit");
    pending = true;
    submitted = true;
}

void CommandBatch::Wait() {
    Require(submitted, "command batch has not been submitted");
    if (!pending) return;
    PerformanceTimer timing("Graphics.Wait");
    const auto result = context.Function<PFN_vkWaitForFences>("vkWaitForFences")(context.device, 1, &fence, VK_TRUE, 5'000'000'000ULL);
    timing.Mark("fence_wait");
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST) pending = false;
    Check(result, "vkWaitForFences graphics");
    // Recorded batches preceded this one, so their completions (write-backs) can run now.
    if (auto* recorder = Recorder::Active()) recorder->Reap();
}

}
