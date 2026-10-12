#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_TOOLS_REPLAYPLATFORM_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_TOOLS_REPLAYPLATFORM_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>

// The host memory and process calls agc_frame_replay makes. On Windows each one is the call the replay always made;
// elsewhere they are POSIX equivalents (mmap, mprotect, memfd), enough to build the tool and parse captures there.
namespace ReplayPlatform {

// A file mapped read-only (pages.bin).
class MappedFile {
public:
    explicit MappedFile(const std::filesystem::path& path);
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    [[nodiscard]] const std::byte* Data() const { return data; }
    [[nodiscard]] std::uint64_t Size() const { return size; }

private:
    void* file = nullptr;
    void* mapping = nullptr;
    const std::byte* data = nullptr;
    std::uint64_t size = 0;
};

// A page protection in the host's own encoding (PAGE_* on Windows, PROT_* elsewhere).
using Protection = std::uint32_t;
[[nodiscard]] Protection NoAccess();
[[nodiscard]] Protection ReadOnly();
[[nodiscard]] Protection ReadWrite();
bool Protect(void* address, std::size_t bytes, Protection protection, Protection* previous);

struct Region {
    std::uintptr_t base = 0;
    std::uint64_t bytes = 0;
    bool free = false;
    bool readable = false;
    // Reserved by this process with Reserve (Windows: MEM_PRIVATE), starting at allocationBase.
    bool privateMemory = false;
    std::uintptr_t allocationBase = 0;
};
// The region of like pages that holds `address`.
bool Query(const void* address, Region& region);

// Reserves exactly [address, address + bytes) without access; Commit makes reserved pages read-write, Decommit
// drops them back to reserved.
bool Reserve(void* address, std::size_t bytes);
bool Commit(void* address, std::size_t bytes);
bool Decommit(void* address, std::size_t bytes);

// Anonymous shared memory a direct memory backing lives in, mapped into the guest arena by ArenaMap.
using Section = void*;
Section CreateSection(std::uint64_t bytes);
void CloseSection(Section section);

// The guest arena calls of the replay's guest space (GuestArena's on Windows).
void ArenaMap(void* pointer, std::size_t bytes, Section section, std::uint64_t offset);
void ArenaCommit(void* pointer, std::size_t bytes, std::size_t granule);
void ArenaReset(void* pointer, std::size_t bytes);
void ArenaSetProtection(std::uintptr_t address, std::size_t bytes, Protection protection);

void DisablePowerThrottling();
bool SetEnvironment(const char* name, const std::filesystem::path& value);
// Ends the process at once, without running destructors of the guest and driver state.
[[noreturn]] void Exit(int status);

}

#endif
