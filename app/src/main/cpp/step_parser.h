// step_parser.h - STEP (ISO 10303-21) BREP geometry parser.
// Supported analytic surfaces: plane/cylinder/cone/sphere/torus,
// trim curves: line/circle/ellipse, loops with holes; tessellation
// in UV space with ear-clipping.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mesh.h"

/**
 * Parses .step/.stp. All ADVANCED_FACE entities are tessellated and merged
 * into a single MeshData (normalized, smooth normals computed).
 */
bool parseStepFile(const uint8_t* data, size_t size, MeshData& out, std::string& err);