/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Topology patches for the Edit Poly modifier (phase 4a of the Edit Poly stack).
 *
 * A patch describes how an Edit Mode session changed the topology of the mesh that arrives at the
 * modifier, entirely in terms of stable IDs:
 * - input vertices and faces that were removed;
 * - new vertices, each anchored to an input vertex (position = anchor position + offset), so new
 *   geometry follows when the shape below the modifier changes;
 * - new faces, as rings of vertex IDs (input or new), each with a source face to copy face and
 *   corner attributes from.
 *
 * If anything the patch refers to is missing from the input (e.g. the Subdivision level below
 * changed), the whole patch is paused, never applied partially.
 */

#pragma once

#include <string>

#include "BLI_array.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_attribute_enums.hh"
#include "BKE_stable_id.hh"

struct Mesh;

namespace blender::edit_poly {

using bke::stable_id::StableID;

struct Patch {
  Vector<StableID> removed_verts;
  Vector<StableID> removed_faces;

  Vector<StableID> new_vert_ids;
  Vector<StableID> new_vert_anchors;
  Vector<float3> new_vert_offsets;

  Vector<int> new_face_sizes;
  Vector<StableID> new_face_ids;
  Vector<StableID> new_face_sources;
  /** Vertex IDs of the corners of all new faces, face after face. */
  Vector<StableID> new_face_corner_verts;

  bool is_empty() const
  {
    return removed_verts.is_empty() && removed_faces.is_empty() && new_vert_ids.is_empty() &&
           new_face_sizes.is_empty();
  }
};

/** Flat storage used in #EditPolyModifierData (versioned, so the layout can change later). */
void patch_encode(const Patch &patch, Vector<int> &r_ints, Vector<float> &r_floats);
/** Returns false if the data is malformed; `r_patch` is then empty. */
bool patch_decode(Span<int> ints, Span<float> floats, Patch &r_patch);

/** Quick counts from encoded data, for the UI. Zero when there is no patch. */
int patch_encoded_new_verts_num(Span<int> ints);
int patch_encoded_new_faces_num(Span<int> ints);
int patch_encoded_removed_faces_num(Span<int> ints);

/**
 * Apply a patch to the mesh arriving at the modifier. Returns a new mesh with stable IDs written
 * for all domains, or null if the patch is paused; `r_reason` then says why.
 */
Mesh *patch_apply(const Mesh &input, const Patch &patch, std::string &r_reason);

/** A vertex move of an input vertex, found while computing a patch. */
struct KeptMove {
  StableID id;
  float3 offset;
};

/** Read session IDs (see #SESSION_ATTR_VERT_ID_LO) of a domain. False if they are missing. */
bool read_session_ids(const Mesh &mesh, bke::AttrDomain domain, Array<StableID> &r_ids);

/**
 * Compare a session mesh after editing with the input mesh at the start of the session.
 *
 * `edited` must carry the session attributes written by #session_prepare. `old_patch` is the
 * patch that was applied when the session started, so new elements from earlier sessions keep
 * their IDs and anchors.
 *
 * Fills the new patch, the moves of input vertices that were kept (`r_moves`, offset may be zero)
 * and the IDs of every input vertex (`r_input_vert_ids`). Returns an error message or empty.
 */
std::string patch_compute(const Mesh &input,
                          const Mesh &edited,
                          const Patch &old_patch,
                          Patch &r_patch,
                          Vector<KeptMove> &r_moves,
                          Vector<StableID> &r_input_vert_ids);

}  // namespace blender::edit_poly
