#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_PASSFILES_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_DRIVER_CAPTURE_PASSFILES_HPP

// The files a captured pass keeps its ranges in (APS5_CAPTURE_PASSES, agc_pass_run, agc_dispatch_run, tools/fidelity).
// A range is <directory>/<address>.bin, a hard link to a blob named by its content; a range above ChunkThreshold is
// <directory>/<address>.chunks instead, a text manifest
//   chunks <bytes hex> <chunk bytes hex> <blob directory>
//   <blob name>            (one line per chunk, in order)
// so passes that bind the same large heap share every chunk that did not change.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace AgcDriver::PassFiles {

inline constexpr std::uint64_t ChunkThreshold = 64ull << 20u;
inline constexpr std::uint64_t ChunkBytes = 1ull << 20u;

inline std::string BlobName(std::string_view data) {
    char name[64];
    std::snprintf(name, sizeof(name), "%016zx_%zx.bin", std::hash<std::string_view>{}(data), data.size());
    return name;
}

inline std::string RangeName(std::uint64_t address, bool chunked) {
    char name[40];
    std::snprintf(name, sizeof(name), "%llx.%s", static_cast<unsigned long long>(address), chunked ? "chunks" : "bin");
    return name;
}

// Stores `data` as a blob unless one with that content exists; `admit(bytes)` decides whether new bytes may be written
// (a budget). Returns the blob path, or an empty path when admit refused.
template <typename TAdmit>
std::filesystem::path StoreBlob(const std::filesystem::path& blobs, std::string_view data, TAdmit&& admit) {
    std::error_code error;
    const auto blob = blobs / BlobName(data);
    if (std::filesystem::exists(blob, error)) return blob;
    if (!admit(data.size())) return {};
    std::filesystem::create_directories(blobs, error);
    const auto partial = blobs / (BlobName(data) + ".part");
    std::ofstream(partial, std::ios::binary).write(data.data(), static_cast<std::streamsize>(data.size()));
    std::filesystem::rename(partial, blob, error);
    return blob;
}

// Writes the range under `directory` (a hard-linked blob, or a chunk manifest above the threshold). Returns false when
// `admit` refused new bytes (nothing is left half written).
template <typename TAdmit>
bool SaveRange(const std::filesystem::path& directory, const std::filesystem::path& blobs, std::uint64_t address, std::string_view data, TAdmit&& admit) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (data.size() <= ChunkThreshold) {
        const auto blob = StoreBlob(blobs, data, admit);
        if (blob.empty()) return false;
        const auto target = directory / RangeName(address, false);
        std::filesystem::remove(target, error);
        std::filesystem::create_hard_link(blob, target, error);
        if (error) std::filesystem::copy_file(blob, target, std::filesystem::copy_options::overwrite_existing, error);
        return true;
    }
    std::string manifest;
    char line[1200];
    std::snprintf(line, sizeof(line), "chunks %zx %llx %s\n", data.size(), static_cast<unsigned long long>(ChunkBytes), std::filesystem::absolute(blobs).string().c_str());
    manifest += line;
    for (std::size_t offset = 0; offset < data.size(); offset += ChunkBytes) {
        const auto chunk = data.substr(offset, std::min<std::size_t>(ChunkBytes, data.size() - offset));
        if (StoreBlob(blobs, chunk, admit).empty()) return false;
        manifest += BlobName(chunk) + "\n";
    }
    std::ofstream(directory / RangeName(address, true), std::ios::binary) << manifest;
    return true;
}

// Reads a saved range (either form) into `destination` (at most `bytes`); returns the bytes read, 0 when absent.
inline std::size_t LoadRange(const std::filesystem::path& directory, std::uint64_t address, void* destination, std::size_t bytes) {
    if (std::ifstream file(directory / RangeName(address, false), std::ios::binary); file) {
        file.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
        return static_cast<std::size_t>(file.gcount());
    }
    std::ifstream manifest(directory / RangeName(address, true));
    if (!manifest) return 0;
    std::string word, sizeText, chunkText, blobs;
    manifest >> word >> sizeText >> chunkText;
    std::getline(manifest >> std::ws, blobs);
    const auto chunkBytes = static_cast<std::size_t>(std::stoull(chunkText, nullptr, 16));
    std::size_t offset = 0;
    for (std::string name; offset < bytes && manifest >> name; offset += chunkBytes) {
        std::ifstream chunk(std::filesystem::path(blobs) / name, std::ios::binary);
        chunk.read(static_cast<char*>(destination) + offset, static_cast<std::streamsize>(std::min(chunkBytes, bytes - offset)));
    }
    return std::min(offset, bytes);
}

}

#endif
