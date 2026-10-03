// The vfx package's particle renderers: glowing sprites and ribbon trails,
// drawn additively into the HDR frame after the opaque scene (depth tested
// against it, not written) and before bloom and tone mapping, so what is
// bright glows. Compiled ahead of time to sprites.spv by
// tools/shaders/shaders.py and embedded in vfx.cone.
//
// Nothing is read from a vertex buffer and nothing is stored per particle:
// instance i of a draw is particle (first + i) of its emitter, evaluated
// where it is drawn by vfx.slang's particleNow, and the vertex index says
// which corner (of the quad, or of which segment of the trail).
//
// Group 0 is the frame's (the camera); group 1 the emitter's parameters.
//
// slangc: -fp-mode precise

import vfx;

struct Frame {
    float4x4 viewProjection;
    float4 cameraPosition;
    float4 cameraUp;            // the camera's up, in the world
};

[[vk::binding(0, 0)]] ConstantBuffer<Frame> frame;
[[vk::binding(0, 1)]] ConstantBuffer<EmitterParams> emitter;

// The segments of a ribbon trail
static const uint TRAIL_SEGMENTS = 8u;

struct VertexOutput {
    float4 position : SV_Position;
    float2 corner;              // in [-1, 1]: across, along
    float4 color;               // linear rgb times intensity
};

// A quad's corner, two triangles: (-1, -1), (1, -1), (1, 1), (-1, -1), (1, 1), (-1, 1)
float2 quadCorner(uint v) {
    float x = (v == 1u || v == 2u || v == 4u) ? 1.0 : -1.0;
    float y = (v == 2u || v == 4u || v == 5u) ? 1.0 : -1.0;
    return float2(x, y);
}

// Where nothing is drawn: every corner the same point, outside the view
VertexOutput nothing() {
    VertexOutput o;
    o.position = float4(2.0, 2.0, 2.0, 1.0);
    o.corner = float2(0.0, 0.0);
    o.color = float4(0.0, 0.0, 0.0, 0.0);
    return o;
}

// A sprite facing the camera, stretched along the particle's motion across
// the view: as long as its size plus the way it moves in 'stretch' seconds,
// its head at the particle
[shader("vertex")]
VertexOutput spriteVertex(uint vid : SV_VulkanVertexID, uint iid : SV_VulkanInstanceID) {
    if (iid >= emitter.seeds.w) { return nothing(); }
    Particle q = particleNow(emitter, emitter.seeds.z + iid);
    if (q.alive == 0.0 || q.color.w <= 0.0) { return nothing(); }
    float3 toCamera = normalize(frame.cameraPosition.xyz - q.position);
    float3 across = q.velocity - toCamera * dot(q.velocity, toCamera);
    float speed = length(across);
    float streak = speed * emitter.motion.z;
    float3 axis = frame.cameraUp.xyz;
    if (streak > 0.0001 * q.size) { axis = across / speed; }
    float3 side = normalize(cross(axis, toCamera));
    float2 c = quadCorner(vid);
    float half = 0.5 * q.size;
    float3 middle = q.position - axis * (0.5 * streak);
    float3 p = middle + side * (c.x * half) + axis * (c.y * (half + 0.5 * streak));
    VertexOutput o;
    o.position = mul(frame.viewProjection, float4(p, 1.0));
    o.corner = c;
    o.color = float4(q.color.xyz * q.color.w, 1.0);
    return o;
}

// A ribbon trail: the particle's own path over the last 'trail time'
// seconds, TRAIL_SEGMENTS quads facing the camera, as wide as the particle
// at its head and narrowing to nothing at its tail, dimming as it goes
[shader("vertex")]
VertexOutput trailVertex(uint vid : SV_VulkanVertexID, uint iid : SV_VulkanInstanceID) {
    if (iid >= emitter.seeds.w) { return nothing(); }
    uint index = emitter.seeds.z + iid;
    float age = emitter.timing.x - float(index) * emitter.timing.y;
    Particle head = particleAt(emitter, index, age);
    if (head.alive == 0.0 || head.color.w <= 0.0) { return nothing(); }
    uint segment = vid / 6u;
    float2 c = quadCorner(vid % 6u);
    // This corner's end of its segment: 0 the head, 1 the tail
    float along = (float(segment) + (c.y > 0.0 ? 0.0 : 1.0)) / float(TRAIL_SEGMENTS);
    float back = max(age - along * emitter.heat.w, 0.0);
    Particle q = particleAt(emitter, index, back);
    float3 toCamera = normalize(frame.cameraPosition.xyz - q.position);
    float3 direction = q.velocity;
    if (dot(direction, direction) < 1e-12) { direction = frame.cameraUp.xyz; }
    float3 side = normalize(cross(direction, toCamera));
    float width = 0.5 * head.size * (1.0 - along);
    float3 p = q.position + side * (c.x * width);
    VertexOutput o;
    o.position = mul(frame.viewProjection, float4(p, 1.0));
    o.corner = float2(c.x, 0.0);
    o.color = float4(head.color.xyz * (head.color.w * (1.0 - along)), 1.0);
    return o;
}

// A soft glow: brightest at the middle, falling to nothing at the edge
[shader("fragment")]
float4 glowFragment(VertexOutput input) : SV_Target {
    float r2 = dot(input.corner, input.corner);
    float glow = max(1.0 - r2, 0.0);
    glow = glow * glow;
    return float4(input.color.xyz * glow, 0.0);
}
