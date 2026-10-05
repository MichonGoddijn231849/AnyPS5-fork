#include "prx/libSceAgcDriver/Execution/include/VulkanDevice.hpp"
#include "prx/libSceAgcDriver/Graphics/include/Draw.hpp"
#include "prx/libc/include/GuestAllocations.hpp"
#include "Recompiler.hpp"
#include "VulkanTestDevice.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using AgcDriver::Graphics::Require;
using ShaderRecompiler::ShaderStage;

constexpr std::uint32_t Threads = 64;
constexpr std::uint32_t RowsPerOp = 16;
constexpr std::uint32_t OpCount = 24;
constexpr std::uint32_t NarrowOps = 13;
constexpr std::uint32_t OpStride = 0x200;
constexpr std::uint32_t Special = OpCount * OpStride;
constexpr std::uint32_t Sentinel = 0x05e471e1;
constexpr std::size_t BlockBytes = 65536;
constexpr std::uint8_t Fill = 0xcd;
alignas(256) std::array<std::uint32_t, OpCount * Threads * 4> Input{};
alignas(256) std::array<std::uint32_t, (OpCount + 2) * Threads * 2> Output{};

alignas(256) constexpr std::array<std::uint32_t, 384> GlobalAtomicsCode{
    0x34020083, 0x34040084, 0x340a0083, 0x7e2802ff, 0x05e471e1, 0x7e2a02ff, 0x05e471e1, 0x4a060280,
    0x4a080480, 0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0a80, 0xdcc18000, 0x14080a03, 0xbf8c3f70,
    0xe0701000, 0x80011406, 0x4a0602ff, 0x00000200, 0x4a0804ff, 0x00000400, 0xe0381000, 0x80000a04,
    0xbf8c3f70, 0x4a0c0aff, 0x00000200, 0xdcc58000, 0x14080a03, 0xbf8c3f70, 0xe0701000, 0x80011406,
    0x4a0602ff, 0x00000400, 0x4a0804ff, 0x00000800, 0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff,
    0x00000400, 0xdcc98000, 0x14080a03, 0xbf8c3f70, 0xe0701000, 0x80011406, 0x4a0602ff, 0x00000600,
    0x4a0804ff, 0x00000c00, 0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff, 0x00000600, 0xdccd8000,
    0x14080a03, 0xbf8c3f70, 0xe0701000, 0x80011406, 0x4a0602ff, 0x00000800, 0x4a0804ff, 0x00001000,
    0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff, 0x00000800, 0xdcd58000, 0x14080a03, 0xbf8c3f70,
    0xe0701000, 0x80011406, 0x4a0602ff, 0x00000a00, 0x4a0804ff, 0x00001400, 0xe0381000, 0x80000a04,
    0xbf8c3f70, 0x4a0c0aff, 0x00000a00, 0xdcd98000, 0x14080a03, 0xbf8c3f70, 0xe0701000, 0x80011406,
    0x4a0602ff, 0x00000c00, 0x4a0804ff, 0x00001800, 0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff,
    0x00000c00, 0xdcdd8000, 0x14080a03, 0xbf8c3f70, 0xe0701000, 0x80011406, 0x4a0602ff, 0x00000e00,
    0x4a0804ff, 0x00001c00, 0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff, 0x00000e00, 0xdce18000,
    0x14080a03, 0xbf8c3f70, 0xe0701000, 0x80011406, 0x4a0602ff, 0x00001000, 0x4a0804ff, 0x00002000,
    0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff, 0x00001000, 0xdce58000, 0x14080a03, 0xbf8c3f70,
    0xe0701000, 0x80011406, 0x4a0602ff, 0x00001200, 0x4a0804ff, 0x00002400, 0xe0381000, 0x80000a04,
    0xbf8c3f70, 0x4a0c0aff, 0x00001200, 0xdce98000, 0x14080a03, 0xbf8c3f70, 0xe0701000, 0x80011406,
    0x4a0602ff, 0x00001400, 0x4a0804ff, 0x00002800, 0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff,
    0x00001400, 0xdced8000, 0x14080a03, 0xbf8c3f70, 0xe0701000, 0x80011406, 0x4a0602ff, 0x00001600,
    0x4a0804ff, 0x00002c00, 0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff, 0x00001600, 0xdcf18000,
    0x14080a03, 0xbf8c3f70, 0xe0701000, 0x80011406, 0x4a0602ff, 0x00001800, 0x4a0804ff, 0x00003000,
    0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff, 0x00001800, 0xdcf58000, 0x14080a03, 0xbf8c3f70,
    0xe0701000, 0x80011406, 0x4a0602ff, 0x00001a00, 0x4a0804ff, 0x00003400, 0xe0381000, 0x80000a04,
    0xbf8c3f70, 0x4a0c0aff, 0x00001a00, 0xdd418000, 0x14080a03, 0xbf8c3f70, 0xe0741000, 0x80011406,
    0x4a0602ff, 0x00001c00, 0x4a0804ff, 0x00003800, 0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff,
    0x00001c00, 0xdd458000, 0x14080a03, 0xbf8c3f70, 0xe0741000, 0x80011406, 0x4a0602ff, 0x00001e00,
    0x4a0804ff, 0x00003c00, 0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff, 0x00001e00, 0xdd498000,
    0x14080a03, 0xbf8c3f70, 0xe0741000, 0x80011406, 0x4a0602ff, 0x00002000, 0x4a0804ff, 0x00004000,
    0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff, 0x00002000, 0xdd4d8000, 0x14080a03, 0xbf8c3f70,
    0xe0741000, 0x80011406, 0x4a0602ff, 0x00002200, 0x4a0804ff, 0x00004400, 0xe0381000, 0x80000a04,
    0xbf8c3f70, 0x4a0c0aff, 0x00002200, 0xdd558000, 0x14080a03, 0xbf8c3f70, 0xe0741000, 0x80011406,
    0x4a0602ff, 0x00002400, 0x4a0804ff, 0x00004800, 0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff,
    0x00002400, 0xdd598000, 0x14080a03, 0xbf8c3f70, 0xe0741000, 0x80011406, 0x4a0602ff, 0x00002600,
    0x4a0804ff, 0x00004c00, 0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff, 0x00002600, 0xdd5d8000,
    0x14080a03, 0xbf8c3f70, 0xe0741000, 0x80011406, 0x4a0602ff, 0x00002800, 0x4a0804ff, 0x00005000,
    0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff, 0x00002800, 0xdd618000, 0x14080a03, 0xbf8c3f70,
    0xe0741000, 0x80011406, 0x4a0602ff, 0x00002a00, 0x4a0804ff, 0x00005400, 0xe0381000, 0x80000a04,
    0xbf8c3f70, 0x4a0c0aff, 0x00002a00, 0xdd658000, 0x14080a03, 0xbf8c3f70, 0xe0741000, 0x80011406,
    0x4a0602ff, 0x00002c00, 0x4a0804ff, 0x00005800, 0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff,
    0x00002c00, 0xdd698000, 0x14080a03, 0xbf8c3f70, 0xe0741000, 0x80011406, 0x4a0602ff, 0x00002e00,
    0x4a0804ff, 0x00005c00, 0xe0381000, 0x80000a04, 0xbf8c3f70, 0x4a0c0aff, 0x00002e00, 0xdd6d8000,
    0x14080a03, 0xbf8c3f70, 0xe0741000, 0x80011406, 0x4a0602ff, 0x00003000, 0x7e2802ff, 0x05e471e1,
    0x4a140081, 0xdcc88000, 0x00080a03, 0xbf8c3f70, 0xd70f6a1e, 0x02020608, 0x7e3e0209, 0x503e3e80,
    0xd70f6a1e, 0x02023cff, 0x00000200, 0x503e3e80, 0x34140088, 0xdced0000, 0x147d0a1e, 0xbf8c0070,
    0x4a0c0aff, 0x00003000, 0xe0701000, 0x80011406, 0x7e0602ff, 0x00003400, 0x4a1414ff, 0x00000100,
    0xdcc98004, 0x14080a03, 0xbf8c3f70, 0x4a0c0aff, 0x00003200, 0xe0701000, 0x80011406, 0x7da80090,
    0x4a0602ff, 0x00003600, 0x7e1402ff, 0x000000f0, 0xdce88000, 0x00080a03, 0xbf8c3f70, 0xbf810000
};

struct Row {
    std::uint32_t op;
    std::uint64_t memory;
    std::uint64_t data;
    std::uint64_t comparator;
    std::uint64_t returned;
    std::uint64_t final;
};

constexpr std::array<Row, 384> Rows{{
    {0, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {0, 0x1ull, 0x80000000ull, 0xf9677975ull, 0x1ull, 0x80000000ull},
    {0, 0x2ull, 0xffffffffull, 0x2ull, 0x2ull, 0xffffffffull},
    {0, 0x7fffffffull, 0x0ull, 0xfc5039f7ull, 0x7fffffffull, 0x0ull},
    {0, 0x80000000ull, 0x7fffffffull, 0x80000000ull, 0x80000000ull, 0x7fffffffull},
    {0, 0x80000001ull, 0xfffffffeull, 0x1098ad65ull, 0x80000001ull, 0xfffffffeull},
    {0, 0xfffffffeull, 0xdeadbeefull, 0xfffffffeull, 0xfffffffeull, 0xdeadbeefull},
    {0, 0xffffffffull, 0x2ull, 0x19211eull, 0xffffffffull, 0x2ull},
    {0, 0x12345678ull, 0x80000001ull, 0x12345678ull, 0x12345678ull, 0x80000001ull},
    {0, 0xdeadbeefull, 0x12345678ull, 0xcdbc2be9ull, 0xdeadbeefull, 0x12345678ull},
    {0, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {0, 0x1ull, 0x80000000ull, 0xa3639922ull, 0x1ull, 0x80000000ull},
    {0, 0x46e9a894ull, 0xd107eea9ull, 0x46e9a894ull, 0x46e9a894ull, 0xd107eea9ull},
    {0, 0xbc50bcc8ull, 0x58ffc3ecull, 0x4cd661ull, 0xbc50bcc8ull, 0x58ffc3ecull},
    {0, 0xb9093c7eull, 0x665932a4ull, 0xb9093c7eull, 0xb9093c7eull, 0x665932a4ull},
    {0, 0xd88449d9ull, 0xfe358db5ull, 0xba80659full, 0xd88449d9ull, 0xfe358db5ull},
    {1, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {1, 0x1ull, 0x80000000ull, 0x109949f9ull, 0x1ull, 0x1ull},
    {1, 0x2ull, 0xffffffffull, 0x2ull, 0x2ull, 0xffffffffull},
    {1, 0x7fffffffull, 0x0ull, 0x22d348c2ull, 0x7fffffffull, 0x7fffffffull},
    {1, 0x80000000ull, 0x7fffffffull, 0x80000000ull, 0x80000000ull, 0x7fffffffull},
    {1, 0x80000001ull, 0xfffffffeull, 0x42872e08ull, 0x80000001ull, 0x80000001ull},
    {1, 0xfffffffeull, 0xdeadbeefull, 0xfffffffeull, 0xfffffffeull, 0xdeadbeefull},
    {1, 0xffffffffull, 0x2ull, 0x675c2152ull, 0xffffffffull, 0xffffffffull},
    {1, 0x12345678ull, 0x80000001ull, 0x12345678ull, 0x12345678ull, 0x80000001ull},
    {1, 0xdeadbeefull, 0x12345678ull, 0x2f1ed38ull, 0xdeadbeefull, 0xdeadbeefull},
    {1, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {1, 0x1ull, 0x80000000ull, 0xd6be8bc1ull, 0x1ull, 0x1ull},
    {1, 0x283d5502ull, 0xfc565b67ull, 0x283d5502ull, 0x283d5502ull, 0xfc565b67ull},
    {1, 0xe503313aull, 0x1722ca72ull, 0xdd44c959ull, 0xe503313aull, 0xe503313aull},
    {1, 0x2a841e2ull, 0x52a450e2ull, 0x2a841e2ull, 0x2a841e2ull, 0x52a450e2ull},
    {1, 0xcc440ac9ull, 0x46313f70ull, 0xee4a57bbull, 0xcc440ac9ull, 0xcc440ac9ull},
    {2, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {2, 0x1ull, 0x80000000ull, 0x452f2ffcull, 0x1ull, 0x80000001ull},
    {2, 0x2ull, 0xffffffffull, 0x2ull, 0x2ull, 0x1ull},
    {2, 0x7fffffffull, 0x0ull, 0xf5091963ull, 0x7fffffffull, 0x7fffffffull},
    {2, 0x80000000ull, 0x7fffffffull, 0x80000000ull, 0x80000000ull, 0xffffffffull},
    {2, 0x80000001ull, 0xfffffffeull, 0xeb165c4ull, 0x80000001ull, 0x7fffffffull},
    {2, 0xfffffffeull, 0xdeadbeefull, 0xfffffffeull, 0xfffffffeull, 0xdeadbeedull},
    {2, 0xffffffffull, 0x2ull, 0x1a438e52ull, 0xffffffffull, 0x1ull},
    {2, 0x12345678ull, 0x80000001ull, 0x12345678ull, 0x12345678ull, 0x92345679ull},
    {2, 0xdeadbeefull, 0x12345678ull, 0x64638c03ull, 0xdeadbeefull, 0xf0e21567ull},
    {2, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {2, 0x1ull, 0x80000000ull, 0xe6d2996dull, 0x1ull, 0x80000001ull},
    {2, 0xa9bdd75aull, 0x70734273ull, 0xa9bdd75aull, 0xa9bdd75aull, 0x1a3119cdull},
    {2, 0x8aa692d0ull, 0x74d3a033ull, 0x5a7fd942ull, 0x8aa692d0ull, 0xff7a3303ull},
    {2, 0x201fe536ull, 0xf8ddb196ull, 0x201fe536ull, 0x201fe536ull, 0x18fd96ccull},
    {2, 0x15f79627ull, 0xb2346d27ull, 0x8a6b642dull, 0x15f79627ull, 0xc82c034eull},
    {3, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0xffffffffull},
    {3, 0x1ull, 0x80000000ull, 0x69639719ull, 0x1ull, 0x80000001ull},
    {3, 0x2ull, 0xffffffffull, 0x2ull, 0x2ull, 0x3ull},
    {3, 0x7fffffffull, 0x0ull, 0x89e34c92ull, 0x7fffffffull, 0x7fffffffull},
    {3, 0x80000000ull, 0x7fffffffull, 0x80000000ull, 0x80000000ull, 0x1ull},
    {3, 0x80000001ull, 0xfffffffeull, 0x73229920ull, 0x80000001ull, 0x80000003ull},
    {3, 0xfffffffeull, 0xdeadbeefull, 0xfffffffeull, 0xfffffffeull, 0x2152410full},
    {3, 0xffffffffull, 0x2ull, 0x66310986ull, 0xffffffffull, 0xfffffffdull},
    {3, 0x12345678ull, 0x80000001ull, 0x12345678ull, 0x12345678ull, 0x92345677ull},
    {3, 0xdeadbeefull, 0x12345678ull, 0x8fa4a357ull, 0xdeadbeefull, 0xcc796877ull},
    {3, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0xffffffffull},
    {3, 0x1ull, 0x80000000ull, 0x64bdad88ull, 0x1ull, 0x80000001ull},
    {3, 0xd5b148e7ull, 0x91d4c68ull, 0xd5b148e7ull, 0xd5b148e7ull, 0xcc93fc7full},
    {3, 0xb6cd96e4ull, 0xa145e36full, 0x6efd9be0ull, 0xb6cd96e4ull, 0x1587b375ull},
    {3, 0xf123997cull, 0x12d3d18aull, 0xf123997cull, 0xf123997cull, 0xde4fc7f2ull},
    {3, 0x62bfff89ull, 0x2106fea7ull, 0x22db0452ull, 0x62bfff89ull, 0x41b900e2ull},
    {4, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x0ull},
    {4, 0x1ull, 0x80000000ull, 0xdbbb69abull, 0x1ull, 0x80000000ull},
    {4, 0x2ull, 0xffffffffull, 0x2ull, 0x2ull, 0xffffffffull},
    {4, 0x7fffffffull, 0x0ull, 0xdffce4e3ull, 0x7fffffffull, 0x0ull},
    {4, 0x80000000ull, 0x7fffffffull, 0x80000000ull, 0x80000000ull, 0x80000000ull},
    {4, 0x80000001ull, 0xfffffffeull, 0x77ac152aull, 0x80000001ull, 0x80000001ull},
    {4, 0xfffffffeull, 0xdeadbeefull, 0xfffffffeull, 0xfffffffeull, 0xdeadbeefull},
    {4, 0xffffffffull, 0x2ull, 0x24880f60ull, 0xffffffffull, 0xffffffffull},
    {4, 0x12345678ull, 0x80000001ull, 0x12345678ull, 0x12345678ull, 0x80000001ull},
    {4, 0xdeadbeefull, 0x12345678ull, 0x6adc5e6full, 0xdeadbeefull, 0xdeadbeefull},
    {4, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x0ull},
    {4, 0x1ull, 0x80000000ull, 0x5c5e447eull, 0x1ull, 0x80000000ull},
    {4, 0xbc193f27ull, 0x5b2c838cull, 0xbc193f27ull, 0xbc193f27ull, 0xbc193f27ull},
    {4, 0xa69c5ce5ull, 0xbf61575eull, 0x341a640bull, 0xa69c5ce5ull, 0xa69c5ce5ull},
    {4, 0x1836851cull, 0x12fef585ull, 0x1836851cull, 0x1836851cull, 0x12fef585ull},
    {4, 0xaa5c538dull, 0x618e48b9ull, 0x56002b6cull, 0xaa5c538dull, 0xaa5c538dull},
    {5, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x0ull},
    {5, 0x1ull, 0x80000000ull, 0x21d3075eull, 0x1ull, 0x1ull},
    {5, 0x2ull, 0xffffffffull, 0x2ull, 0x2ull, 0x2ull},
    {5, 0x7fffffffull, 0x0ull, 0x16b60422ull, 0x7fffffffull, 0x0ull},
    {5, 0x80000000ull, 0x7fffffffull, 0x80000000ull, 0x80000000ull, 0x7fffffffull},
    {5, 0x80000001ull, 0xfffffffeull, 0xfd31cfd7ull, 0x80000001ull, 0x80000001ull},
    {5, 0xfffffffeull, 0xdeadbeefull, 0xfffffffeull, 0xfffffffeull, 0xdeadbeefull},
    {5, 0xffffffffull, 0x2ull, 0x6c5db104ull, 0xffffffffull, 0x2ull},
    {5, 0x12345678ull, 0x80000001ull, 0x12345678ull, 0x12345678ull, 0x12345678ull},
    {5, 0xdeadbeefull, 0x12345678ull, 0xca2b6cbeull, 0xdeadbeefull, 0x12345678ull},
    {5, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x0ull},
    {5, 0x1ull, 0x80000000ull, 0xdc32d1aaull, 0x1ull, 0x1ull},
    {5, 0xfd11be0eull, 0x30eaba72ull, 0xfd11be0eull, 0xfd11be0eull, 0x30eaba72ull},
    {5, 0x4f696046ull, 0xed71a68aull, 0x59f1d18dull, 0x4f696046ull, 0x4f696046ull},
    {5, 0x2e192541ull, 0x1f6c7f26ull, 0x2e192541ull, 0x2e192541ull, 0x1f6c7f26ull},
    {5, 0x7a1ff8aeull, 0xdae2a240ull, 0xd604dd68ull, 0x7a1ff8aeull, 0x7a1ff8aeull},
    {6, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {6, 0x1ull, 0x80000000ull, 0xadf8324cull, 0x1ull, 0x1ull},
    {6, 0x2ull, 0xffffffffull, 0x2ull, 0x2ull, 0x2ull},
    {6, 0x7fffffffull, 0x0ull, 0x9f75da08ull, 0x7fffffffull, 0x7fffffffull},
    {6, 0x80000000ull, 0x7fffffffull, 0x80000000ull, 0x80000000ull, 0x7fffffffull},
    {6, 0x80000001ull, 0xfffffffeull, 0xcd48b9bdull, 0x80000001ull, 0xfffffffeull},
    {6, 0xfffffffeull, 0xdeadbeefull, 0xfffffffeull, 0xfffffffeull, 0xfffffffeull},
    {6, 0xffffffffull, 0x2ull, 0x40591809ull, 0xffffffffull, 0x2ull},
    {6, 0x12345678ull, 0x80000001ull, 0x12345678ull, 0x12345678ull, 0x12345678ull},
    {6, 0xdeadbeefull, 0x12345678ull, 0x951a2694ull, 0xdeadbeefull, 0x12345678ull},
    {6, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {6, 0x1ull, 0x80000000ull, 0x55ff16b5ull, 0x1ull, 0x1ull},
    {6, 0xadb9fc15ull, 0x9263cee2ull, 0xadb9fc15ull, 0xadb9fc15ull, 0xadb9fc15ull},
    {6, 0xa055a0b2ull, 0xe06373c3ull, 0x406ffc43ull, 0xa055a0b2ull, 0xe06373c3ull},
    {6, 0xe442a859ull, 0xbcdeb639ull, 0xe442a859ull, 0xe442a859ull, 0xe442a859ull},
    {6, 0x238c6bf4ull, 0xfe8fedc6ull, 0x771aaf91ull, 0x238c6bf4ull, 0x238c6bf4ull},
    {7, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {7, 0x1ull, 0x80000000ull, 0x237244c9ull, 0x1ull, 0x80000000ull},
    {7, 0x2ull, 0xffffffffull, 0x2ull, 0x2ull, 0xffffffffull},
    {7, 0x7fffffffull, 0x0ull, 0xf62e5e4full, 0x7fffffffull, 0x7fffffffull},
    {7, 0x80000000ull, 0x7fffffffull, 0x80000000ull, 0x80000000ull, 0x80000000ull},
    {7, 0x80000001ull, 0xfffffffeull, 0x90bfe553ull, 0x80000001ull, 0xfffffffeull},
    {7, 0xfffffffeull, 0xdeadbeefull, 0xfffffffeull, 0xfffffffeull, 0xfffffffeull},
    {7, 0xffffffffull, 0x2ull, 0x44b4cc99ull, 0xffffffffull, 0xffffffffull},
    {7, 0x12345678ull, 0x80000001ull, 0x12345678ull, 0x12345678ull, 0x80000001ull},
    {7, 0xdeadbeefull, 0x12345678ull, 0x99faa521ull, 0xdeadbeefull, 0xdeadbeefull},
    {7, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {7, 0x1ull, 0x80000000ull, 0x5136e276ull, 0x1ull, 0x80000000ull},
    {7, 0x64a7abb4ull, 0x146d75bcull, 0x64a7abb4ull, 0x64a7abb4ull, 0x64a7abb4ull},
    {7, 0x2ae5498eull, 0x63a37ae1ull, 0xde40e42bull, 0x2ae5498eull, 0x63a37ae1ull},
    {7, 0x99206560ull, 0x2a0494fdull, 0x99206560ull, 0x99206560ull, 0x99206560ull},
    {7, 0x634e77acull, 0xbfe9ef2bull, 0x70824c54ull, 0x634e77acull, 0xbfe9ef2bull},
    {8, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x0ull},
    {8, 0x1ull, 0x80000000ull, 0x5b03f923ull, 0x1ull, 0x0ull},
    {8, 0x2ull, 0xffffffffull, 0x2ull, 0x2ull, 0x2ull},
    {8, 0x7fffffffull, 0x0ull, 0x32c25127ull, 0x7fffffffull, 0x0ull},
    {8, 0x80000000ull, 0x7fffffffull, 0x80000000ull, 0x80000000ull, 0x0ull},
    {8, 0x80000001ull, 0xfffffffeull, 0x1197fd88ull, 0x80000001ull, 0x80000000ull},
    {8, 0xfffffffeull, 0xdeadbeefull, 0xfffffffeull, 0xfffffffeull, 0xdeadbeeeull},
    {8, 0xffffffffull, 0x2ull, 0x7aa29c55ull, 0xffffffffull, 0x2ull},
    {8, 0x12345678ull, 0x80000001ull, 0x12345678ull, 0x12345678ull, 0x0ull},
    {8, 0xdeadbeefull, 0x12345678ull, 0x4dc23c90ull, 0xdeadbeefull, 0x12241668ull},
    {8, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x0ull},
    {8, 0x1ull, 0x80000000ull, 0xfa62c0f8ull, 0x1ull, 0x0ull},
    {8, 0xfd7537d1ull, 0x72643b5aull, 0xfd7537d1ull, 0xfd7537d1ull, 0x70643350ull},
    {8, 0xd8dd1ff5ull, 0x8007f94cull, 0x1c7c7b29ull, 0xd8dd1ff5ull, 0x80051944ull},
    {8, 0xadaa9f69ull, 0x542db28dull, 0xadaa9f69ull, 0xadaa9f69ull, 0x4289209ull},
    {8, 0xeba8ad6dull, 0xaf3f49e4ull, 0xbf7eef4eull, 0xeba8ad6dull, 0xab280964ull},
    {9, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {9, 0x1ull, 0x80000000ull, 0xd2dc3da8ull, 0x1ull, 0x80000001ull},
    {9, 0x2ull, 0xffffffffull, 0x2ull, 0x2ull, 0xffffffffull},
    {9, 0x7fffffffull, 0x0ull, 0xcbcf2e9cull, 0x7fffffffull, 0x7fffffffull},
    {9, 0x80000000ull, 0x7fffffffull, 0x80000000ull, 0x80000000ull, 0xffffffffull},
    {9, 0x80000001ull, 0xfffffffeull, 0xf1616b9dull, 0x80000001ull, 0xffffffffull},
    {9, 0xfffffffeull, 0xdeadbeefull, 0xfffffffeull, 0xfffffffeull, 0xffffffffull},
    {9, 0xffffffffull, 0x2ull, 0xaf3ff23eull, 0xffffffffull, 0xffffffffull},
    {9, 0x12345678ull, 0x80000001ull, 0x12345678ull, 0x12345678ull, 0x92345679ull},
    {9, 0xdeadbeefull, 0x12345678ull, 0xd2b04560ull, 0xdeadbeefull, 0xdebdfeffull},
    {9, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {9, 0x1ull, 0x80000000ull, 0xef07f743ull, 0x1ull, 0x80000001ull},
    {9, 0x24c74e5ull, 0x5084dec1ull, 0x24c74e5ull, 0x24c74e5ull, 0x52ccfee5ull},
    {9, 0x8c6705a2ull, 0x1e525d48ull, 0xab05ab28ull, 0x8c6705a2ull, 0x9e775deaull},
    {9, 0xa36a05e5ull, 0x3a793efcull, 0xa36a05e5ull, 0xa36a05e5ull, 0xbb7b3ffdull},
    {9, 0xd5f31646ull, 0xa296c3e7ull, 0x213632ddull, 0xd5f31646ull, 0xf7f7d7e7ull},
    {10, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {10, 0x1ull, 0x80000000ull, 0x11e1dbaaull, 0x1ull, 0x80000001ull},
    {10, 0x2ull, 0xffffffffull, 0x2ull, 0x2ull, 0xfffffffdull},
    {10, 0x7fffffffull, 0x0ull, 0x2cd60e1aull, 0x7fffffffull, 0x7fffffffull},
    {10, 0x80000000ull, 0x7fffffffull, 0x80000000ull, 0x80000000ull, 0xffffffffull},
    {10, 0x80000001ull, 0xfffffffeull, 0x1f025ad0ull, 0x80000001ull, 0x7fffffffull},
    {10, 0xfffffffeull, 0xdeadbeefull, 0xfffffffeull, 0xfffffffeull, 0x21524111ull},
    {10, 0xffffffffull, 0x2ull, 0x5d9eb5b7ull, 0xffffffffull, 0xfffffffdull},
    {10, 0x12345678ull, 0x80000001ull, 0x12345678ull, 0x12345678ull, 0x92345679ull},
    {10, 0xdeadbeefull, 0x12345678ull, 0xf167a808ull, 0xdeadbeefull, 0xcc99e897ull},
    {10, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {10, 0x1ull, 0x80000000ull, 0x9f4e6575ull, 0x1ull, 0x80000001ull},
    {10, 0x8bd3aa1cull, 0x44b01dcdull, 0x8bd3aa1cull, 0x8bd3aa1cull, 0xcf63b7d1ull},
    {10, 0xc2c143f7ull, 0x51bc9443ull, 0xd65e2124ull, 0xc2c143f7ull, 0x937dd7b4ull},
    {10, 0xb0cb286eull, 0xec5194a4ull, 0xb0cb286eull, 0xb0cb286eull, 0x5c9abccaull},
    {10, 0xe17a51ecull, 0xe0bfc82aull, 0x44dde368ull, 0xe17a51ecull, 0x1c599c6ull},
    {11, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {11, 0x1ull, 0x80000000ull, 0x20d071afull, 0x1ull, 0x2ull},
    {11, 0x2ull, 0xffffffffull, 0x2ull, 0x2ull, 0x3ull},
    {11, 0x7fffffffull, 0x0ull, 0x7a347eebull, 0x7fffffffull, 0x0ull},
    {11, 0x80000000ull, 0x7fffffffull, 0x80000000ull, 0x80000000ull, 0x0ull},
    {11, 0x80000001ull, 0xfffffffeull, 0x56e21801ull, 0x80000001ull, 0x80000002ull},
    {11, 0xfffffffeull, 0xdeadbeefull, 0xfffffffeull, 0xfffffffeull, 0x0ull},
    {11, 0xffffffffull, 0x2ull, 0x7a390ac1ull, 0xffffffffull, 0x0ull},
    {11, 0x12345678ull, 0x80000001ull, 0x12345678ull, 0x12345678ull, 0x12345679ull},
    {11, 0xdeadbeefull, 0x12345678ull, 0xe457cd1full, 0xdeadbeefull, 0x0ull},
    {11, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {11, 0x1ull, 0x80000000ull, 0x460a51e2ull, 0x1ull, 0x2ull},
    {11, 0xe3202a2aull, 0xe3202a2bull, 0xe3202a2aull, 0xe3202a2aull, 0xe3202a2bull},
    {11, 0x9ec2949ull, 0x9ec2949ull, 0x947613e8ull, 0x9ec2949ull, 0x0ull},
    {11, 0x8f8118afull, 0x8f8118b0ull, 0x8f8118afull, 0x8f8118afull, 0x8f8118b0ull},
    {11, 0x761757b6ull, 0x761757b6ull, 0x8be15c75ull, 0x761757b6ull, 0x0ull},
    {12, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {12, 0x1ull, 0x80000000ull, 0x7c5a8c15ull, 0x1ull, 0x0ull},
    {12, 0x2ull, 0xffffffffull, 0x2ull, 0x2ull, 0x1ull},
    {12, 0x7fffffffull, 0x0ull, 0x29769614ull, 0x7fffffffull, 0x0ull},
    {12, 0x80000000ull, 0x7fffffffull, 0x80000000ull, 0x80000000ull, 0x7fffffffull},
    {12, 0x80000001ull, 0xfffffffeull, 0x537a54d3ull, 0x80000001ull, 0x80000000ull},
    {12, 0xfffffffeull, 0xdeadbeefull, 0xfffffffeull, 0xfffffffeull, 0xdeadbeefull},
    {12, 0xffffffffull, 0x2ull, 0x6add45d9ull, 0xffffffffull, 0x2ull},
    {12, 0x12345678ull, 0x80000001ull, 0x12345678ull, 0x12345678ull, 0x12345677ull},
    {12, 0xdeadbeefull, 0x12345678ull, 0x978c0812ull, 0xdeadbeefull, 0x12345678ull},
    {12, 0x0ull, 0x1ull, 0x0ull, 0x0ull, 0x1ull},
    {12, 0x1ull, 0x80000000ull, 0xf67e5e26ull, 0x1ull, 0x0ull},
    {12, 0xebea6cf1ull, 0xebea6cf2ull, 0xebea6cf1ull, 0xebea6cf1ull, 0xebea6cf0ull},
    {12, 0xf9a9360ull, 0xf9a9360ull, 0xf85b81cdull, 0xf9a9360ull, 0xf9a935full},
    {12, 0x6e723d5cull, 0x6e723d5dull, 0x6e723d5cull, 0x6e723d5cull, 0x6e723d5bull},
    {12, 0x1f609b4full, 0x1f609b4full, 0x69d177f8ull, 0x1f609b4full, 0x1f609b4eull},
    {13, 0x0ull, 0x7fffffffffffffffull, 0x0ull, 0x0ull, 0x7fffffffffffffffull},
    {13, 0x1ull, 0xfffffffeffffffffull, 0x3ull, 0x1ull, 0xfffffffeffffffffull},
    {13, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x7fffffffffffffffull, 0x8000000000000000ull},
    {13, 0x8000000000000000ull, 0x123456789abcdef0ull, 0xb210ee8d643f54e6ull, 0x8000000000000000ull, 0x123456789abcdef0ull},
    {13, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull},
    {13, 0xffffffffull, 0x0ull, 0xffffffdfull, 0xffffffffull, 0x0ull},
    {13, 0x100000000ull, 0xffffffffull, 0x100000000ull, 0x100000000ull, 0xffffffffull},
    {13, 0xfffffffeffffffffull, 0x1ull, 0xed5fb21be38948d7ull, 0xfffffffeffffffffull, 0x1ull},
    {13, 0x123456789abcdef0ull, 0x100000000ull, 0x123456789abcdef0ull, 0x123456789abcdef0ull, 0x100000000ull},
    {13, 0x0ull, 0x7fffffffffffffffull, 0x200ull, 0x0ull, 0x7fffffffffffffffull},
    {13, 0x1ull, 0xfffffffeffffffffull, 0x1ull, 0x1ull, 0xfffffffeffffffffull},
    {13, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xb346daefa27ab170ull, 0x7fffffffffffffffull, 0x8000000000000000ull},
    {13, 0x53bb9629497dfb3full, 0xa4abfed10ad2a242ull, 0x53bb9629497dfb3full, 0x53bb9629497dfb3full, 0xa4abfed10ad2a242ull},
    {13, 0x6c12816965391baaull, 0xce7cd7b44b45eaf3ull, 0x6c12816967391baaull, 0x6c12816965391baaull, 0xce7cd7b44b45eaf3ull},
    {13, 0xdba996ccffffffffull, 0x10bbe64100000001ull, 0xdba996ccffffffffull, 0xdba996ccffffffffull, 0x10bbe64100000001ull},
    {13, 0x5a7c138cffffffffull, 0x9c15fed00000001ull, 0x5a7c138cdfffffffull, 0x5a7c138cffffffffull, 0x9c15fed00000001ull},
    {14, 0x0ull, 0x7fffffffffffffffull, 0x0ull, 0x0ull, 0x7fffffffffffffffull},
    {14, 0x1ull, 0xfffffffeffffffffull, 0x3ull, 0x1ull, 0x1ull},
    {14, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x7fffffffffffffffull, 0x8000000000000000ull},
    {14, 0x8000000000000000ull, 0x123456789abcdef0ull, 0x166c48c6e9c1b01cull, 0x8000000000000000ull, 0x8000000000000000ull},
    {14, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull},
    {14, 0xffffffffull, 0x0ull, 0xffffffdfull, 0xffffffffull, 0xffffffffull},
    {14, 0x100000000ull, 0xffffffffull, 0x100000000ull, 0x100000000ull, 0xffffffffull},
    {14, 0xfffffffeffffffffull, 0x1ull, 0x866b08e67ea9d02cull, 0xfffffffeffffffffull, 0xfffffffeffffffffull},
    {14, 0x123456789abcdef0ull, 0x100000000ull, 0x123456789abcdef0ull, 0x123456789abcdef0ull, 0x100000000ull},
    {14, 0x0ull, 0x7fffffffffffffffull, 0x200ull, 0x0ull, 0x0ull},
    {14, 0x1ull, 0xfffffffeffffffffull, 0x1ull, 0x1ull, 0xfffffffeffffffffull},
    {14, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xa54604e5b9b1311full, 0x7fffffffffffffffull, 0x7fffffffffffffffull},
    {14, 0xb9a56d03ead2890full, 0x242f2f3b525af861ull, 0xb9a56d03ead2890full, 0xb9a56d03ead2890full, 0x242f2f3b525af861ull},
    {14, 0x65c783e7e26c0130ull, 0x19a0106761c2f186ull, 0x65c783e7e06c0130ull, 0x65c783e7e26c0130ull, 0x65c783e7e26c0130ull},
    {14, 0x7bebc240ffffffffull, 0x4da1ba4b00000001ull, 0x7bebc240ffffffffull, 0x7bebc240ffffffffull, 0x4da1ba4b00000001ull},
    {14, 0x57e3536bffffffffull, 0xd8d033bd00000001ull, 0x57e3536bdfffffffull, 0x57e3536bffffffffull, 0x57e3536bffffffffull},
    {15, 0x0ull, 0x7fffffffffffffffull, 0x0ull, 0x0ull, 0x7fffffffffffffffull},
    {15, 0x1ull, 0xfffffffeffffffffull, 0x3ull, 0x1ull, 0xffffffff00000000ull},
    {15, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x7fffffffffffffffull, 0xffffffffffffffffull},
    {15, 0x8000000000000000ull, 0x123456789abcdef0ull, 0xd4db9193123f6ebaull, 0x8000000000000000ull, 0x923456789abcdef0ull},
    {15, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xfffffffffffffffeull},
    {15, 0xffffffffull, 0x0ull, 0xffffffdfull, 0xffffffffull, 0xffffffffull},
    {15, 0x100000000ull, 0xffffffffull, 0x100000000ull, 0x100000000ull, 0x1ffffffffull},
    {15, 0xfffffffeffffffffull, 0x1ull, 0x25fd502c82facbc0ull, 0xfffffffeffffffffull, 0xffffffff00000000ull},
    {15, 0x123456789abcdef0ull, 0x100000000ull, 0x123456789abcdef0ull, 0x123456789abcdef0ull, 0x123456799abcdef0ull},
    {15, 0x0ull, 0x7fffffffffffffffull, 0x200ull, 0x0ull, 0x7fffffffffffffffull},
    {15, 0x1ull, 0xfffffffeffffffffull, 0x1ull, 0x1ull, 0xffffffff00000000ull},
    {15, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xf9124f0e4aae24c5ull, 0x7fffffffffffffffull, 0xffffffffffffffffull},
    {15, 0xc3e86469baffc1dull, 0xc22f0e9d6d69338dull, 0xc3e86469baffc1dull, 0xc3e86469baffc1dull, 0xce6d94e409192faaull},
    {15, 0xe499e6544c91e53ull, 0x4647904bf55b8053ull, 0xe499e6546c91e53ull, 0xe499e6544c91e53ull, 0x54912eb13a249ea6ull},
    {15, 0x7a4a48e2ffffffffull, 0x7ccc4b5300000001ull, 0x7a4a48e2ffffffffull, 0x7a4a48e2ffffffffull, 0xf716943600000000ull},
    {15, 0xa925179dffffffffull, 0xb2b1ac1800000001ull, 0xa925179ddfffffffull, 0xa925179dffffffffull, 0x5bd6c3b600000000ull},
    {16, 0x0ull, 0x7fffffffffffffffull, 0x0ull, 0x0ull, 0x8000000000000001ull},
    {16, 0x1ull, 0xfffffffeffffffffull, 0x3ull, 0x1ull, 0x100000002ull},
    {16, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x7fffffffffffffffull, 0xffffffffffffffffull},
    {16, 0x8000000000000000ull, 0x123456789abcdef0ull, 0x6835bf86a066a97eull, 0x8000000000000000ull, 0x6dcba98765432110ull},
    {16, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0x0ull},
    {16, 0xffffffffull, 0x0ull, 0xffffffdfull, 0xffffffffull, 0xffffffffull},
    {16, 0x100000000ull, 0xffffffffull, 0x100000000ull, 0x100000000ull, 0x1ull},
    {16, 0xfffffffeffffffffull, 0x1ull, 0xbcbca7d9810b83c1ull, 0xfffffffeffffffffull, 0xfffffffefffffffeull},
    {16, 0x123456789abcdef0ull, 0x100000000ull, 0x123456789abcdef0ull, 0x123456789abcdef0ull, 0x123456779abcdef0ull},
    {16, 0x0ull, 0x7fffffffffffffffull, 0x200ull, 0x0ull, 0x8000000000000001ull},
    {16, 0x1ull, 0xfffffffeffffffffull, 0x1ull, 0x1ull, 0x100000002ull},
    {16, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x663c553a9a16e35ull, 0x7fffffffffffffffull, 0xffffffffffffffffull},
    {16, 0x63fbf4e8e453a4c5ull, 0xb7a15652045c43f6ull, 0x63fbf4e8e453a4c5ull, 0x63fbf4e8e453a4c5ull, 0xac5a9e96dff760cfull},
    {16, 0x1603b25645bdd8aaull, 0x1f87b96dd545e1f1ull, 0x1603b25647bdd8aaull, 0x1603b25645bdd8aaull, 0xf67bf8e87077f6b9ull},
    {16, 0x757fd168ffffffffull, 0x2356488500000001ull, 0x757fd168ffffffffull, 0x757fd168ffffffffull, 0x522988e3fffffffeull},
    {16, 0x752b2a35ffffffffull, 0x869e617c00000001ull, 0x752b2a35dfffffffull, 0x752b2a35ffffffffull, 0xee8cc8b9fffffffeull},
    {17, 0x0ull, 0x7fffffffffffffffull, 0x0ull, 0x0ull, 0x0ull},
    {17, 0x1ull, 0xfffffffeffffffffull, 0x3ull, 0x1ull, 0xfffffffeffffffffull},
    {17, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x7fffffffffffffffull, 0x8000000000000000ull},
    {17, 0x8000000000000000ull, 0x123456789abcdef0ull, 0xd286c038892fd9d4ull, 0x8000000000000000ull, 0x8000000000000000ull},
    {17, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull},
    {17, 0xffffffffull, 0x0ull, 0xffffffdfull, 0xffffffffull, 0x0ull},
    {17, 0x100000000ull, 0xffffffffull, 0x100000000ull, 0x100000000ull, 0xffffffffull},
    {17, 0xfffffffeffffffffull, 0x1ull, 0x80b34c5cb5907e40ull, 0xfffffffeffffffffull, 0xfffffffeffffffffull},
    {17, 0x123456789abcdef0ull, 0x100000000ull, 0x123456789abcdef0ull, 0x123456789abcdef0ull, 0x100000000ull},
    {17, 0x0ull, 0x7fffffffffffffffull, 0x200ull, 0x0ull, 0x0ull},
    {17, 0x1ull, 0xfffffffeffffffffull, 0x1ull, 0x1ull, 0xfffffffeffffffffull},
    {17, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xe7cd1f2260293767ull, 0x7fffffffffffffffull, 0x8000000000000000ull},
    {17, 0x8372abbda96ba8eeull, 0x5e71be928ba29363ull, 0x8372abbda96ba8eeull, 0x8372abbda96ba8eeull, 0x8372abbda96ba8eeull},
    {17, 0xce77c3cdcc677910ull, 0xdf44f6f934647f0eull, 0xce77c3cdce677910ull, 0xce77c3cdcc677910ull, 0xce77c3cdcc677910ull},
    {17, 0x9b3edbb1ffffffffull, 0xf4f16cf100000001ull, 0x9b3edbb1ffffffffull, 0x9b3edbb1ffffffffull, 0x9b3edbb1ffffffffull},
    {17, 0xadf3fbf0ffffffffull, 0x8b29592900000001ull, 0xadf3fbf0dfffffffull, 0xadf3fbf0ffffffffull, 0x8b29592900000001ull},
    {18, 0x0ull, 0x7fffffffffffffffull, 0x0ull, 0x0ull, 0x0ull},
    {18, 0x1ull, 0xfffffffeffffffffull, 0x3ull, 0x1ull, 0x1ull},
    {18, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x7fffffffffffffffull, 0x7fffffffffffffffull},
    {18, 0x8000000000000000ull, 0x123456789abcdef0ull, 0xff63cf82d01938e9ull, 0x8000000000000000ull, 0x123456789abcdef0ull},
    {18, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull},
    {18, 0xffffffffull, 0x0ull, 0xffffffdfull, 0xffffffffull, 0x0ull},
    {18, 0x100000000ull, 0xffffffffull, 0x100000000ull, 0x100000000ull, 0xffffffffull},
    {18, 0xfffffffeffffffffull, 0x1ull, 0x2a88358605f1254ull, 0xfffffffeffffffffull, 0x1ull},
    {18, 0x123456789abcdef0ull, 0x100000000ull, 0x123456789abcdef0ull, 0x123456789abcdef0ull, 0x100000000ull},
    {18, 0x0ull, 0x7fffffffffffffffull, 0x200ull, 0x0ull, 0x0ull},
    {18, 0x1ull, 0xfffffffeffffffffull, 0x1ull, 0x1ull, 0x1ull},
    {18, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xf078418740b4f6d3ull, 0x7fffffffffffffffull, 0x7fffffffffffffffull},
    {18, 0x8d4a240ceafb92f2ull, 0xd789c424b5282b1dull, 0x8d4a240ceafb92f2ull, 0x8d4a240ceafb92f2ull, 0x8d4a240ceafb92f2ull},
    {18, 0x64b3620ed7a9434full, 0x930b9894c3741be0ull, 0x64b3620ed5a9434full, 0x64b3620ed7a9434full, 0x64b3620ed7a9434full},
    {18, 0x62a12f63ffffffffull, 0x4a1df58400000001ull, 0x62a12f63ffffffffull, 0x62a12f63ffffffffull, 0x4a1df58400000001ull},
    {18, 0xd0826a0cffffffffull, 0x14fe08aa00000001ull, 0xd0826a0cdfffffffull, 0xd0826a0cffffffffull, 0x14fe08aa00000001ull},
    {19, 0x0ull, 0x7fffffffffffffffull, 0x0ull, 0x0ull, 0x7fffffffffffffffull},
    {19, 0x1ull, 0xfffffffeffffffffull, 0x3ull, 0x1ull, 0x1ull},
    {19, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x7fffffffffffffffull, 0x7fffffffffffffffull},
    {19, 0x8000000000000000ull, 0x123456789abcdef0ull, 0x35f6b71611838017ull, 0x8000000000000000ull, 0x123456789abcdef0ull},
    {19, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull},
    {19, 0xffffffffull, 0x0ull, 0xffffffdfull, 0xffffffffull, 0xffffffffull},
    {19, 0x100000000ull, 0xffffffffull, 0x100000000ull, 0x100000000ull, 0x100000000ull},
    {19, 0xfffffffeffffffffull, 0x1ull, 0x884dcfec36f43da6ull, 0xfffffffeffffffffull, 0x1ull},
    {19, 0x123456789abcdef0ull, 0x100000000ull, 0x123456789abcdef0ull, 0x123456789abcdef0ull, 0x123456789abcdef0ull},
    {19, 0x0ull, 0x7fffffffffffffffull, 0x200ull, 0x0ull, 0x7fffffffffffffffull},
    {19, 0x1ull, 0xfffffffeffffffffull, 0x1ull, 0x1ull, 0x1ull},
    {19, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x97e21514d87b299dull, 0x7fffffffffffffffull, 0x7fffffffffffffffull},
    {19, 0x7ae9f54bc19e56ddull, 0x4a6ac7aeb830ed76ull, 0x7ae9f54bc19e56ddull, 0x7ae9f54bc19e56ddull, 0x7ae9f54bc19e56ddull},
    {19, 0x5b1c8aa1eb9a258cull, 0xf4a0ea8b3badac72ull, 0x5b1c8aa1e99a258cull, 0x5b1c8aa1eb9a258cull, 0x5b1c8aa1eb9a258cull},
    {19, 0x869f35deffffffffull, 0xdeffa68100000001ull, 0x869f35deffffffffull, 0x869f35deffffffffull, 0xdeffa68100000001ull},
    {19, 0x5752b4dfffffffffull, 0x2a51ee7e00000001ull, 0x5752b4dfdfffffffull, 0x5752b4dfffffffffull, 0x5752b4dfffffffffull},
    {20, 0x0ull, 0x7fffffffffffffffull, 0x0ull, 0x0ull, 0x7fffffffffffffffull},
    {20, 0x1ull, 0xfffffffeffffffffull, 0x3ull, 0x1ull, 0xfffffffeffffffffull},
    {20, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x7fffffffffffffffull, 0x8000000000000000ull},
    {20, 0x8000000000000000ull, 0x123456789abcdef0ull, 0xd316f470e2d8993bull, 0x8000000000000000ull, 0x8000000000000000ull},
    {20, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull},
    {20, 0xffffffffull, 0x0ull, 0xffffffdfull, 0xffffffffull, 0xffffffffull},
    {20, 0x100000000ull, 0xffffffffull, 0x100000000ull, 0x100000000ull, 0x100000000ull},
    {20, 0xfffffffeffffffffull, 0x1ull, 0xfc2b7ace1072e7b9ull, 0xfffffffeffffffffull, 0xfffffffeffffffffull},
    {20, 0x123456789abcdef0ull, 0x100000000ull, 0x123456789abcdef0ull, 0x123456789abcdef0ull, 0x123456789abcdef0ull},
    {20, 0x0ull, 0x7fffffffffffffffull, 0x200ull, 0x0ull, 0x7fffffffffffffffull},
    {20, 0x1ull, 0xfffffffeffffffffull, 0x1ull, 0x1ull, 0xfffffffeffffffffull},
    {20, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x142c1e683d47a123ull, 0x7fffffffffffffffull, 0x8000000000000000ull},
    {20, 0x5ea0e74f0d30cb76ull, 0xecd62560f1407a13ull, 0x5ea0e74f0d30cb76ull, 0x5ea0e74f0d30cb76ull, 0xecd62560f1407a13ull},
    {20, 0x6f8664b99db31c8bull, 0x77417f9d1c387d00ull, 0x6f8664b99fb31c8bull, 0x6f8664b99db31c8bull, 0x77417f9d1c387d00ull},
    {20, 0x1e0ce078ffffffffull, 0xc130cf3c00000001ull, 0x1e0ce078ffffffffull, 0x1e0ce078ffffffffull, 0xc130cf3c00000001ull},
    {20, 0xd160d889ffffffffull, 0x2b456f6800000001ull, 0xd160d889dfffffffull, 0xd160d889ffffffffull, 0xd160d889ffffffffull},
    {21, 0x0ull, 0x7fffffffffffffffull, 0x0ull, 0x0ull, 0x0ull},
    {21, 0x1ull, 0xfffffffeffffffffull, 0x3ull, 0x1ull, 0x1ull},
    {21, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x7fffffffffffffffull, 0x0ull},
    {21, 0x8000000000000000ull, 0x123456789abcdef0ull, 0x691beca72ac31615ull, 0x8000000000000000ull, 0x0ull},
    {21, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull},
    {21, 0xffffffffull, 0x0ull, 0xffffffdfull, 0xffffffffull, 0x0ull},
    {21, 0x100000000ull, 0xffffffffull, 0x100000000ull, 0x100000000ull, 0x0ull},
    {21, 0xfffffffeffffffffull, 0x1ull, 0xc89594109911783aull, 0xfffffffeffffffffull, 0x1ull},
    {21, 0x123456789abcdef0ull, 0x100000000ull, 0x123456789abcdef0ull, 0x123456789abcdef0ull, 0x0ull},
    {21, 0x0ull, 0x7fffffffffffffffull, 0x200ull, 0x0ull, 0x0ull},
    {21, 0x1ull, 0xfffffffeffffffffull, 0x1ull, 0x1ull, 0x1ull},
    {21, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xa69bfe93d6efb4ddull, 0x7fffffffffffffffull, 0x0ull},
    {21, 0xd5e7cc579d21963aull, 0x93826a1ee8ab2ed1ull, 0xd5e7cc579d21963aull, 0xd5e7cc579d21963aull, 0x9182481688210610ull},
    {21, 0x657c382c4e1cfcddull, 0x837a0dc63ac35f32ull, 0x657c382c4c1cfcddull, 0x657c382c4e1cfcddull, 0x17808040a005c10ull},
    {21, 0x7552c284ffffffffull, 0x995c2cf200000001ull, 0x7552c284ffffffffull, 0x7552c284ffffffffull, 0x1150008000000001ull},
    {21, 0xe20d4f8dffffffffull, 0x4864cdbc00000001ull, 0xe20d4f8ddfffffffull, 0xe20d4f8dffffffffull, 0x40044d8c00000001ull},
    {22, 0x0ull, 0x7fffffffffffffffull, 0x0ull, 0x0ull, 0x7fffffffffffffffull},
    {22, 0x1ull, 0xfffffffeffffffffull, 0x3ull, 0x1ull, 0xfffffffeffffffffull},
    {22, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x7fffffffffffffffull, 0xffffffffffffffffull},
    {22, 0x8000000000000000ull, 0x123456789abcdef0ull, 0xbad3dfdbb007d4ecull, 0x8000000000000000ull, 0x923456789abcdef0ull},
    {22, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull},
    {22, 0xffffffffull, 0x0ull, 0xffffffdfull, 0xffffffffull, 0xffffffffull},
    {22, 0x100000000ull, 0xffffffffull, 0x100000000ull, 0x100000000ull, 0x1ffffffffull},
    {22, 0xfffffffeffffffffull, 0x1ull, 0xedecea3c6fa0d23full, 0xfffffffeffffffffull, 0xfffffffeffffffffull},
    {22, 0x123456789abcdef0ull, 0x100000000ull, 0x123456789abcdef0ull, 0x123456789abcdef0ull, 0x123456799abcdef0ull},
    {22, 0x0ull, 0x7fffffffffffffffull, 0x200ull, 0x0ull, 0x7fffffffffffffffull},
    {22, 0x1ull, 0xfffffffeffffffffull, 0x1ull, 0x1ull, 0xfffffffeffffffffull},
    {22, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xf7926675f45d1971ull, 0x7fffffffffffffffull, 0xffffffffffffffffull},
    {22, 0x360443a82569e20cull, 0x3800d9ff899231faull, 0x360443a82569e20cull, 0x360443a82569e20cull, 0x3e04dbffadfbf3feull},
    {22, 0xc265df77a6c71a0dull, 0x5e94d6ccc50557f8ull, 0xc265df77a4c71a0dull, 0xc265df77a6c71a0dull, 0xdef5dfffe7c75ffdull},
    {22, 0x41a90d6bffffffffull, 0x4bb3092400000001ull, 0x41a90d6bffffffffull, 0x41a90d6bffffffffull, 0x4bbb0d6fffffffffull},
    {22, 0xc54e9269ffffffffull, 0xf536f34000000001ull, 0xc54e9269dfffffffull, 0xc54e9269ffffffffull, 0xf57ef369ffffffffull},
    {23, 0x0ull, 0x7fffffffffffffffull, 0x0ull, 0x0ull, 0x7fffffffffffffffull},
    {23, 0x1ull, 0xfffffffeffffffffull, 0x3ull, 0x1ull, 0xfffffffefffffffeull},
    {23, 0x7fffffffffffffffull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x7fffffffffffffffull, 0xffffffffffffffffull},
    {23, 0x8000000000000000ull, 0x123456789abcdef0ull, 0xe3b411de678ee5c0ull, 0x8000000000000000ull, 0x923456789abcdef0ull},
    {23, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0x0ull},
    {23, 0xffffffffull, 0x0ull, 0xffffffdfull, 0xffffffffull, 0xffffffffull},
    {23, 0x100000000ull, 0xffffffffull, 0x100000000ull, 0x100000000ull, 0x1ffffffffull},
    {23, 0xfffffffeffffffffull, 0x1ull, 0xe4e171d83c299defull, 0xfffffffeffffffffull, 0xfffffffefffffffeull},
    {23, 0x123456789abcdef0ull, 0x100000000ull, 0x123456789abcdef0ull, 0x123456789abcdef0ull, 0x123456799abcdef0ull},
    {23, 0x0ull, 0x7fffffffffffffffull, 0x200ull, 0x0ull, 0x7fffffffffffffffull},
    {23, 0x1ull, 0xfffffffeffffffffull, 0x1ull, 0x1ull, 0xfffffffefffffffeull},
    {23, 0x7fffffffffffffffull, 0x8000000000000000ull, 0xaa23249b400d9ebdull, 0x7fffffffffffffffull, 0xffffffffffffffffull},
    {23, 0xc1f06114a97ad060ull, 0x679f7fe64c950a15ull, 0xc1f06114a97ad060ull, 0xc1f06114a97ad060ull, 0xa66f1ef2e5efda75ull},
    {23, 0x59bdb13042bcfe48ull, 0x1669010b3ea903bbull, 0x59bdb13040bcfe48ull, 0x59bdb13042bcfe48ull, 0x4fd4b03b7c15fdf3ull},
    {23, 0x701d03a9ffffffffull, 0xf96a081600000001ull, 0x701d03a9ffffffffull, 0x701d03a9ffffffffull, 0x89770bbffffffffeull},
    {23, 0x9b817862ffffffffull, 0x3c3e50fe00000001ull, 0x9b817862dfffffffull, 0x9b817862ffffffffull, 0xa7bf289cfffffffeull},
}};

class GuestBlock {
public:
    explicit GuestBlock(bool writable) {
#ifdef _WIN32
        block = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, BlockBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
        block = static_cast<std::uint8_t*>(std::aligned_alloc(BlockBytes, BlockBytes));
#endif
        Require(block != nullptr, "global atomics: cannot allocate the guest block");
        GuestAllocations::Mutation().Add(block, BlockBytes, true, writable);
    }

    ~GuestBlock() {
        GuestAllocations::Mutation().Remove(block);
    }

    GuestBlock(const GuestBlock&) = delete;
    GuestBlock& operator=(const GuestBlock&) = delete;

    std::uint8_t* Data() { return block; }
    const std::uint8_t* Data() const { return block; }

private:
    std::uint8_t* block = nullptr;
};

void Put(std::vector<std::uint8_t>& image, std::uint32_t offset, std::uint64_t value, std::uint32_t bytes) {
    for (std::uint32_t byte = 0; byte < bytes; ++byte) image.at(offset + byte) = static_cast<std::uint8_t>(value >> (byte * 8u));
}

std::array<std::uint32_t, 4> BufferDescriptor(const void* data, std::uint32_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(data);
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>((address >> 32u) & 0xffffu), bytes, 0x01016facu};
}

std::string Hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

const Row& RowOf(std::uint32_t op, std::uint32_t tid) {
    return Rows.at(op * RowsPerOp + tid % RowsPerOp);
}

std::uint32_t OpBytes(std::uint32_t op) {
    return op < NarrowOps ? 4u : 8u;
}

std::vector<std::uint8_t> Initial() {
    std::vector<std::uint8_t> image(BlockBytes, Fill);
    for (std::uint32_t op = 0; op < OpCount; ++op) {
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            Put(image, op * OpStride + tid * 8u, RowOf(op, tid).memory, OpBytes(op));
        }
    }
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        Put(image, Special + tid * 8u, 0x1000u * tid, 4);
        Put(image, Special + Threads * 8u + tid * 8u, 0xa5a5a5a5u ^ tid, 4);
        Put(image, Special + Threads * 24u + tid * 8u, 0x0f0f0f00u + tid, 4);
    }
    Put(image, Special + Threads * 16u + 4u, 0x11u, 4);
    return image;
}

std::vector<std::uint8_t> Expected() {
    auto image = Initial();
    for (std::uint32_t op = 0; op < OpCount; ++op) {
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            Put(image, op * OpStride + tid * 8u, RowOf(op, tid).final, OpBytes(op));
        }
    }
    std::uint32_t total = 0x11u;
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        Put(image, Special + tid * 8u, 0x1000u * tid + tid + 1u, 4);
        Put(image, Special + Threads * 8u + tid * 8u, (0xa5a5a5a5u ^ tid) ^ (tid << 8u), 4);
        if (tid < 16u) Put(image, Special + Threads * 24u + tid * 8u, (0x0f0f0f00u + tid) | 0xf0u, 4);
        total += (tid + 1u) << 8u;
    }
    Put(image, Special + Threads * 16u + 4u, total, 4);
    return image;
}

void FillInput() {
    for (std::uint32_t op = 0; op < OpCount; ++op) {
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            const auto& row = RowOf(op, tid);
            auto* words = &Input[(op * Threads + tid) * 4u];
            if (OpBytes(op) == 4u) {
                words[0] = static_cast<std::uint32_t>(row.data);
                words[1] = static_cast<std::uint32_t>(row.comparator);
                words[2] = 0u;
                words[3] = 0u;
            } else {
                words[0] = static_cast<std::uint32_t>(row.data);
                words[1] = static_cast<std::uint32_t>(row.data >> 32u);
                words[2] = static_cast<std::uint32_t>(row.comparator);
                words[3] = static_cast<std::uint32_t>(row.comparator >> 32u);
            }
        }
    }
}

void Dispatch(AgcDriver::VulkanDevice& device, std::uint32_t waveSize, const std::uint8_t* base) {
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(base));
    std::vector<std::uint32_t> userData(10, 0u);
    const auto input = BufferDescriptor(Input.data(), static_cast<std::uint32_t>(Input.size() * 4u));
    const auto output = BufferDescriptor(Output.data(), static_cast<std::uint32_t>(Output.size() * 4u));
    std::copy(input.begin(), input.end(), userData.begin());
    std::copy(output.begin(), output.end(), userData.begin() + 4);
    userData[8] = static_cast<std::uint32_t>(address);
    userData[9] = static_cast<std::uint32_t>(address >> 32u);
    const std::span<const std::uint32_t> code(GlobalAtomicsCode);
    const std::array<ShaderRecompiler::MemoryRegion, 1> memory{{{reinterpret_cast<std::uintptr_t>(code.data()), std::as_bytes(code)}}};
    const ShaderRecompiler::ShaderComputeStageInfo compute{{Threads, 1, 1}, 0, {false, false, false}, false, 1};
    ShaderRecompiler::RecompileRequest request{
        {ShaderStage::Compute, reinterpret_cast<std::uintptr_t>(code.data()), code, 0, {}},
        {waveSize, 0, userData, compute, std::nullopt, std::nullopt, memory},
        device.Target(),
        {0, 0, 0, 128}
    };
    request.useCache = false;
    const auto result = ShaderRecompiler::Recompile(request);
    device.Dispatch(result, 1, 1, 1, {}, reinterpret_cast<std::uintptr_t>(code.data()));
    device.WaitIdle();
}

void RunAtomics(AgcDriver::VulkanDevice& device, GuestBlock& guest, std::uint32_t waveSize) {
    const auto initial = Initial();
    std::memcpy(guest.Data(), initial.data(), BlockBytes);
    FillInput();
    Output.fill(Sentinel);
    Dispatch(device, waveSize, guest.Data());
    const auto wave = "global atomics: wave" + std::to_string(waveSize) + " ";
    for (std::uint32_t op = 0; op < OpCount; ++op) {
        for (std::uint32_t tid = 0; tid < Threads; ++tid) {
            const auto& row = RowOf(op, tid);
            const auto* words = &Output[(op * Threads + tid) * 2u];
            const auto returned = OpBytes(op) == 4u ? static_cast<std::uint64_t>(words[0]) : (static_cast<std::uint64_t>(words[1]) << 32u) | words[0];
            Require(returned == row.returned, wave + "op " + std::to_string(op) + " lane " + std::to_string(tid) + " returned " + Hex(returned) + ", expected " + Hex(row.returned));
        }
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> order;
    for (std::uint32_t tid = 0; tid < Threads; ++tid) {
        const auto flat = Output[(OpCount * Threads + tid) * 2u];
        Require(flat == (0xa5a5a5a5u ^ tid), wave + "flat xor returned " + Hex(flat) + " to lane " + std::to_string(tid));
        order.emplace_back(Output[((OpCount + 1u) * Threads + tid) * 2u], tid);
    }
    std::sort(order.begin(), order.end());
    std::uint32_t running = 0x11u;
    for (const auto& [returned, tid] : order) {
        Require(returned == running, wave + "contended add returned " + Hex(returned) + " to lane " + std::to_string(tid) + ", expected " + Hex(running));
        running += (tid + 1u) << 8u;
    }
    const auto expected = Expected();
    for (std::uint32_t offset = 0; offset < BlockBytes; ++offset) {
        const auto actual = guest.Data()[offset];
        Require(actual == expected[offset], wave + "byte " + std::to_string(offset) + " is " + std::to_string(actual) + ", expected " + std::to_string(expected[offset]));
    }
}

void RunReadOnly(AgcDriver::VulkanDevice& device, GuestBlock& guest) {
    const auto initial = Initial();
    std::memcpy(guest.Data(), initial.data(), BlockBytes);
    FillInput();
    Dispatch(device, 32, guest.Data());
    for (std::uint32_t offset = 0; offset < BlockBytes; ++offset) {
        Require(guest.Data()[offset] == initial[offset], "global atomics: an atomic into a read-only range changed byte " + std::to_string(offset));
    }
}

}

int main() {
    try {
        const auto device = OpenVulkanTestDevice();
        if (!device) return VulkanTestSkipped;
        GuestBlock writable(true);
        GuestBlock readOnly(false);
        RunAtomics(*device, writable, 32);
        RunAtomics(*device, writable, 64);
        RunReadOnly(*device, readOnly);
        std::puts("global atomics tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
