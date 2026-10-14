// The render package's extra material lobes, a Slang module ('import lobes;'):
// cloth SHEEN and ANISOTROPIC specular, the pieces pbr.slang's extended
// fragment stage (fragmentExtended, compiled into pbrx.spv) adds to the base
// metallic-roughness lobe. Pure functions: no bindings.
//
// SHEEN, the soft bright rim of velvet, satin and dust-covered things, lit
// from grazing angles. The distribution is "Charlie" (Estevez and Kulla,
// "Production Friendly Microfacet Sheen BRDF", SIGGRAPH 2017 course notes:
// an inverted Gaussian-like cylinder of fibres, D = (2 + 1/a) sin^(1/a)(h) /
// 2 pi), with the visibility of Neubelt and Pettineo (Ready At Dawn, "Crafting
// a Next-Gen Material Pipeline for The Order: 1886", SIGGRAPH 2013:
// 1 / (4 (n.l + n.v - n.l n.v))), as Filament shades cloth. The energy the
// sheen takes from the layer under it is the sheen's own directional albedo
// (a table baked by capture.slang from the very same D and V), so a velvet
// never reflects more than it receives.
//
// ANISOTROPY, highlights drawn out along a grain: brushed metal, satin
// ribbon, hair. The distribution is Burley's anisotropic GGX ("Physically
// Based Shading at Disney", 2012, in Filament's form) and the visibility the
// height-correlated Smith for two roughnesses (Heitz, "Understanding the
// Masking-Shadowing Function in Microfacet-Based BRDFs", JCGT 2014), the
// roughness along the tangent and bitangent split by the anisotropy a in -1
// to 1: at = alpha (1 + a), ab = alpha (1 - a) (Kulla and Conty 2017).

static const float LOBES_PI = 3.14159265358979;

// Charlie sheen distribution at 'noh' for the roughness 'alpha' (the sheen
// roughness squared, as the base lobe's)
float sheenDistribution(float noh, float alpha) {
    float invAlpha = 1.0 / alpha;
    float cos2h = noh * noh;
    float sin2h = max(1.0 - cos2h, 0.0078125);
    return (2.0 + invAlpha) * pow(sin2h, invAlpha * 0.5) / (2.0 * LOBES_PI);
}

// Neubelt and Pettineo's sheen visibility
float sheenVisibility(float nov, float nol) {
    return saturate(1.0 / (4.0 * (nol + nov - nol * nov)));
}

// Anisotropic GGX distribution; 'at' and 'ab' the two alphas, 'toh' and 'boh' the
// half vector's components along the tangent and the bitangent
float distributionAnisotropic(float at, float ab, float toh, float boh, float noh) {
    float a2 = at * ab;
    float3 d = float3(ab * toh, at * boh, a2 * noh);
    float d2 = dot(d, d);
    float b2 = a2 / d2;
    return a2 * b2 * b2 * (1.0 / LOBES_PI);
}

// Anisotropic height-correlated Smith visibility (V = G / (4 n.l n.v))
float visibilityAnisotropic(float at, float ab, float tov, float bov, float tol, float bol, float nov, float nol) {
    float lambdaV = nol * length(float3(at * tov, ab * bov, nov));
    float lambdaL = nov * length(float3(at * tol, ab * bol, nol));
    return saturate(0.5 / (lambdaV + lambdaL));
}

// The two alphas of an anisotropic lobe of roughness 'alpha' and anisotropy
// 'anisotropy' (-1 to 1): positive makes the surface rougher along the tangent
// (the highlight spreads along it, across the grain that runs along the
// bitangent), negative the other way about
float2 anisotropicAlphas(float alpha, float anisotropy) {
    return float2(max(alpha * (1.0 + anisotropy), 0.002), max(alpha * (1.0 - anisotropy), 0.002));
}

// The reflection vector of an anisotropic surface, the normal bent towards
// the grain (McAuley, "Rendering the World of Far Cry 4", GDC 2015; Filament):
// the environment's highlight stretches the way the direct one does
float3 anisotropicReflection(float3 n, float3 t, float3 b, float3 v, float anisotropy) {
    float3 grain = anisotropy >= 0.0 ? b : t;
    float3 across = cross(grain, v);
    float3 bentNormal = cross(across, grain);
    bentNormal = normalize(lerp(n, bentNormal, abs(anisotropy)));
    return reflect(-v, bentNormal);
}
