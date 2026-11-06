/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Re-runnable operations for the Edit Poly modifier (phase 4b of the Edit Poly stack).
 *
 * While a level is edited, extrude / inset / bevel are logged with their settings and the stable
 * IDs of the faces or edges they used. The log is kept only if replaying it on the level's input
 * gives the topology of the edited mesh, so it always explains the level's patch.
 *
 * When the patch cannot apply because the topology below changed (e.g. a Subdivision level), the
 * modifier finds the faces and edges that now cover the logged ones, using "footprints": the
 * logged element's parent face and its position inside that face (subdivision's
 * `.stable_subd_uv`), and re-runs the operations there. One extruded face then becomes the four
 * faces of the next level, extruded.
 *
 * Text format, one operation per line:
 *   EXTRUDE params=x,y,z faces=lo:hi,lo:hi [fp=...]
 *   INSET params=individual,thickness,depth,even,relative,interpolate,boundary,outset,edge_rail
 *         faces=... [fp=...]
 *   BEVEL params=offset,offset_type,segments,profile_type,profile,clamp,loop_slide,miter_outer,
 *         miter_inner,spread,vmesh,mark_seam,mark_sharp,material,face_strength
 *         edges=alo:ahi:blo:bhi,... [fp=...]
 * `fp` lists one footprint per element, separated by ';': parent lo:hi followed by u:v pairs, or
 * '-' when there is none. IDs are 32 bit halves, signed or unsigned.
 */

#pragma once

#include <string>

#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "BKE_stable_id.hh"

struct Mesh;

namespace blender::edit_poly {

using bke::stable_id::StableID;

enum class OpType : int8_t { Extrude, Inset, Bevel };

struct Footprint {
  StableID parent;
  Vector<float2> uv;
  bool is_valid() const
  {
    return parent.is_valid() && !uv.is_empty();
  }
};

struct OpRecord {
  OpType type = OpType::Extrude;
  Vector<float> params;
  /** Extrude, inset: face IDs. */
  Vector<StableID> faces;
  /** Bevel: edges as pairs of vertex IDs. */
  Vector<std::pair<StableID, StableID>> edges;
  /** One per face or edge, filled when the log is stored. */
  Vector<Footprint> footprints;

  int64_t elements_num() const
  {
    return type == OpType::Bevel ? edges.size() : faces.size();
  }
};

/** Parse the text format. Returns false (and an empty log) on malformed text. */
bool ops_parse(StringRef text, Vector<OpRecord> &r_ops);
std::string ops_to_text(Span<OpRecord> ops);

/** Fill the footprints of all elements from the mesh they were logged on. */
void ops_compute_footprints(const Mesh &input, MutableSpan<OpRecord> ops);
/** True if every element of every operation has a footprint. */
bool ops_can_retarget(Span<OpRecord> ops);

/**
 * Replay the operations on `input`. Elements are found by ID, or with `retarget` by footprint.
 * Returns a new mesh with stable IDs for all domains, or null with `r_reason` set.
 */
Mesh *ops_apply(const Mesh &input, Span<OpRecord> ops, bool retarget, std::string &r_reason);

}  // namespace blender::edit_poly
