// The render package's frame, a Slang module ('import renderframe;'): what
// every material's shader, the package's own and a custom pipeline's, reads
// from group 0, written by the renderer once a render.
//
// THE LIGHTING CONVENTION, one for every material:
// - the light's colour ('lightColor') is the irradiance it gives a surface
//   facing it head on: lux, in a scene in physical units (the sun at noon is
//   about 100 000);
// - the ambient light ('ambient') and the environment are radiance: what a
//   surface open to the whole sky sees in every direction, cd/m2 (nits); a
//   white Lambertian surface under it is exactly that bright;
// - a material's emissive colour is radiance too, nits;
// - so a diffuse surface's radiance from the light is its albedo / pi times
//   the irradiance times the cosine (Lambert), and from the ambient light its
//   albedo times the radiance;
// - everything a material outputs is multiplied by 'exposure' before it is
//   written (pre-exposure, as Frostbite does), so the half-float frame holds
//   numbers near 1 however bright the scene, and nothing overflows it.
//   'exposureFromEv100' (camera.cone) gives the exposure of a camera at an
//   EV100; a scene in display units (a light of about 1) uses exposure 1.
//
// The Frame's layout is render.cone's 'frame' array, FRAME_BYTES long.

struct Frame {
    float4x4 viewProjection;       // 0
    float4 cameraPosition;         // 64: xyz, the camera in world coordinates
    float4 lightDirection;         // 80: xyz towards the light, unit length
    float4 lightColor;             // 96: rgb, irradiance head on
    float4 ambient;                // 112: rgb, radiance
    float4x4 inverseViewProjection;   // 128
    float4 viewport;               // 192: width, height in pixels; time in seconds; the environment's strength
    float4 exposure;               // 208: x the exposure every output is multiplied by
};

[[vk::binding(0, 0)]] ConstantBuffer<Frame> frame;

static const float RENDER_PI = 3.14159265358979;

// The normal 'n' (model coordinates) in world coordinates under 'model':
// by the cofactor matrix of its upper 3 x 3 (columns a0, a1, a2), the
// inverse transpose times the determinant, so right under non-uniform
// scale; normalize it in the fragment stage
float3 worldNormal(float4x4 model, float3 n) {
    float3 a0 = float3(model[0][0], model[1][0], model[2][0]);
    float3 a1 = float3(model[0][1], model[1][1], model[2][1]);
    float3 a2 = float3(model[0][2], model[1][2], model[2][2]);
    return cross(a1, a2) * n.x + cross(a2, a0) * n.y + cross(a0, a1) * n.z;
}
