#include "Optimization/WideFeedback.hpp"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace ShaderRecompiler::WideFeedback {

namespace {

struct State {
    std::mutex mutex;
    std::atomic<const volatile std::uint32_t*> words{nullptr};
    std::uint32_t count = 0;
    std::uint32_t next = 1;
    std::unordered_map<std::uint64_t, std::pair<std::uint32_t, std::uint32_t>> regions;
};

State& state() {
    static State instance;
    return instance;
}

}

void SetMemory(const volatile std::uint32_t* words, std::uint32_t count) {
    auto& self = state();
    std::lock_guard lock(self.mutex);
    self.words.store(words, std::memory_order_release);
    self.count = words != nullptr ? count : 0u;
    self.next = 1;
    self.regions.clear();
}

bool Enabled() {
    return state().words.load(std::memory_order_acquire) != nullptr;
}

std::uint32_t Region(std::uint64_t table, std::uint32_t entries) {
    auto& self = state();
    std::lock_guard lock(self.mutex);
    if (self.words.load(std::memory_order_relaxed) == nullptr) return 0u;
    const auto found = self.regions.find(table);
    if (found != self.regions.end() && found->second.second >= entries) return found->second.first;
    // A table that grew takes a new region (its old flags stay unused).
    if (entries > self.count - self.next) return 0u;
    const auto region = self.next;
    self.next += entries;
    self.regions[table] = {region, entries};
    return region;
}

bool Touched(std::uint32_t region, std::uint32_t key) {
    const auto* words = state().words.load(std::memory_order_acquire);
    return words != nullptr && region != 0u && words[region + key] != 0u;
}

}
