// agc_surface_png <surface.bin> <out.png> --size WxH[xD[v]] (--format GUEST --tile T | --vk-format F --color-tile C)
//                 [--slice Z] [--raw] [--alpha] [--compare OTHER.bin [--ulps N]]
// Turns a guest surface saved by the driver (APS5_CAPTURE_PASSES, APS5_DUMP_AFTER, agc_dispatch_run --dump) into a PNG
// with the driver's own layout code: DescribeSurface (mips, array layers, volume slabs), the swizzle equations and
// ColorTargetLayout. --format/--tile are a texture's guest format and TextureTileMode as [dispatch-io] prints them
// ("f36 t2"); --vk-format/--color-tile are a color target's VkFormat and ColorTileMode as [draw-io] prints them. Array
// layers and volume slices are laid out as a grid unless --slice picks one. Float and integer formats are stretched to
// each channel's range (--raw keeps them clamped to [0, 1]); alpha is drawn opaque unless --alpha. Mip 0 only.
// --compare draws this dump, the other one and their difference side by side, and prints a "compare:" line counting
// elements that are exact, within N steps of the format's precision (--ulps, default 4) or beyond.
#include "prx/libSceAgcDriver/Graphics/include/ColorTargetLayout.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureFormat.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureSwizzleEquations.hpp"
#include "prx/libSceAgcDriver/Graphics/include/TextureTiling.hpp"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace AgcDriver::Graphics;

struct Decoder {
    std::uint32_t bytes = 4;
    // Writes the element's channels as floats; returns whether the format is normalized (no stretch needed).
    bool (*decode)(const std::uint8_t* element, std::array<float, 4>& rgba) = nullptr;
};

float Half(std::uint16_t value) {
    const std::uint32_t sign = (value & 0x8000u) << 16u;
    std::uint32_t exponent = (value >> 10u) & 0x1fu;
    std::uint32_t mantissa = value & 0x3ffu;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa != 0) {
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1u;
                --exponent;
            }
            bits = sign | (exponent << 23u) | ((mantissa & 0x3ffu) << 13u);
        } else {
            bits = sign;
        }
    } else if (exponent == 31) {
        bits = sign | 0x7f800000u | (mantissa << 13u);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23u) | (mantissa << 13u);
    }
    return std::bit_cast<float>(bits);
}

// Unsigned small floats (B10G11R11): 5-bit exponent, mantissaBits mantissa.
float SmallFloat(std::uint32_t value, std::uint32_t mantissaBits) {
    const std::uint32_t exponent = value >> mantissaBits;
    const std::uint32_t mantissa = value & ((1u << mantissaBits) - 1u);
    if (exponent == 0) return std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissaBits));
    if (exponent == 31) return mantissa == 0 ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    return std::ldexp(1.0f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissaBits), static_cast<int>(exponent) - 15);
}

template <typename T>
T Load(const std::uint8_t* bytes, std::size_t index = 0) {
    T value;
    std::memcpy(&value, bytes + index * sizeof(T), sizeof(T));
    return value;
}

std::optional<Decoder> DecoderFor(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB: return Decoder{1, [](const std::uint8_t* e, std::array<float, 4>& c) { c = {e[0] / 255.0f, e[0] / 255.0f, e[0] / 255.0f, 1}; return true; }};
        case VK_FORMAT_R8_UINT: return Decoder{1, [](const std::uint8_t* e, std::array<float, 4>& c) { c = {float(e[0]), float(e[0]), float(e[0]), 1}; return false; }};
        case VK_FORMAT_R8G8_UNORM: return Decoder{2, [](const std::uint8_t* e, std::array<float, 4>& c) { c = {e[0] / 255.0f, e[1] / 255.0f, 0, 1}; return true; }};
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return Decoder{4, [](const std::uint8_t* e, std::array<float, 4>& c) { c = {e[0] / 255.0f, e[1] / 255.0f, e[2] / 255.0f, e[3] / 255.0f}; return true; }};
        case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return Decoder{4, [](const std::uint8_t* e, std::array<float, 4>& c) { c = {e[2] / 255.0f, e[1] / 255.0f, e[0] / 255.0f, e[3] / 255.0f}; return true; }};
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return Decoder{4, [](const std::uint8_t* e, std::array<float, 4>& c) { const auto v = Load<std::uint32_t>(e); c = {(v & 1023u) / 1023.0f, ((v >> 10u) & 1023u) / 1023.0f, ((v >> 20u) & 1023u) / 1023.0f, (v >> 30u) / 3.0f}; return true; }};
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return Decoder{4, [](const std::uint8_t* e, std::array<float, 4>& c) { const auto v = Load<std::uint32_t>(e); c = {((v >> 20u) & 1023u) / 1023.0f, ((v >> 10u) & 1023u) / 1023.0f, (v & 1023u) / 1023.0f, (v >> 30u) / 3.0f}; return true; }};
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return Decoder{4, [](const std::uint8_t* e, std::array<float, 4>& c) { const auto v = Load<std::uint32_t>(e); c = {SmallFloat(v & 0x7ffu, 6), SmallFloat((v >> 11u) & 0x7ffu, 6), SmallFloat(v >> 22u, 5), 1}; return false; }};
        case VK_FORMAT_R16_UNORM: case VK_FORMAT_D16_UNORM: return Decoder{2, [](const std::uint8_t* e, std::array<float, 4>& c) { const float v = Load<std::uint16_t>(e) / 65535.0f; c = {v, v, v, 1}; return true; }};
        case VK_FORMAT_R16_UINT: return Decoder{2, [](const std::uint8_t* e, std::array<float, 4>& c) { const float v = Load<std::uint16_t>(e); c = {v, v, v, 1}; return false; }};
        case VK_FORMAT_R16_SFLOAT: return Decoder{2, [](const std::uint8_t* e, std::array<float, 4>& c) { const float v = Half(Load<std::uint16_t>(e)); c = {v, v, v, 1}; return false; }};
        case VK_FORMAT_R16G16_UNORM: return Decoder{4, [](const std::uint8_t* e, std::array<float, 4>& c) { c = {Load<std::uint16_t>(e) / 65535.0f, Load<std::uint16_t>(e, 1) / 65535.0f, 0, 1}; return true; }};
        case VK_FORMAT_R16G16_SFLOAT: return Decoder{4, [](const std::uint8_t* e, std::array<float, 4>& c) { c = {Half(Load<std::uint16_t>(e)), Half(Load<std::uint16_t>(e, 1)), 0, 1}; return false; }};
        case VK_FORMAT_R16G16B16A16_UNORM: return Decoder{8, [](const std::uint8_t* e, std::array<float, 4>& c) { for (int i = 0; i < 4; ++i) c[i] = Load<std::uint16_t>(e, i) / 65535.0f; return true; }};
        case VK_FORMAT_R16G16B16A16_SFLOAT: return Decoder{8, [](const std::uint8_t* e, std::array<float, 4>& c) { for (int i = 0; i < 4; ++i) c[i] = Half(Load<std::uint16_t>(e, i)); return false; }};
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: return Decoder{4, [](const std::uint8_t* e, std::array<float, 4>& c) { const float v = Load<float>(e); c = {v, v, v, 1}; return false; }};
        case VK_FORMAT_R32_UINT: return Decoder{4, [](const std::uint8_t* e, std::array<float, 4>& c) { const float v = static_cast<float>(Load<std::uint32_t>(e)); c = {v, v, v, 1}; return false; }};
        case VK_FORMAT_R32G32_SFLOAT: return Decoder{8, [](const std::uint8_t* e, std::array<float, 4>& c) { c = {Load<float>(e), Load<float>(e, 1), 0, 1}; return false; }};
        case VK_FORMAT_R32G32_UINT: return Decoder{8, [](const std::uint8_t* e, std::array<float, 4>& c) { c = {float(Load<std::uint32_t>(e)), float(Load<std::uint32_t>(e, 1)), 0, 1}; return false; }};
        case VK_FORMAT_R32G32B32A32_SFLOAT: return Decoder{16, [](const std::uint8_t* e, std::array<float, 4>& c) { for (int i = 0; i < 4; ++i) c[i] = Load<float>(e, i); return false; }};
        case VK_FORMAT_R32G32B32A32_UINT: return Decoder{16, [](const std::uint8_t* e, std::array<float, 4>& c) { for (int i = 0; i < 4; ++i) c[i] = static_cast<float>(Load<std::uint32_t>(e, i)); return false; }};
        default: return std::nullopt;
    }
}

// One step of a format's precision, for --compare's tolerance: relative (times the larger magnitude, at least
// `floor`) for floats, absolute for normalized formats; 0 (exact) for integer formats.
struct Precision {
    bool floating = false;
    double step = 0;
    double floor = 0;
};

Precision PrecisionOf(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32B32A32_SFLOAT: return {true, std::ldexp(1.0, -23), std::ldexp(1.0, -126)};
        case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R16G16B16A16_SFLOAT: return {true, std::ldexp(1.0, -10), std::ldexp(1.0, -14)};
        // 6-bit mantissas for red and green, 5 for blue: the coarser one.
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return {true, std::ldexp(1.0, -5), std::ldexp(1.0, -14)};
        case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return {false, 1.0 / 255.0, 0};
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return {false, 1.0 / 1023.0, 0};
        case VK_FORMAT_R16_UNORM: case VK_FORMAT_D16_UNORM: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16B16A16_UNORM: return {false, 1.0 / 65535.0, 0};
        default: return {};
    }
}

std::uint64_t EquationOffset(const TextureSwizzleEquation& equation, std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    std::uint64_t offset = 0;
    for (std::uint32_t bit = 0; bit < 16; ++bit) {
        const auto mask = equation.bits[bit];
        const auto selected = (x & (mask & 0xfffu)) ^ ((y << 12u) & (mask & 0xfff000u)) ^ ((z << 24u) & (mask & 0xff000000u));
        offset |= static_cast<std::uint64_t>(std::popcount(selected) & 1u) << bit;
    }
    return offset;
}

// The hardware SW_MODE of a texture tile mode (thick equations are keyed 0x100 | SW_MODE).
std::uint32_t SwizzleMode(TextureTileMode mode) {
    if (const auto xorMode = XorSwizzleMode(mode)) return xorMode;
    switch (mode) {
        case TextureTileMode::kStandard256B: return 1;
        case TextureTileMode::kStandard4KB: return 5;
        case TextureTileMode::kStandard64KB: return 9;
        case TextureTileMode::kD256B: return 2;
        case TextureTileMode::kD4KB: return 6;
        case TextureTileMode::kD64KB: return 10;
        default: return 0;
    }
}

std::optional<ColorTileMode> AsColorTileMode(TextureTileMode mode) {
    switch (mode) {
        case TextureTileMode::kLinear: return ColorTileMode::Linear;
        case TextureTileMode::kStandard4KB: return ColorTileMode::Standard4KB;
        case TextureTileMode::kStandard64KB: return ColorTileMode::Standard64KB;
        case TextureTileMode::kS4KBX: return ColorTileMode::S4KBX;
        case TextureTileMode::kD4KBX: return ColorTileMode::D4KBX;
        case TextureTileMode::kR64KBX: return ColorTileMode::RenderTarget;
        default: return std::nullopt;
    }
}

}

int main(int argc, char** argv) {
    try {
        if (argc < 3) throw std::runtime_error("usage: agc_surface_png <surface.bin> <out.png> --size WxH[xD[v]] (--format GUEST --tile T | --vk-format F --color-tile C) [--slice Z] [--raw] [--alpha]");
        const std::string input = argv[1];
        const std::string output = argv[2];
        std::uint32_t width = 0, height = 0, depth = 1;
        bool volume = false;
        std::optional<std::uint32_t> guestFormat, tile, vkFormat, colorTile, slice;
        bool raw = false, alpha = false;
        std::optional<std::string> compare;
        double ulps = 4;
        for (int i = 3; i < argc; ++i) {
            const std::string argument = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(argument + " needs a value");
                return argv[++i];
            };
            const auto number = [&] { return static_cast<std::uint32_t>(std::stoul(value(), nullptr, 0)); };
            if (argument == "--size") {
                const auto text = value();
                char suffix = 0;
                const int read = std::sscanf(text.c_str(), "%ux%ux%u%c", &width, &height, &depth, &suffix);
                if (read < 2) throw std::runtime_error("--size takes WxH[xD[v]]");
                if (read < 3) depth = 1;
                volume = suffix == 'v';
            } else if (argument == "--format") guestFormat = number();
            else if (argument == "--tile") tile = number();
            else if (argument == "--vk-format") vkFormat = number();
            else if (argument == "--color-tile") colorTile = number();
            else if (argument == "--slice") slice = number();
            else if (argument == "--raw") raw = true;
            else if (argument == "--alpha") alpha = true;
            else if (argument == "--compare") compare = value();
            else if (argument == "--ulps") ulps = std::stod(value());
            else throw std::runtime_error("unknown argument " + argument);
        }
        if (width == 0 || height == 0) throw std::runtime_error("--size is required");
        std::ifstream file(input, std::ios::binary);
        if (!file) throw std::runtime_error("cannot read " + input);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        const VkFormat format = vkFormat ? static_cast<VkFormat>(*vkFormat) : guestFormat ? ResolveTextureFormat(*guestFormat) : throw std::runtime_error("--format or --vk-format is required");
        const auto decoder = DecoderFor(format);
        if (!decoder) throw std::runtime_error("no PNG decoding for VkFormat " + std::to_string(static_cast<int>(format)) + " yet");
        const auto elementBytes = decoder->bytes;

        // Each slice's element offsets in the dump: z selects an array layer or a volume depth slice.
        std::vector<std::uint32_t> slices;
        if (slice) slices.push_back(*slice);
        else for (std::uint32_t z = 0; z < depth; ++z) slices.push_back(z);
        std::function<std::uint64_t(std::uint32_t, std::uint32_t, std::uint32_t)> offsetOf;
        if (colorTile) {
            const ColorTargetLayout layout(width, height, static_cast<ColorTileMode>(*colorTile), elementBytes);
            const auto sliceBytes = layout.Bytes();
            offsetOf = [layout, sliceBytes](std::uint32_t x, std::uint32_t y, std::uint32_t z) { return z * sliceBytes + layout.Offset(x, y); };
        } else {
            if (!guestFormat || !tile) throw std::runtime_error("a texture needs --format and --tile");
            GuestTextureResource descriptor{};
            descriptor.width = width;
            descriptor.height = height;
            descriptor.depthOrLastArray = depth - 1;
            descriptor.mipCount = 1;
            descriptor.tileMode = static_cast<TextureTileMode>(*tile);
            descriptor.dimension = volume ? TextureDimension::k3D : depth > 1 ? TextureDimension::k2DArray : TextureDimension::k2D;
            descriptor.format = *guestFormat;
            const auto geometry = DescribeSurface(descriptor);
            const auto& mip = geometry.mips.at(0);
            const auto mode = descriptor.tileMode;
            if (geometry.thick) {
                const auto* equation = FindTextureSwizzleEquation(0x100u | SwizzleMode(mode), elementBytes);
                if (equation == nullptr) throw std::runtime_error("no thick swizzle equation for tile mode " + std::to_string(*tile));
                const auto block = ThickBlockExtent(mode, elementBytes);
                const std::uint64_t blockBytes = std::uint64_t{block[0]} * block[1] * block[2] * elementBytes;
                offsetOf = [geometry, mip, equation, block, blockBytes](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
                    return geometry.GuestLayerOffset(z) + mip.tiledOffset + (static_cast<std::uint64_t>(y / block[1]) * mip.blocksPerRow + x / block[0]) * blockBytes + EquationOffset(*equation, x, y, z);
                };
            } else if (mode == TextureTileMode::kLinear) {
                offsetOf = [geometry, mip, elementBytes](std::uint32_t x, std::uint32_t y, std::uint32_t z) { return geometry.GuestLayerOffset(z) + mip.tiledOffset + std::uint64_t{y} * mip.pitchBytes + std::uint64_t{x} * elementBytes; };
            } else if (const auto* equation = FindTextureSwizzleEquation(SwizzleMode(mode), elementBytes)) {
                const auto layout = ThinBlockLayout(mode, elementBytes);
                offsetOf = [geometry, mip, equation, layout](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
                    return geometry.GuestLayerOffset(z) + mip.tiledOffset + (static_cast<std::uint64_t>(y / layout[2]) * mip.blocksPerRow + x / layout[1]) * layout[0] + EquationOffset(*equation, x, y, 0);
                };
            } else if (const auto colorMode = AsColorTileMode(mode)) {
                const ColorTargetLayout layout(width, height, *colorMode, elementBytes);
                offsetOf = [geometry, mip, layout](std::uint32_t x, std::uint32_t y, std::uint32_t z) { return geometry.GuestLayerOffset(z) + mip.tiledOffset + layout.Offset(x, y); };
            } else {
                throw std::runtime_error("tile mode " + std::to_string(*tile) + " has no CPU address function here yet");
            }
        }

        // Decode every slice into floats (a grid of slices), with each channel's range.
        const auto columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(slices.size()))));
        const auto rows = static_cast<std::uint32_t>((slices.size() + columns - 1) / columns);
        const std::uint32_t gridWidth = columns * width, gridHeight = rows * height;
        struct Grid {
            std::vector<std::array<float, 4>> pixels;
            std::array<float, 4> low{}, high{};
            bool normalized = true;
            std::size_t outside = 0;
        };
        const auto decodeGrid = [&](const std::vector<std::uint8_t>& data) {
            Grid grid;
            grid.pixels.assign(static_cast<std::size_t>(gridWidth) * gridHeight, {0, 0, 0, 1});
            grid.low.fill(std::numeric_limits<float>::infinity());
            grid.high.fill(-std::numeric_limits<float>::infinity());
            for (std::size_t s = 0; s < slices.size(); ++s) {
                const auto originX = static_cast<std::uint32_t>(s % columns) * width;
                const auto originY = static_cast<std::uint32_t>(s / columns) * height;
                for (std::uint32_t y = 0; y < height; ++y) {
                    for (std::uint32_t x = 0; x < width; ++x) {
                        const auto offset = offsetOf(x, y, slices[s]);
                        if (offset + elementBytes > data.size()) {
                            ++grid.outside;
                            continue;
                        }
                        std::array<float, 4> rgba{};
                        grid.normalized = decoder->decode(data.data() + offset, rgba) && grid.normalized;
                        for (int c = 0; c < 4; ++c) {
                            if (!std::isfinite(rgba[c])) continue;
                            grid.low[c] = std::min(grid.low[c], rgba[c]);
                            grid.high[c] = std::max(grid.high[c], rgba[c]);
                        }
                        grid.pixels[static_cast<std::size_t>(originY + y) * gridWidth + originX + x] = rgba;
                    }
                }
            }
            return grid;
        };
        const auto first = decodeGrid(bytes);
        std::optional<Grid> second;
        if (compare) {
            std::ifstream other(*compare, std::ios::binary);
            if (!other) throw std::runtime_error("cannot read " + *compare);
            second = decodeGrid(std::vector<std::uint8_t>((std::istreambuf_iterator<char>(other)), std::istreambuf_iterator<char>()));
        }
        // Both images share one stretch, so equal values look equal.
        auto low = first.low, high = first.high;
        if (second) {
            for (int c = 0; c < 4; ++c) {
                low[c] = std::min(low[c], second->low[c]);
                high[c] = std::max(high[c], second->high[c]);
            }
        }
        const bool stretch = !first.normalized && !raw;
        const auto toByte = [&](float v, int c) {
            if (!std::isfinite(v)) v = c == 3 ? 1.0f : 0.0f;
            if (stretch && high[c] > low[c]) v = (v - low[c]) / (high[c] - low[c]);
            return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
        };
        // --compare: per element, exact (same values), near (every channel within the tolerance) or beyond. The
        // tolerance is --ulps steps of the format's precision: relative for floats (with a floor at the smallest
        // normal), absolute for normalized formats; integer formats compare exactly.
        std::uint64_t elements = 0, exact = 0, near = 0, beyond = 0;
        double maxAbs = 0;
        std::string firstBeyond;
        std::vector<std::uint8_t> marks;
        if (second) {
            marks.assign(first.pixels.size(), 0);
            const auto precision = PrecisionOf(format);
            for (std::size_t i = 0; i < first.pixels.size(); ++i) {
                const auto& a = first.pixels[i];
                const auto& b = second->pixels[i];
                ++elements;
                bool same = true, within = true;
                for (int c = 0; c < 4; ++c) {
                    if (std::bit_cast<std::uint32_t>(a[c]) == std::bit_cast<std::uint32_t>(b[c]) || (std::isnan(a[c]) && std::isnan(b[c]))) continue;
                    same = false;
                    const double difference = std::fabs(static_cast<double>(a[c]) - b[c]);
                    if (std::isfinite(difference)) maxAbs = std::max(maxAbs, difference);
                    double tolerance = 0;
                    if (precision.floating) tolerance = ulps * precision.step * std::max({std::fabs(static_cast<double>(a[c])), std::fabs(static_cast<double>(b[c])), precision.floor});
                    else if (precision.step > 0) tolerance = ulps * precision.step;
                    if (!(difference <= tolerance)) within = false;
                }
                if (same) {
                    ++exact;
                } else if (within) {
                    ++near;
                    marks[i] = 1;
                } else {
                    ++beyond;
                    marks[i] = 2;
                    if (firstBeyond.empty()) {
                        const auto gx = static_cast<std::uint32_t>(i % gridWidth), gy = static_cast<std::uint32_t>(i / gridWidth);
                        const auto z = slices[(gy / height) * columns + gx / width];
                        firstBeyond = std::to_string(gx % width) + "," + std::to_string(gy % height) + "," + std::to_string(z);
                    }
                }
            }
        }
        // The PNG: the surface, or with --compare three panels: this dump | the other | the difference (exact: dimmed
        // copy, near: yellow, beyond: red).
        const std::uint32_t panels = second ? 3u : 1u;
        const std::uint32_t gap = second ? 4u : 0u;
        const std::uint32_t outWidth = panels * gridWidth + (panels - 1) * gap, outHeight = gridHeight;
        std::vector<std::uint8_t> image(static_cast<std::size_t>(outWidth) * outHeight * 4, 0);
        for (std::uint32_t y = 0; y < gridHeight; ++y) {
            for (std::uint32_t x = 0; x < gridWidth; ++x) {
                const auto i = static_cast<std::size_t>(y) * gridWidth + x;
                for (std::uint32_t panel = 0; panel < panels; ++panel) {
                    auto* out = &image[(static_cast<std::size_t>(y) * outWidth + panel * (gridWidth + gap) + x) * 4];
                    if (panel < 2) {
                        const auto& source = panel == 0 ? first.pixels[i] : second->pixels[i];
                        for (int c = 0; c < 4; ++c) out[c] = toByte(source[c], c);
                        if (!alpha) out[3] = 255;
                    } else {
                        static constexpr std::array<std::array<std::uint8_t, 3>, 3> colors{{{0, 0, 0}, {255, 220, 0}, {255, 0, 0}}};
                        if (marks[i] == 0) {
                            for (int c = 0; c < 3; ++c) out[c] = static_cast<std::uint8_t>(toByte(first.pixels[i][c], c) / 4);
                        } else {
                            for (int c = 0; c < 3; ++c) out[c] = colors[marks[i]][c];
                        }
                        out[3] = 255;
                    }
                }
            }
        }
        if (!stbi_write_png(output.c_str(), static_cast<int>(outWidth), static_cast<int>(outHeight), 4, image.data(), static_cast<int>(outWidth * 4))) throw std::runtime_error("cannot write " + output);
        std::printf("%s: %ux%u, %zu slice(s) in a %ux%u grid, VkFormat %d, range r[%g, %g] g[%g, %g] b[%g, %g] a[%g, %g]%s%s\n", output.c_str(), width, height, slices.size(), columns, rows, static_cast<int>(format), low[0], high[0], low[1], high[1], low[2], high[2], low[3], high[3], stretch ? ", stretched" : "", first.outside != 0 ? (", " + std::to_string(first.outside) + " elements outside the dump").c_str() : "");
        if (second) std::printf("compare: elements %llu exact %llu near %llu beyond %llu max_abs %g ulps %g first_beyond %s\n", static_cast<unsigned long long>(elements), static_cast<unsigned long long>(exact), static_cast<unsigned long long>(near), static_cast<unsigned long long>(beyond), maxAbs, ulps, firstBeyond.empty() ? "-" : firstBeyond.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "agc_surface_png: %s\n", error.what());
        return 1;
    }
}
