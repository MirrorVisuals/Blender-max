/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Edit Mode sessions on an Edit Poly modifier (phases 3 and 4a of the Edit Poly stack).
 *
 * A session edits a copy of the modifier's current result. #session_prepare stamps that copy with
 * the stable IDs of its vertices and faces (as visible attributes, which survive the Edit Mode
 * round trip) and gives the input mesh IDs for every domain. After editing, #session_commit
 * compares the edited copy with the input: vertex moves become moves, topology changes become the
 * modifier's patch.
 */

#pragma once

#include <string>

struct EditPolyModifierData;
struct Mesh;

namespace blender::edit_poly {

/**
 * Session IDs as pairs of plain integer attributes (low and high 32 bits), so that Python can read
 * them from a BMesh in Edit Mode (it has no two-component integer layers).
 */
constexpr const char *SESSION_ATTR_VERT_ID_LO = "edit_poly_vertex_id_lo";
constexpr const char *SESSION_ATTR_VERT_ID_HI = "edit_poly_vertex_id_hi";
constexpr const char *SESSION_ATTR_FACE_ID_LO = "edit_poly_face_id_lo";
constexpr const char *SESSION_ATTR_FACE_ID_HI = "edit_poly_face_id_hi";

/**
 * `input` is the mesh as it arrives at the modifier; `mesh` is the modifier's result that will be
 * edited. Both are copies owned by the session.
 */
void session_prepare(Mesh &input, Mesh &mesh);

/**
 * Check that the modifier's topology edits apply to `input` (the mesh as it arrives at the
 * modifier). Editing a level whose edits are paused would replace them, so sessions are refused
 * then. Returns the reason, or an empty string when editing is fine.
 */
std::string session_check(const EditPolyModifierData &emd, const Mesh &input);

/**
 * Record the edits of a session mesh in `emd`. Moves of input vertices replace earlier moves of
 * those vertices; moves of points that are not in the input (paused moves) are kept. The patch is
 * replaced.
 *
 * `operations` is the log of tools used in the session, in the text format of
 * MOD_edit_poly_ops.hh. It is appended to the modifier's operation log, which is kept only if
 * replaying it on `input` gives the topology of `mesh`. Returns an error message, or empty.
 */
std::string session_commit(EditPolyModifierData &emd,
                           const Mesh &input,
                           const Mesh &mesh,
                           const std::string &operations);

}  // namespace blender::edit_poly
