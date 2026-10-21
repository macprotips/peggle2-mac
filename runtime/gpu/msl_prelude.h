// MSL shared by every translated shader. Semantics follow Xenia (BSD-3-Clause).
#pragma once

namespace rt::gpu
{
    inline const char* kMslPrelude = R"MSL(
#include <metal_stdlib>
using namespace metal;
#pragma METAL fp math_mode(safe)
#pragma METAL fp contract(off)

struct XeState
{
    float4 r[64];
    float4 o[16];
    float4 position;
    float4 pointSize;
    float4 color[4];
    float depth;
    float ps;
    bool p0;
    bool killed;
    int a0;
    int aL[5];
    int loopCounter[5];
    int loopDepth;
    int callStack[4];
    int callDepth;
    int fetchConstant;
    int fetchIndex;
    uint fetchStride;
};

struct XePixelIO
{
    float4 fragCoord;      // in guest pixels
    bool frontFacing;
    constant float* texScale; // per texture slot: host texels per guest texel (scaled resolve copies)
};

// Shader Model 3 multiplication: +-0 (or denormal) times anything, even inf or NaN, is +0.
static inline float xe_mul(float a, float b) { return (min(abs(a), abs(b)) == 0.0) ? 0.0 : a * b; }
static inline float4 xe_mul(float4 a, float4 b) { return select(a * b, float4(0.0), min(abs(a), abs(b)) == 0.0); }
// Shader Model 3 max/min: NaN handling of `a op b ? a : b`.
static inline float xe_max(float a, float b) { return a >= b ? a : b; }
static inline float4 xe_max(float4 a, float4 b) { return select(b, a, a >= b); }
static inline float xe_min(float a, float b) { return a < b ? a : b; }
static inline float4 xe_min(float4 a, float4 b) { return select(b, a, a < b); }
static inline float xe_dot4(float4 a, float4 b) { float4 p = xe_mul(a, b); return ((p.x + p.y) + p.z) + p.w; }
static inline float xe_dot3(float4 a, float4 b) { float4 p = xe_mul(a, b); return (p.x + p.y) + p.z; }
static inline float xe_dot2add(float4 a, float4 b, float4 c) { float4 p = xe_mul(a, b); return (p.x + p.y) + c.x; }
static inline float xe_max4(float4 a) { return max(max(a.x, a.y), max(a.z, a.w)); }
static inline float4 xe_dst(float4 a, float4 b) { return float4(1.0, xe_mul(a.y, b.y), a.z, b.w); }
static inline float xe_clamp_neg_inf(float v) { return v == -INFINITY ? -FLT_MAX : v; }
static inline float xe_clamp_inf(float v) { return v == INFINITY ? FLT_MAX : (v == -INFINITY ? -FLT_MAX : v); }
static inline float xe_inf_to_zero(float v) { return isinf(v) ? copysign(0.0, v) : v; }
static inline float xe_mul_prev2(float a, float b, float ps)
{
    return (ps == -FLT_MAX || !isfinite(ps) || !isfinite(b) || b <= 0.0) ? -FLT_MAX : xe_mul(a, ps);
}

// cube: operand 0 is (z, _, x, y) as .zzxy, operand 1 is .yxzz; result is (t, s, 2 * major axis, face).
static inline float4 xe_cube(float4 a, float4 b)
{
    float3 v = float3(b.y, b.x, a.x);
    float3 m = abs(v);
    float face, ma, s, t;
    if (m.z >= m.x && m.z >= m.y) { face = v.z < 0.0 ? 5.0 : 4.0; ma = v.z; s = v.z < 0.0 ? -v.x : v.x; t = -v.y; }
    else if (m.y >= m.x) { face = v.y < 0.0 ? 3.0 : 2.0; ma = v.y; s = v.x; t = v.y < 0.0 ? -v.z : v.z; }
    else { face = v.x < 0.0 ? 1.0 : 0.0; ma = v.x; s = v.x < 0.0 ? v.z : -v.z; t = -v.y; }
    return float4(t, s, 2.0 * abs(ma), face);
}

// Rebuilds a direction from xe_cube's (t, s, 2 * major axis, face) after the title's own projection.
static inline float3 xe_cube_direction(float4 c)
{
    float s = c.y, t = c.x;
    switch (int(c.w))
    {
    case 0: return float3(1.0, -t, -s);
    case 1: return float3(-1.0, -t, s);
    case 2: return float3(s, 1.0, t);
    case 3: return float3(s, -1.0, -t);
    case 4: return float3(s, -t, 1.0);
    default: return float3(-s, -t, -1.0);
    }
}

static inline uint xe_swap(uint v, uint endian)
{
    switch (endian & 3u)
    {
    case 1u: return ((v << 8) & 0xFF00FF00u) | ((v >> 8) & 0x00FF00FFu);
    case 2u: return ((v << 24) | ((v << 8) & 0x00FF0000u) | ((v >> 8) & 0x0000FF00u) | (v >> 24));
    case 3u: return (v >> 16) | (v << 16);
    default: return v;
    }
}

// Vertex fetch. Fetch constant k is two dwords at fetch[k * 2]: address in dwords (bits 2:31), then
// endian (bits 0:1). `dword` is the element offset in dwords from the instruction.
static inline uint xe_word(device const uint* mem, constant uint* fetch, int k, uint dword)
{
    uint base = fetch[k * 2] >> 2;
    uint endian = fetch[k * 2 + 1] & 3u;
    return xe_swap(mem[base + dword], endian);
}

static inline float xe_norm(int v, uint bits, bool isSigned, bool isInteger, uint rfMode)
{
    if (isInteger)
        return float(v);
    if (!isSigned)
        return float(uint(v)) / float((1u << bits) - 1u);
    float range = float((1u << (bits - 1u)) - 1u);
    return rfMode == 0u ? max(float(v) / range, -1.0) : (float(v) + 0.5) / (range + 0.5);
}

static inline int xe_field(uint word, uint offset, uint bits, bool isSigned)
{
    uint v = (word >> offset) & ((1u << bits) - 1u);
    if (isSigned && bits < 32u && (v & (1u << (bits - 1u))))
        return int(v | ~((1u << bits) - 1u));
    return int(v);
}

static inline float4 xe_vfetch(device const uint* mem, constant uint* fetch, int k, uint dword, uint format,
                               bool isSigned, bool isInteger, uint rfMode, int expAdjust)
{
    float4 r = float4(0.0, 0.0, 0.0, 1.0);
    switch (format)
    {
    case 6: // k_8_8_8_8
    {
        uint w = xe_word(mem, fetch, k, dword);
        for (uint i = 0; i < 4u; i++)
            r[i] = xe_norm(xe_field(w, i * 8u, 8u, isSigned), 8u, isSigned, isInteger, rfMode);
        break;
    }
    case 7: // k_2_10_10_10
    {
        uint w = xe_word(mem, fetch, k, dword);
        r.x = xe_norm(xe_field(w, 0u, 10u, isSigned), 10u, isSigned, isInteger, rfMode);
        r.y = xe_norm(xe_field(w, 10u, 10u, isSigned), 10u, isSigned, isInteger, rfMode);
        r.z = xe_norm(xe_field(w, 20u, 10u, isSigned), 10u, isSigned, isInteger, rfMode);
        r.w = xe_norm(xe_field(w, 30u, 2u, isSigned), 2u, isSigned, isInteger, rfMode);
        break;
    }
    case 16: // k_10_11_11
    {
        uint w = xe_word(mem, fetch, k, dword);
        r.x = xe_norm(xe_field(w, 0u, 11u, isSigned), 11u, isSigned, isInteger, rfMode);
        r.y = xe_norm(xe_field(w, 11u, 11u, isSigned), 11u, isSigned, isInteger, rfMode);
        r.z = xe_norm(xe_field(w, 22u, 10u, isSigned), 10u, isSigned, isInteger, rfMode);
        break;
    }
    case 17: // k_11_11_10
    {
        uint w = xe_word(mem, fetch, k, dword);
        r.x = xe_norm(xe_field(w, 0u, 10u, isSigned), 10u, isSigned, isInteger, rfMode);
        r.y = xe_norm(xe_field(w, 10u, 11u, isSigned), 11u, isSigned, isInteger, rfMode);
        r.z = xe_norm(xe_field(w, 21u, 11u, isSigned), 11u, isSigned, isInteger, rfMode);
        break;
    }
    case 25: // k_16_16
    case 26: // k_16_16_16_16
    {
        uint words = format == 25u ? 1u : 2u;
        for (uint i = 0; i < words * 2u; i++)
        {
            uint w = xe_word(mem, fetch, k, dword + i / 2u);
            r[i] = xe_norm(xe_field(w, (i & 1u) * 16u, 16u, isSigned), 16u, isSigned, isInteger, rfMode);
        }
        break;
    }
    case 31: // k_16_16_FLOAT
    case 32: // k_16_16_16_16_FLOAT
    {
        uint words = format == 31u ? 1u : 2u;
        for (uint i = 0; i < words * 2u; i++)
        {
            uint w = xe_word(mem, fetch, k, dword + i / 2u);
            r[i] = float(as_type<half>(ushort((w >> ((i & 1u) * 16u)) & 0xFFFFu)));
        }
        break;
    }
    case 33: // k_32
    case 34: // k_32_32
    case 35: // k_32_32_32_32
    {
        uint n = format == 35u ? 4u : format - 32u; // k_32 = 33, k_32_32 = 34, k_32_32_32_32 = 35
        for (uint i = 0; i < n; i++)
        {
            uint w = xe_word(mem, fetch, k, dword + i);
            if (isInteger)
                r[i] = isSigned ? float(int(w)) : float(w);
            else if (isSigned)
                r[i] = rfMode == 0u ? max(float(int(w)) * (1.0 / 2147483647.0), -1.0) : float(int(w)) * (1.0 / 2147483647.5) + (0.5 / 2147483647.5);
            else
                r[i] = float(w) * (1.0 / 4294967295.0);
        }
        break;
    }
    case 36: // k_32_FLOAT
    case 37: // k_32_32_FLOAT
    case 57: // k_32_32_32_FLOAT
    case 38: // k_32_32_32_32_FLOAT
    {
        uint n = format == 36u ? 1u : format == 37u ? 2u : format == 57u ? 3u : 4u;
        for (uint i = 0; i < n; i++)
            r[i] = as_type<float>(xe_word(mem, fetch, k, dword + i));
        return r; // floats ignore exp_adjust
    }
    default:
        break;
    }
    if (expAdjust != 0)
        r = r * exp2(float(expAdjust));
    return r;
}

// Per-draw values computed on the CPU; the layout matches rt::gpu::XeDraw in renderer.mm.
struct XeDraw
{
    float4 ndcScale;
    float4 ndcOffset;
    uint indexed;
    uint indexBase;      // physical byte address
    uint indexFormat;    // 0 = 16-bit, 1 = 32-bit
    uint indexEndian;
    int indexOffset;     // VGT_INDX_OFFSET
    uint flags;          // bit 0: XY divided by W; bit 1: Z divided by W; bit 2: W is not 1/W
    uint primitive;      // xenos::PrimitiveType
    uint alphaFunc;      // xenos::CompareFunction, 7 when alpha test is off (shaders use xe_alpha_func)
    float alphaRef;
    float pixelScale;    // host pixels per guest pixel (RenderScale)
    uint pad[2];
};

// Set per pipeline: the interpolants the vertex shader writes and the pixel shader may read (bit n: on),
// and the alpha test's compare function (7: always). Only those interpolants are stored and passed.
constant uint xe_interpolants [[function_constant(0)]];
constant uint xe_alpha_func [[function_constant(1)]];
constant bool xe_has_o0 = (xe_interpolants & 1u) != 0u;
constant bool xe_has_o1 = (xe_interpolants & 2u) != 0u;
constant bool xe_has_o2 = (xe_interpolants & 4u) != 0u;
constant bool xe_has_o3 = (xe_interpolants & 8u) != 0u;
constant bool xe_has_o4 = (xe_interpolants & 16u) != 0u;
constant bool xe_has_o5 = (xe_interpolants & 32u) != 0u;
constant bool xe_has_o6 = (xe_interpolants & 64u) != 0u;
constant bool xe_has_o7 = (xe_interpolants & 128u) != 0u;
constant bool xe_has_o8 = (xe_interpolants & 256u) != 0u;
constant bool xe_has_o9 = (xe_interpolants & 512u) != 0u;
constant bool xe_has_o10 = (xe_interpolants & 1024u) != 0u;
constant bool xe_has_o11 = (xe_interpolants & 2048u) != 0u;
constant bool xe_has_o12 = (xe_interpolants & 4096u) != 0u;
constant bool xe_has_o13 = (xe_interpolants & 8192u) != 0u;
constant bool xe_has_o14 = (xe_interpolants & 16384u) != 0u;
constant bool xe_has_o15 = (xe_interpolants & 32768u) != 0u;

struct XeVertexOut
{
    float4 position [[position]];
    float4 o0 [[user(o0), function_constant(xe_has_o0)]];
    float4 o1 [[user(o1), function_constant(xe_has_o1)]];
    float4 o2 [[user(o2), function_constant(xe_has_o2)]];
    float4 o3 [[user(o3), function_constant(xe_has_o3)]];
    float4 o4 [[user(o4), function_constant(xe_has_o4)]];
    float4 o5 [[user(o5), function_constant(xe_has_o5)]];
    float4 o6 [[user(o6), function_constant(xe_has_o6)]];
    float4 o7 [[user(o7), function_constant(xe_has_o7)]];
    float4 o8 [[user(o8), function_constant(xe_has_o8)]];
    float4 o9 [[user(o9), function_constant(xe_has_o9)]];
    float4 o10 [[user(o10), function_constant(xe_has_o10)]];
    float4 o11 [[user(o11), function_constant(xe_has_o11)]];
    float4 o12 [[user(o12), function_constant(xe_has_o12)]];
    float4 o13 [[user(o13), function_constant(xe_has_o13)]];
    float4 o14 [[user(o14), function_constant(xe_has_o14)]];
    float4 o15 [[user(o15), function_constant(xe_has_o15)]];
};
)MSL";

    // Vertex entry point, appended after the translated xe_vs_main. Index fetch and the position
    // transform follow Xenia's SPIR-V translator; primitives Metal lacks are expanded here.
    inline const char* kMslVertexEntry = R"MSL(
static XeVertexOut xe_run_vertex(uint guestVertex, constant XeDraw& d, constant float4* c, constant uint* boolLoop,
                                 constant uint* fetch, device const uint* mem)
{
    uint index = guestVertex;
    if (d.indexed != 0u)
    {
        uint address = d.indexBase + guestVertex * (d.indexFormat != 0u ? 4u : 2u);
        uint word = mem[address >> 2];
        if (d.indexFormat == 0u)
            word = (word >> ((address & 2u) * 8u)) & 0xFFFFu;
        index = xe_swap(word, d.indexEndian);
    }
    XeState st = {};
    st.r[0].x = float(int(index) + d.indexOffset);
    xe_vs_main(st, c, boolLoop, fetch, mem);

    float4 p = st.position;
    if ((d.flags & 4u) == 0u)
        p.w = 1.0 / p.w;
    if ((d.flags & 1u) != 0u)
        p.xy *= p.w;
    if ((d.flags & 2u) != 0u)
        p.z *= p.w;
    p.xyz = p.xyz * d.ndcScale.xyz + d.ndcOffset.xyz * p.w;

    XeVertexOut out;
    out.position = p;
    if (xe_has_o0) out.o0 = st.o[0];
    if (xe_has_o1) out.o1 = st.o[1];
    if (xe_has_o2) out.o2 = st.o[2];
    if (xe_has_o3) out.o3 = st.o[3];
    if (xe_has_o4) out.o4 = st.o[4];
    if (xe_has_o5) out.o5 = st.o[5];
    if (xe_has_o6) out.o6 = st.o[6];
    if (xe_has_o7) out.o7 = st.o[7];
    if (xe_has_o8) out.o8 = st.o[8];
    if (xe_has_o9) out.o9 = st.o[9];
    if (xe_has_o10) out.o10 = st.o[10];
    if (xe_has_o11) out.o11 = st.o[11];
    if (xe_has_o12) out.o12 = st.o[12];
    if (xe_has_o13) out.o13 = st.o[13];
    if (xe_has_o14) out.o14 = st.o[14];
    if (xe_has_o15) out.o15 = st.o[15];
    return out;
}

// b + c - a for every output: the fourth corner of a rectangle.
static XeVertexOut xe_mirror(XeVertexOut a, XeVertexOut b, XeVertexOut c)
{
    XeVertexOut r;
    r.position = b.position + c.position - a.position;
    if (xe_has_o0) r.o0 = b.o0 + c.o0 - a.o0;
    if (xe_has_o1) r.o1 = b.o1 + c.o1 - a.o1;
    if (xe_has_o2) r.o2 = b.o2 + c.o2 - a.o2;
    if (xe_has_o3) r.o3 = b.o3 + c.o3 - a.o3;
    if (xe_has_o4) r.o4 = b.o4 + c.o4 - a.o4;
    if (xe_has_o5) r.o5 = b.o5 + c.o5 - a.o5;
    if (xe_has_o6) r.o6 = b.o6 + c.o6 - a.o6;
    if (xe_has_o7) r.o7 = b.o7 + c.o7 - a.o7;
    if (xe_has_o8) r.o8 = b.o8 + c.o8 - a.o8;
    if (xe_has_o9) r.o9 = b.o9 + c.o9 - a.o9;
    if (xe_has_o10) r.o10 = b.o10 + c.o10 - a.o10;
    if (xe_has_o11) r.o11 = b.o11 + c.o11 - a.o11;
    if (xe_has_o12) r.o12 = b.o12 + c.o12 - a.o12;
    if (xe_has_o13) r.o13 = b.o13 + c.o13 - a.o13;
    if (xe_has_o14) r.o14 = b.o14 + c.o14 - a.o14;
    if (xe_has_o15) r.o15 = b.o15 + c.o15 - a.o15;
    return r;
}

vertex XeVertexOut xe_vertex(uint vid [[vertex_id]], constant XeDraw& d [[buffer(0)]], constant float4* c [[buffer(1)]],
                             constant uint* boolLoop [[buffer(2)]], constant uint* fetch [[buffer(3)]],
                             device const uint* mem [[buffer(4)]])
{
    switch (d.primitive)
    {
    case 5: // triangle fan, drawn as a list: triangle t is (0, t + 1, t + 2)
    {
        uint k = vid % 3u;
        return xe_run_vertex(k == 0u ? 0u : vid / 3u + k, d, c, boolLoop, fetch, mem);
    }
    case 13: // quad list, drawn as a triangle list
    {
        const uint corners[6] = { 0u, 1u, 2u, 0u, 2u, 3u };
        return xe_run_vertex(vid / 6u * 4u + corners[vid % 6u], d, c, boolLoop, fetch, mem);
    }
    case 8: // rectangle list: the fourth vertex mirrors one across the longest edge (the diagonal)
    {
        uint first = vid / 6u * 3u;
        XeVertexOut v[3];
        for (uint i = 0; i < 3u; i++)
            v[i] = xe_run_vertex(first + i, d, c, boolLoop, fetch, mem);
        float2 p0 = v[0].position.xy, p1 = v[1].position.xy, p2 = v[2].position.xy;
        float e12 = distance_squared(p1, p2), e20 = distance_squared(p2, p0), e01 = distance_squared(p0, p1);
        uint s = (e12 > e20 && e12 > e01) ? 0u : (e20 > e01 ? 1u : 2u);
        // Strip s, s+1, s+2, mirrored as the list (0, 1, 2), (2, 1, 3).
        const uint slots[6] = { 0u, 1u, 2u, 2u, 1u, 3u };
        uint slot = slots[vid % 6u];
        if (slot < 3u)
            return v[(s + slot) % 3u];
        return xe_mirror(v[s], v[(s + 1u) % 3u], v[(s + 2u) % 3u]);
    }
    default:
        return xe_run_vertex(vid, d, c, boolLoop, fetch, mem);
    }
}
)MSL";
}
