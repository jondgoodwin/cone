// slangc: -fp-mode precise
//
// The extended physically based material: pbr.slang compiled with
// PBR_EXTENDED, which adds fragmentExtended (sheen, anisotropy, subsurface,
// screen-space reflections, glass) beside the vertex stages and sky stages
// it already has. The base pbr.spv is compiled without it and is untouched,
// so every material that asks for none of these draws with it exactly as
// before; a material that asks for one draws with this module's pipelines
// (scenecapture.cone) and its fourth bind group, the capture.
//
// This file holds no code of its own: it names what pbr.slang imports, so
// that tools/shaders/shaders.py's hash of it covers those modules too, and
// includes pbr.slang itself. (Its entry points are [shader("vertex")] and
// [shader("fragment")] stages declared there; the tool takes this file for a
// shader because it reads that text here.) pbr.slang's own text is not part of
// this file's hash: 'shaders.py --check' compiles it again and compares the
// bytes where slangc is found, which catches an edit to it.

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
#include "pbr.slang"
