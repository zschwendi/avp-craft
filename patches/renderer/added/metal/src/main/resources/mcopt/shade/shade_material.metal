// Copyright 2026 mcopt-shade contributors. Licensed under Apache-2.0.
// Compact LabPBR _n atlas. Self-contained for offscreen validation: only metal_stdlib is required.
#ifndef MCOPT_SHADE_MATERIAL
#define MCOPT_SHADE_MATERIAL
#include <metal_stdlib>
using namespace metal;

struct ShadeMaterial {
    float4 diffuse; // normalized block-atlas sprite rectangle
    float4 tile;    // normalized compact-atlas content rectangle (gutters outside it)
    float4 height;  // x/y alpha range, z -1 neutral metadata / 0 real map / 1 POM, w solid terrain
    uint id;       // 0 = neutral fallback
};

// ids: R8Unorm, one texel per aligned 16x16 block-atlas texels. table: RGBA32Float,
// width 256, height 3: rows diffuse bounds, compact content bounds, height metadata.
static inline ShadeMaterial shadeMaterialLookup(float2 uv, texture2d<half> normals,
                                                 texture2d<half> ids, texture2d<float> table) {
    ShadeMaterial m = {};
    if (normals.get_width() <= 1 || table.get_height() < 3 || any(uv < 0.0) || any(uv >= 1.0)) return m;
    uint2 q = min(uint2(uv * float2(ids.get_width(), ids.get_height())), uint2(ids.get_width()-1, ids.get_height()-1));
    uint id = uint(round(float(ids.read(q).r) * 255.0));
    if (id == 0 || id >= table.get_width()) return m;
    float4 rect = table.read(uint2(id, 0));
    if (any(uv < rect.xy) || any(uv >= rect.zw) || any(rect.zw <= rect.xy)) return m;
    m.diffuse = rect;
    m.tile = table.read(uint2(id, 1));
    m.height = table.read(uint2(id, 2));
    m.id = id;
    return m;
}

static inline float2 shadeMaterialLocal(ShadeMaterial m, float2 uv) {
    return saturate((uv - m.diffuse.xy) / max(m.diffuse.zw - m.diffuse.xy, float2(1e-8)));
}

static inline float2 shadeMaterialAtlasUV(ShadeMaterial m, float2 local, texture2d<half> normals) {
    float2 halfTexel = 0.5 / float2(normals.get_width(), normals.get_height());
    return clamp(mix(m.tile.xy, m.tile.zw, local), m.tile.xy + halfTexel, m.tile.zw - halfTexel);
}

// No color-space conversion: RG is the DirectX/top-down tangent normal, B is AO, A is height.
static inline float4 shadeMaterialSample(ShadeMaterial m, float2 local, texture2d<half> normals) {
    if (m.id == 0 || m.height.z < 0.0) return float4(0.5, 0.5, 1.0, 1.0);
    constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::linear, mip_filter::none);
    return float4(normals.sample(smp, shadeMaterialAtlasUV(m, local, normals), level(0.0)));
}

// Derivative TBN follows increasing U and increasing V (texture V points down). LabPBR's
// DirectX G already points down, so it is used with +B; negating it again would invert the relief.
// The geometric face normal remains separate for shadow bias and material classification.
static inline bool shadeMaterialBasis(float3 geometric, float3 dpx, float3 dpy, float2 du, float2 dv,
                                      thread float3 &T, thread float3 &B) {
    float det = du.x * dv.y - du.y * dv.x;
    if (abs(det) < 1e-12) return false;
    float3 tu = (dpx * dv.y - dpy * du.y) / det;
    float3 tv = (dpy * du.x - dpx * dv.x) / det;
    tu -= geometric * dot(geometric, tu);
    tv -= geometric * dot(geometric, tv);
    if (min(dot(tu,tu), dot(tv,tv)) < 1e-16) return false;
    T = normalize(tu);
    // Gram-Schmidt preserves mirrored UV handedness without assuming a fixed cube orientation.
    tv -= T * dot(T, tv);
    if (dot(tv,tv) < 1e-16) return false;
    B = normalize(tv);
    return true;
}

static inline float3 shadeMaterialNormal(float4 data, float3 geometric, float3 T, float3 B, float strength) {
    float2 xy = clamp((data.rg * 2.0 - 1.0) * strength, float2(-0.99), float2(0.99));
    // Neutral PNGs typically use 127/128; remove the unavoidable half-step tilt at that value.
    xy = select(xy, float2(0.0), abs(xy) <= float2(1.01 / 255.0));
    float z = sqrt(max(1.0 - dot(xy,xy), 0.001));
    return normalize(T * xy.x + B * xy.y + geometric * z);
}

// 12 fixed layers + 3 bisections, only eligible opaque terrain within 12 blocks. This changes
// interior texture relief, never geometry, silhouette, collision, or the shadow caster mesh.
static inline float2 shadeMaterialParallax(ShadeMaterial m, float2 local, float3 towardEye,
                                          float3 geometric, float3 T, float3 B, texture2d<half> normals,
                                          float distance, float depthScale) {
    if (m.id == 0 || m.height.z < 0.5 || m.height.y-m.height.x < 1.0/255.0 || depthScale <= 0.0 || distance >= 12.0) return local;
    if (shadeMaterialSample(m,local,normals).a >= 1.0-0.5/255.0) return local; // an exposed top has exactly zero displacement
    float facing = dot(towardEye, geometric);
    if (facing <= 0.18) return local;
    float fade = (1.0-smoothstep(8.0,12.0,distance)) * smoothstep(0.18,0.35,facing);
    float2 delta = float2(dot(towardEye,T),dot(towardEye,B)) / max(facing,0.18) * depthScale * fade;
    delta = clamp(delta, float2(-0.16), float2(0.16));
    // Surface follows the ray away from the viewer into the block; texture V is top-down.
    float low = 0.0, high = 1.0;
    float2 hit = local;
    bool found = false;
    for (uint i=1; i<=12; ++i) {
        float layer = float(i)/12.0;
        float2 at = local - delta * layer;
        // Never borrow the neighbor's tile or stretch the edge indefinitely.
        if (any(at < 0.0) || any(at > 1.0)) return local;
        float depth = 1.0-shadeMaterialSample(m,at,normals).a;
        if (layer >= depth) { high=layer; low=float(i-1)/12.0; found=true; break; }
    }
    if (!found) return local;
    for (uint i=0; i<3; ++i) {
        float layer=(low+high)*0.5;
        float2 at=local-delta*layer;
        if (layer >= 1.0-shadeMaterialSample(m,at,normals).a) high=layer; else low=layer;
    }
    hit = local-delta*((low+high)*0.5);
    return clamp(hit,float2(0.0),float2(1.0));
}
#endif
