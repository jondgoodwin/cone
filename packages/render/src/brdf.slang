// The render package's physically based shading functions, a Slang module
// (no entry point: imported by name, 'import brdf;'): the microfacet
// specular lobe (GGX distribution, Smith's height-correlated visibility,
// Schlick's Fresnel), the clear coat's visibility (Kelemen's), Belcour and
// Barla's thin-film interference, and what image-based lighting's
// prefiltering needs (Hammersley points, GGX and cosine importance sampling).
//
// Roughness is perceptual (0 smooth to 1 rough); the distributions take
// alpha = roughness squared, as glTF, Filament and Unreal do.
//
// Sources: Walter et al., "Microfacet Models for Refraction through Rough
// Surfaces", EGSR 2007 (GGX); Heitz, "Understanding the Masking-Shadowing
// Function in Microfacet-Based BRDFs", JCGT 3(2), 2014 (the height-correlated
// Smith term); Karis, "Real Shading in Unreal Engine 4", SIGGRAPH 2013 (the
// split sum and its sampling); Kelemen and Szirmay-Kalos, "A Microfacet Based
// Coupled Specular-Matte BRDF Model with Importance Sampling", Eurographics
// 2001 (the coat's visibility, as Filament uses it); Belcour and Barla, "A
// Practical Extension to Microfacet Theory for the Modeling of Varying
// Iridescence", ACM TOG 36(4), SIGGRAPH 2017, doi:10.1145/3072959.3073620
// (the thin film).

static const float PI = 3.14159265358979;

// The GGX (Trowbridge-Reitz) normal distribution, for alpha 'a'
float distributionGGX(float noh, float a) {
    float a2 = a * a;
    float d = noh * noh * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

// Smith's masking-shadowing for GGX, height-correlated, folded with the
// microfacet BRDF's 1 / (4 n.l n.v): the visibility term V
float visibilitySmithGGX(float nov, float nol, float a) {
    float a2 = a * a;
    float gv = nol * sqrt(nov * nov * (1.0 - a2) + a2);
    float gl = nov * sqrt(nol * nol * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-6);
}

// Kelemen's visibility, for a smooth clear coat: 1 / (4 (l.h)^2)
float visibilityKelemen(float loh) {
    return 0.25 / max(loh * loh, 1e-4);
}

// Schlick's Fresnel, from the reflectance at normal incidence 'f0'
float3 fresnelSchlick(float3 f0, float cosTheta) {
    float m = saturate(1.0 - cosTheta);
    float m2 = m * m;
    return f0 + (1.0 - f0) * (m2 * m2 * m);
}

float fresnelSchlick1(float f0, float cosTheta) {
    float m = saturate(1.0 - cosTheta);
    float m2 = m * m;
    return f0 + (1.0 - f0) * (m2 * m2 * m);
}

// The reflectance at normal incidence of an interface from index 1 into 'ior'
float iorToF0(float ior) {
    float r = (ior - 1.0) / (ior + 1.0);
    return r * r;
}

// ---- Thin-film interference (Belcour and Barla 2017) ----
//
// Light reflected by the top of a film (air to the film, index eta2) and by
// its bottom (the film to what is under it, eta3) interferes; which
// wavelengths add depends on the film's thickness and the angle. Belcour and
// Barla sum the film's reflections (Airy's series) as a Fourier series in the
// optical path difference, and integrate each term against the CIE colour
// matching functions in closed form, each fitted by Gaussians: so an RGB
// renderer gets the spectrum's colour without sampling wavelengths, and
// without aliasing as the thickness varies. The s and p polarizations are
// summed apart and averaged, each interface by the exact dielectric Fresnel
// equations with their phase shifts (0, or pi where the amplitude is
// negative). Two orders of the series beyond the first, as the paper's own
// code keeps.

// Exact dielectric Fresnel from index 'n1' into 'n2' at cosine 'ct1': the
// reflectance of s and p light, and each one's phase shift on reflection.
// 'ct2' is the cosine in the second medium (Snell's law), 0 past total
// internal reflection (then the reflectance is 1).
void fresnelDielectricSP(float ct1, float n1, float n2, out float2 r, out float2 phase, out float ct2) {
    float st2sq = (n1 / n2) * (n1 / n2) * (1.0 - ct1 * ct1);
    if (st2sq >= 1.0) {
        r = float2(1.0, 1.0);
        phase = float2(0.0, 0.0);
        ct2 = 0.0;
        return;
    }
    ct2 = sqrt(1.0 - st2sq);
    float rs = (n1 * ct1 - n2 * ct2) / (n1 * ct1 + n2 * ct2);
    float rp = (n2 * ct1 - n1 * ct2) / (n2 * ct1 + n1 * ct2);
    r = float2(rs * rs, rp * rp);
    phase = float2(rs < 0.0 ? PI : 0.0, rp < 0.0 ? PI : 0.0);
}

// The XYZ colour matching functions' Fourier transform at optical path
// difference 'opd' (nanometres), shifted in phase by 'shift', as linear
// Rec. 709 RGB: the paper's Gaussian fit (three lobes, and a second for X)
float3 filmSensitivity(float opd, float shift) {
    float phase = 2.0 * PI * opd * 1.0e-9;
    float3 val = float3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    float3 pos = float3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    float3 var = float3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    float3 xyz = val * sqrt(2.0 * PI * var) * cos(pos * phase + shift) * exp(-(phase * phase) * var);
    xyz.x += 9.7470e-14 * sqrt(2.0 * PI * 4.5282e+09) * cos(2.2399e+06 * phase + shift)
        * exp(-4.5282e+09 * phase * phase);
    xyz = xyz / 1.0685e-7;
    // CIE XYZ to linear sRGB (Rec. 709 primaries, D65)
    return float3(
         3.2404542 * xyz.x - 1.5371385 * xyz.y - 0.4985314 * xyz.z,
        -0.9692660 * xyz.x + 1.8760108 * xyz.y + 0.0415560 * xyz.z,
         0.0556434 * xyz.x - 0.2040259 * xyz.y + 1.0572252 * xyz.z);
}

// The reflectance, as RGB, of a film of index 'filmIor' and 'thickness'
// nanometres, over a dielectric of index 'baseIor', seen from air at cosine
// 'cosTheta' to its normal
float3 thinFilmReflectance(float cosTheta, float filmIor, float baseIor, float thickness) {
    float ct1 = clamp(cosTheta, 1e-4, 1.0);
    // The top interface, air into the film
    float2 r12;
    float2 phi12;
    float ct2;
    fresnelDielectricSP(ct1, 1.0, filmIor, r12, phi12, ct2);
    if (ct2 <= 0.0) {
        return float3(1.0, 1.0, 1.0);
    }
    float2 t121 = 1.0 - r12;
    float2 phi21 = PI - phi12;
    // The bottom, the film into the base
    float2 r23;
    float2 phi23;
    float ct3;
    fresnelDielectricSP(ct2, filmIor, baseIor, r23, phi23, ct3);
    // The path difference of one round trip through the film, and the phase
    float opd = 2.0 * filmIor * thickness * ct2;
    float2 phi2 = phi21 + phi23;
    // Airy's series: the first term, then the pairs of orders
    float2 r123 = clamp(r12 * r23, 1e-5, 0.9999);
    float2 sr123 = sqrt(r123);
    float2 rs = t121 * t121 * r23 / (1.0 - r123);
    float2 c0 = r12 + rs;
    float3 result = float3(1.0, 1.0, 1.0) * (0.5 * (c0.x + c0.y));
    float2 cm = rs - t121;
    for (int m = 1; m <= 2; m++) {
        cm = cm * sr123;
        float3 ss = 2.0 * filmSensitivity(float(m) * opd, float(m) * phi2.x);
        float3 sp = 2.0 * filmSensitivity(float(m) * opd, float(m) * phi2.y);
        result += 0.5 * (cm.x * ss + cm.y * sp);
    }
    return max(result, float3(0.0, 0.0, 0.0));
}

// ---- Sampling, for prefiltering an environment ----

// Point 'i' of 'n' of the Hammersley set in the unit square
float2 hammersley(uint i, uint n) {
    return float2(float(i) / float(n), float(reversebits(i)) * 2.3283064365386963e-10);
}

// Two unit vectors at right angles to 'n', and to each other
void tangentFrame(float3 n, out float3 t, out float3 b) {
    float3 up = abs(n.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    t = normalize(cross(up, n));
    b = cross(n, t);
}

// A half vector about 'n' drawn from GGX of alpha 'a' at 'xi' (Karis)
float3 sampleGGX(float2 xi, float a, float3 n) {
    float phi = 2.0 * PI * xi.x;
    float cosTheta = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    float3 t;
    float3 b;
    tangentFrame(n, t, b);
    return normalize(t * (sinTheta * cos(phi)) + b * (sinTheta * sin(phi)) + n * cosTheta);
}

// A direction about 'n' drawn in proportion to its cosine with 'n'
float3 sampleCosine(float2 xi, float3 n) {
    float phi = 2.0 * PI * xi.x;
    float r = sqrt(xi.y);
    float3 t;
    float3 b;
    tangentFrame(n, t, b);
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(1.0 - xi.y, 0.0)));
}

// The direction of texel coordinates 'uv' (0 to 1, u across, v down the
// rows) on cube face 'face' (Vulkan's and WebGPU's order: +x, -x, +y, -y,
// +z, -z, and each face's orientation as a cube view samples it)
float3 cubeDirection(uint face, float2 uv) {
    float s = uv.x * 2.0 - 1.0;
    float t = uv.y * 2.0 - 1.0;
    float3 d;
    if (face == 0) {
        d = float3(1.0, -t, -s);
    } else if (face == 1) {
        d = float3(-1.0, -t, s);
    } else if (face == 2) {
        d = float3(s, 1.0, t);
    } else if (face == 3) {
        d = float3(s, -1.0, -t);
    } else if (face == 4) {
        d = float3(s, -t, 1.0);
    } else {
        d = float3(-s, -t, -1.0);
    }
    return normalize(d);
}
