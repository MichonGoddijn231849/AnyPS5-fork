#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECACHE_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_GRAPHICS_INCLUDE_PIPELINECACHE_HPP

#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace AgcDriver::Graphics {

class PipelineCacheSaveSchedule {
public:
    using Clock = std::chrono::steady_clock;
    struct Settings {
        Clock::duration minSpacing = std::chrono::seconds(60);
        Clock::duration quiet = std::chrono::seconds(3);
        Clock::duration maxDeferral = std::chrono::minutes(1);
        std::size_t minGrowth = std::size_t{1} << 20u;
    };

    PipelineCacheSaveSchedule(Settings settings, std::size_t savedBytes, Clock::time_point start) : settings(settings), savedBytes(savedBytes), lastSave(start) {}
    bool Ready(Clock::time_point now, std::uint64_t creations, Clock::time_point lastCreation) const {
        if (creations == checkedCreations) return false;
        if (now - lastCreation < settings.quiet) return now - lastSave >= settings.maxDeferral;
        return now - lastSave >= settings.minSpacing;
    }
    bool Worth(std::size_t bytes) const { return bytes >= savedBytes + settings.minGrowth; }
    bool WorthAtTeardown(std::size_t bytes) const { return bytes != 0 && bytes != savedBytes; }
    void Saved(Clock::time_point now, std::uint64_t creations, std::size_t bytes) {
        savedBytes = bytes;
        checkedCreations = creations;
        lastSave = now;
    }
    void Skipped(std::uint64_t creations) { checkedCreations = creations; }
    template<typename TBytes, typename TSave>
    bool Tick(Clock::time_point now, std::uint64_t creations, Clock::time_point lastCreation, TBytes&& bytes, TSave&& save) {
        if (!Ready(now, creations, lastCreation)) return false;
        if (!Worth(bytes())) {
            Skipped(creations);
            return false;
        }
        std::size_t saved = 0;
        if (!save(saved)) {
            Skipped(creations);
            return false;
        }
        Saved(now, creations, saved);
        return true;
    }

private:
    Settings settings;
    std::size_t savedBytes = 0;
    std::uint64_t checkedCreations = 0;
    Clock::time_point lastSave;
};

class PipelineCache {
public:
    static constexpr int SaveIntervalSeconds = 60;

    PipelineCache(const Context& context, const VkPhysicalDeviceProperties& properties);
    ~PipelineCache();

    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;
    VkPipelineCache Handle() const { return cache; }

    static void NoteCreation() {
        lastCreationTicks.store(PipelineCacheSaveSchedule::Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
        creations.fetch_add(1, std::memory_order_release);
    }
    static std::uint64_t Creations() { return creations.load(std::memory_order_acquire); }
    static PipelineCacheSaveSchedule::Clock::time_point LastCreation() {
        return PipelineCacheSaveSchedule::Clock::time_point(PipelineCacheSaveSchedule::Clock::duration(lastCreationTicks.load(std::memory_order_relaxed)));
    }

private:
    static inline std::atomic<std::uint64_t> creations{0};
    static inline std::atomic<PipelineCacheSaveSchedule::Clock::rep> lastCreationTicks{0};
    void load(std::vector<std::byte>& initialData);
    std::size_t currentBytes() const;
    bool save(bool final);
    void run();

    Context context;
    VkPhysicalDeviceProperties properties{};
    VkPipelineCache cache = VK_NULL_HANDLE;
    std::filesystem::path path;
    std::size_t savedBytes = 0;
    std::mutex mutex;
    std::condition_variable wake;
    bool stopping = false;
    std::thread saver;
};

}

#endif
