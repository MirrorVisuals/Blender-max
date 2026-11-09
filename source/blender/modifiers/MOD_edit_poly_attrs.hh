/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Edit Poly: element data edited in a session (bevel weights, creases, sharp edges, seams,
 * material indices, flat shading), stored by stable ID and written on the mesh that arrives.
 *
 * Vertices and faces are found by their stable ID, edges by the stable IDs of their two
 * vertices. Encoding: ints = [version, records, then per record: attribute code, item count,
 * item keys (2 ints for a vertex or face, 4 for an edge)]; floats = the item values in order.
 */

#pragma once

#include "BLI_array.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

#include "BKE_stable_id.hh"

struct Mesh;

namespace blender::edit_poly {

using bke::stable_id::StableID;

/**
 * Data of `edited` (a session mesh) that differs from the same elements of `input`. Items of
 * the old encoding whose elements are not in `edited` are kept (paused).
 * \param edited_vert_ids, edited_face_ids: resulting stable ID of every element of `edited`.
 */
void attrs_compute(const Mesh &input,
                   const Mesh &edited,
                   Span<StableID> edited_vert_ids,
                   Span<StableID> edited_face_ids,
                   Span<int> old_ints,
                   Span<float> old_floats,
                   Vector<int> &r_ints,
                   Vector<float> &r_floats);

/**
 * Write the stored data on `mesh`, whose element IDs are `vert_ids` and `face_ids`. Returns the
 * number of items whose element is missing.
 */
int attrs_apply(Span<int> ints,
                Span<float> floats,
                Mesh &mesh,
                Span<StableID> vert_ids,
                Span<StableID> face_ids);

/** Number of stored items. */
int attrs_count(Span<int> ints);

}  // namespace blender::edit_poly
