#include "ReplayPlatform.hpp"

#include "prx/libc/include/GuestArena.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ReplayPlatform {

#ifdef _WIN32

MappedFile::MappedFile(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    file = CreateFileW(path.wstring().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open " + name);
    LARGE_INTEGER bytes{};
    if (!GetFileSizeEx(file, &bytes)) throw std::runtime_error("cannot size " + name);
    size = static_cast<std::uint64_t>(bytes.QuadPart);
    if (size != 0) {
        mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (mapping == nullptr) throw std::runtime_error("cannot map " + name);
        data = static_cast<const std::byte*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
        if (data == nullptr) throw std::runtime_error("cannot map a view of " + name);
    }
}

MappedFile::~MappedFile() {
    if (data != nullptr) UnmapViewOfFile(data);
    if (mapping != nullptr) CloseHandle(mapping);
    if (file != nullptr && file != INVALID_HANDLE_VALUE) CloseHandle(file);
}

Protection NoAccess() { return PAGE_NOACCESS; }
Protection ReadOnly() { return PAGE_READONLY; }
Protection ReadWrite() { return PAGE_READWRITE; }

bool Protect(void* address, std::size_t bytes, Protection protection, Protection* previous) {
    DWORD old = 0;
    const bool done = VirtualProtect(address, static_cast<SIZE_T>(bytes), protection, &old) != 0;
    if (previous != nullptr) *previous = old;
    return done;
}

bool Query(const void* address, Region& region) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory)) return false;
    region.base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    region.bytes = memory.RegionSize;
    region.free = memory.State == MEM_FREE;
    region.readable = memory.State == MEM_COMMIT && (memory.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) != 0 && (memory.Protect & PAGE_GUARD) == 0;
    region.privateMemory = memory.Type == MEM_PRIVATE;
    region.allocationBase = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
    return true;
}

bool Reserve(void* address, std::size_t bytes) { return VirtualAlloc(address, static_cast<SIZE_T>(bytes), MEM_RESERVE, PAGE_NOACCESS) != nullptr; }
bool Commit(void* address, std::size_t bytes) { return VirtualAlloc(address, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr; }
bool Decommit(void* address, std::size_t bytes) { return VirtualFree(address, bytes, MEM_DECOMMIT) != 0; }

Section CreateSection(std::uint64_t bytes) {
    return CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, static_cast<DWORD>(bytes >> 32u), static_cast<DWORD>(bytes), nullptr);
}

void CloseSection(Section section) { CloseHandle(section); }

void ArenaMap(void* pointer, std::size_t bytes, Section section, std::uint64_t offset) { GuestArena::GuestArenaMap_nid_postfix(pointer, bytes, section, offset, PAGE_READWRITE); }
void ArenaCommit(void* pointer, std::size_t bytes, std::size_t granule) { GuestArena::GuestArenaCommit_nid_postfix(pointer, bytes, PAGE_READWRITE, granule); }
void ArenaReset(void* pointer, std::size_t bytes) { GuestArena::GuestArenaReset_nid_postfix(pointer, bytes); }
void ArenaSetProtection(std::uintptr_t address, std::size_t bytes, Protection protection) { GuestArena::GuestArenaSetProtection_nid_postfix(address, bytes, protection); }

void DisablePowerThrottling() {
    constexpr ULONG executionSpeed = 0x1;
    constexpr ULONG ignoreTimerResolution = 0x4;
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = executionSpeed | ignoreTimerResolution;
    state.StateMask = 0;
    if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))) std::fprintf(stderr, "[replay] could not opt out of power throttling (error %lu); timings may vary\n", GetLastError());
}

bool SetEnvironment(const char* name, const std::filesystem::path& value) {
    const std::string narrow(name);
    const std::wstring wide(narrow.begin(), narrow.end());
    return _wputenv_s(wide.c_str(), value.wstring().c_str()) == 0;
}

void Exit(int status) {
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(status));
    std::_Exit(status);
}

#else

namespace {

// Reserve's own ranges: the kernel merges adjacent mappings, so /proc/self/maps cannot tell where a reservation began.
std::mutex reservationsMutex;
std::map<std::uintptr_t, std::uintptr_t> reservations;

int SectionFile(Section section) { return static_cast<int>(reinterpret_cast<std::intptr_t>(section)) - 1; }

struct Mapping {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
    bool readable = false;
    bool writable = false;
};

// The mapping holding `address` (true), or the free gap around it (false: begin..end free).
bool FindMapping(std::uintptr_t address, Mapping& found) {
    std::ifstream maps("/proc/self/maps");
    std::string line;
    std::uintptr_t gapBegin = 0;
    while (std::getline(maps, line)) {
        std::istringstream fields(line);
        std::string range;
        std::string permissions;
        fields >> range >> permissions;
        const auto dash = range.find('-');
        const auto begin = static_cast<std::uintptr_t>(std::stoull(range.substr(0, dash), nullptr, 16));
        const auto end = static_cast<std::uintptr_t>(std::stoull(range.substr(dash + 1), nullptr, 16));
        if (address < begin) {
            found = {gapBegin, begin, false, false};
            return false;
        }
        if (address < end) {
            found = {begin, end, permissions.size() > 0 && permissions[0] == 'r', permissions.size() > 1 && permissions[1] == 'w'};
            return true;
        }
        gapBegin = end;
    }
    found = {gapBegin, UINTPTR_MAX, false, false};
    return false;
}

}

MappedFile::MappedFile(const std::filesystem::path& path) {
    const auto name = path.filename().string();
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) throw std::runtime_error("cannot open " + name);
    file = reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor) + 1);
    struct stat status{};
    if (fstat(descriptor, &status) != 0) throw std::runtime_error("cannot size " + name);
    size = static_cast<std::uint64_t>(status.st_size);
    if (size != 0) {
        void* view = mmap(nullptr, static_cast<std::size_t>(size), PROT_READ, MAP_PRIVATE, descriptor, 0);
        if (view == MAP_FAILED) throw std::runtime_error("cannot map " + name);
        data = static_cast<const std::byte*>(view);
    }
}

MappedFile::~MappedFile() {
    if (data != nullptr) munmap(const_cast<std::byte*>(data), static_cast<std::size_t>(size));
    if (file != nullptr) close(static_cast<int>(reinterpret_cast<std::intptr_t>(file)) - 1);
}

Protection NoAccess() { return PROT_NONE; }
Protection ReadOnly() { return PROT_READ; }
Protection ReadWrite() { return PROT_READ | PROT_WRITE; }

bool Protect(void* address, std::size_t bytes, Protection protection, Protection* previous) {
    if (previous != nullptr) {
        Mapping mapping;
        FindMapping(reinterpret_cast<std::uintptr_t>(address), mapping);
        *previous = (mapping.readable ? PROT_READ : 0) | (mapping.writable ? PROT_WRITE : 0);
    }
    return mprotect(address, bytes, static_cast<int>(protection)) == 0;
}

bool Query(const void* address, Region& region) {
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    Mapping mapping;
    const bool mapped = FindMapping(at, mapping);
    region = {};
    region.base = mapping.begin;
    region.bytes = mapping.end - mapping.begin;
    region.free = !mapped;
    region.readable = mapped && mapping.readable;
    std::lock_guard lock(reservationsMutex);
    auto it = reservations.upper_bound(at);
    if (mapped && it != reservations.begin() && at < std::prev(it)->second) {
        --it;
        region.privateMemory = true;
        region.allocationBase = it->first;
        region.base = std::max(region.base, it->first);
        region.bytes = std::min<std::uintptr_t>(mapping.end, it->second) - region.base;
    }
    return true;
}

bool Reserve(void* address, std::size_t bytes) {
    void* view = mmap(address, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (view != address) {
        if (view != MAP_FAILED) munmap(view, bytes);
        return false;
    }
    std::lock_guard lock(reservationsMutex);
    reservations[reinterpret_cast<std::uintptr_t>(address)] = reinterpret_cast<std::uintptr_t>(address) + bytes;
    return true;
}

bool Commit(void* address, std::size_t bytes) { return mprotect(address, bytes, PROT_READ | PROT_WRITE) == 0; }

bool Decommit(void* address, std::size_t bytes) { return mmap(address, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == address; }

Section CreateSection(std::uint64_t bytes) {
    const int descriptor = memfd_create("agc_frame_replay_direct", 0);
    if (descriptor < 0) return nullptr;
    if (ftruncate(descriptor, static_cast<off_t>(bytes)) != 0) {
        close(descriptor);
        return nullptr;
    }
    return reinterpret_cast<Section>(static_cast<std::intptr_t>(descriptor) + 1);
}

void CloseSection(Section section) {
    if (section != nullptr) close(SectionFile(section));
}

void ArenaMap(void* pointer, std::size_t bytes, Section section, std::uint64_t offset) {
    if (mmap(pointer, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, SectionFile(section), static_cast<off_t>(offset)) != pointer) throw std::runtime_error("cannot map a direct memory section");
}

void ArenaCommit(void* pointer, std::size_t bytes, std::size_t) {
    if (mmap(pointer, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != pointer) throw std::runtime_error("cannot commit guest memory");
}

void ArenaReset(void* pointer, std::size_t bytes) { mmap(pointer, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0); }

void ArenaSetProtection(std::uintptr_t, std::size_t, Protection) {}

void DisablePowerThrottling() {}

bool SetEnvironment(const char* name, const std::filesystem::path& value) { return setenv(name, value.c_str(), 1) == 0; }

void Exit(int status) { std::_Exit(status); }

#endif

}
