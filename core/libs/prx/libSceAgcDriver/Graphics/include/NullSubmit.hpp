#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_NULLSUBMIT_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_NULLSUBMIT_HPP

#include <cstdint>
#include <mutex>
#include <vulkan/vulkan.h>

namespace AgcDriver::Graphics {

struct Context;

// APS5_NULL_SUBMIT=1 (docs/dev/NIGHT_SHIFT.md B0): the driver's CPU path without GPU work, so a frame replay times
// the worker and the committer on a machine whose GPU (lavapipe) could not run the frame. Everything up to the submit
// runs as usual: recording, descriptor writes, pipeline lookups. Then:
// - every submit is an empty one that only signals the batch's fence and timeline value, so the batch completes at
//   once and every wait and retirement path runs as it would on a GPU that finished;
// - guest pipelines are not compiled: each one is the device's stub pipeline (NullPipeline), bound in command
//   buffers that never execute;
// - the device reports a 32-wide subgroup in every stage, as the NVIDIA machine does, so the recompiler emits what
//   the day machine gets (lavapipe has 8 lanes and no vertex-stage subgroups).
// What GPU work would have written (render targets, storage, labels) is not written: read-backs see what memory
// holds.
bool NullSubmit();

// The stub pipeline of the context's device (an empty compute shader), made on first use. Pipeline destructors leave
// it alone; ReleaseNullPipeline destroys it with the device.
VkPipeline NullPipeline(const Context& context);
void ReleaseNullPipeline(const Context& context);

// The device function resolver the driver uses under NullSubmit: `real`'s functions, with vkCreateImage making a
// sample count lavapipe lacks (it has 1 and 4) at 4, and vkQueueSubmit submitting the same signals without command
// buffers. One real resolver per process (the first device's).
PFN_vkGetDeviceProcAddr NullDeviceProc(PFN_vkGetDeviceProcAddr real);

// vkQueueSubmit of `submits`, or under NullSubmit the same signals (semaphores, fence) without command buffers.
// Under BatchSubmits it first lets the submit thread hand over what it holds and keeps the queue for the call.
VkResult QueueSubmit(const Context& context, VkQueue queue, PFN_vkQueueSubmit submit, std::uint32_t count, const VkSubmitInfo* submits, VkFence fence);

// APS5_BATCH_SUBMITS=1 (docs/dev/NIGHT_SHIFT.md B2): the recorder's batches reach the queue from a submit thread, so
// the committer never blocks in vkQueueSubmit, and batches end only where the guest can observe it (the driver's
// submit points read this switch: no submit at a REWIND segment end the guest already released).
// A batch is in flight from Recorder::Submit on: its fence and timeline value are waited for as before (a wait before
// the thread submitted is valid Vulkan and only lasts longer).
bool BatchSubmits();

// One recorded batch for the submit thread: its command buffer, the timeline value it signals, its fence.
struct QueuedSubmit {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkSemaphore timeline = VK_NULL_HANDLE;
    std::uint64_t value = 0;
    VkFence fence = VK_NULL_HANDLE;
};

// Hands `work` to the submit thread, which submits on context.queue in enqueue order. A submission the thread saw fail
// is rethrown here or at the next AcquireQueue (as Check does).
void EnqueueSubmit(const Context& context, PFN_vkQueueSubmit submit, const QueuedSubmit& work);

// Every other use of the queue (a submit, a present, a wait for idle) goes under this lock: it returns once the submit
// thread submitted everything enqueued before the call, and keeps the thread off the queue while held. Without
// BatchSubmits it is an empty lock.
std::unique_lock<std::mutex> AcquireQueue();

// Batches the submit thread submitted, and the time it spent in vkQueueSubmit (the [recorder] submits line's cost
// moves there under BatchSubmits).
struct SubmitThreadCounts {
    std::uint64_t submits = 0;
    double submitUs = 0;
};
SubmitThreadCounts TakeSubmitThreadCounts();

}

#endif
