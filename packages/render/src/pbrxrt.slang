// slangc: -fp-mode precise -capability spvRayQueryKHR
//
// The extended physically based material for a card with ray tracing: pbr.slang compiled with PBR_EXTENDED
// and PBR_RT, which gives fragmentExtended ray-traced glass, soap-film iridescence, glass seen through glass,
// reflections of what is off the screen and glass-tinted sunlight (rtglass.slang, included by pbr.slang). It is
// used in place of pbrx.spv, with the same vertex stages, the same four bind groups and the same material
// description; group 3 has four more bindings (the top-level structure and the records and geometry its hits
// are read from: raytrace.cone). A card without ray tracing never loads it and uses pbrx.spv.
//
// Like pbrx.slang, this file holds no code of its own: it names what pbr.slang imports, so that
// tools/shaders/shaders.py's hash of it covers those modules, and includes pbr.slang (whose entry points are
// [shader("vertex")] and [shader("fragment")] stages declared there; the tool takes this file for a shader
// because it reads that text here). rtglass.slang is
// included, not imported (it needs pbr.slang's own declarations), so an edit to it is caught by --check's
// recompile, as an edit to pbr.slang is.

import brdf;
import sky;
import noise;
import renderframe;
import skinning;
import fade;
import shadowing;
import flutter;
import locallights;
import lobes;
import scenecapture;

#define PBR_EXTENDED 1
#define PBR_RT 1
#include "pbr.slang"
