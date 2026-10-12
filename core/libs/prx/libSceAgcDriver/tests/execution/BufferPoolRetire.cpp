// The exit crash (docs/dev/NIGHT_SHIFT.md B7): a buffer kept by a static (an image mirror) was destroyed at exit,
// after the Vulkan device, and went back to the BufferPool, which freed its memory on the dead device. VulkanDevice's
// teardown now retires the pool before destroying the device (BufferPool::Retire): the retained slots are destroyed
// while the device lives, and whatever comes back later is dropped without a Vulkan call. A fake device resolver
// counts the calls, so this runs without a GPU.
#include "prx/libSceAgcDriver/Graphics/include/BufferPool.hpp"
#include "prx/libSceAgcDriver/Graphics/include/GuestBufferMemory.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Resources.hpp"
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>

namespace {

using AgcDriver::Graphics::BufferAllocation;
using AgcDriver::Graphics::BufferPool;
using AgcDriver::Graphics::Require;

struct Calls {
    int unmaps = 0;
    int destroys = 0;
    int frees = 0;
    int others = 0;
};

Calls calls;

VKAPI_ATTR void VKAPI_CALL FakeUnmap(VkDevice, VkDeviceMemory) { ++calls.unmaps; }
VKAPI_ATTR void VKAPI_CALL FakeDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) { ++calls.destroys; }
VKAPI_ATTR void VKAPI_CALL FakeFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks*) { ++calls.frees; }
VKAPI_ATTR void VKAPI_CALL FakeOther() { ++calls.others; }

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FakeDeviceProc(VkDevice, const char* name) {
    if (std::strcmp(name, "vkUnmapMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&FakeUnmap);
    if (std::strcmp(name, "vkDestroyBuffer") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&FakeDestroyBuffer);
    if (std::strcmp(name, "vkFreeMemory") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&FakeFreeMemory);
    return reinterpret_cast<PFN_vkVoidFunction>(&FakeOther);
}

// A host-visible allocation of `bytes` (a size class) with fake handles.
BufferAllocation Allocation(std::uintptr_t handle, std::size_t bytes) {
    static std::byte mapping[16];
    return {reinterpret_cast<VkBuffer>(handle), reinterpret_cast<VkDeviceMemory>(handle), mapping, 0, bytes, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
}

int Frees() { return calls.frees; }

}

int main() {
    try {
        AgcDriver::Graphics::Context context{};
        context.device = reinterpret_cast<VkDevice>(std::uintptr_t{0x1000});
        context.deviceProc = &FakeDeviceProc;
        context.limits.maxMemoryAllocationCount = 4096;
        {
            BufferPool pool(context);
            // A live pool retains what comes back and frees on Trim.
            pool.Put(Allocation(0x10, 256));
            Require(Frees() == 0, "retire: a live pool retains a returned allocation");
            Require(pool.Trim() == 256 && Frees() == 1 && calls.unmaps == 1 && calls.destroys == 1, "retire: a live pool frees its slots on Trim");
            // Retire destroys the retained slots while the device lives.
            pool.Put(Allocation(0x20, 256));
            pool.Put(Allocation(0x30, 512));
            pool.Retire();
            Require(Frees() == 3 && calls.destroys == 3, "retire: the retained slots are freed while the device lives");
            Require(pool.RetainedBytes().first == 0, "retire: nothing stays retained");
            // After the device is gone: dropped without a Vulkan call, and nothing is retained.
            pool.Put(Allocation(0x40, 256));
            Require(Frees() == 3 && calls.destroys == 3 && calls.unmaps == 3, "retire: an allocation returned after Retire makes no Vulkan call");
            Require(pool.RetainedBytes().first == 0, "retire: a retired pool retains nothing");
            Require(pool.Trim() == 0 && Frees() == 3, "retire: Trim after Retire frees nothing");
        }
        Require(Frees() == 3, "retire: destroying a retired pool makes no Vulkan call");
        // The exit path itself: a Buffer (an image mirror's) outliving the device's teardown. It takes a retained
        // allocation (no Vulkan call), the pool is retired as VulkanDevice's teardown does, and the buffer is
        // destroyed later, as a static at exit.
        {
            context.bufferPool = std::make_shared<BufferPool>(context);
            context.bufferPool->Put(Allocation(0x50, 256));
            auto kept = std::make_shared<AgcDriver::Graphics::Buffer>(context, 200, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            Require(calls.others == 0, "retire: the kept buffer came from the pool");
            const auto before = calls;
            context.bufferPool->Retire();
            context.bufferPool.reset();
            kept.reset();
            Require(calls.frees == before.frees && calls.destroys == before.destroys && calls.unmaps == before.unmaps, "retire: a buffer destroyed after the teardown makes no Vulkan call");
        }
        // Mirrors of another device (none here) are left alone.
        AgcDriver::Graphics::ClearImageMirrors(context.device);
        std::puts("buffer pool retire tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
