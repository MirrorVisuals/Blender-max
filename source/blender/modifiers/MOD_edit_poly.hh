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

constexpr const char *SESSION_ATTR_VERT_ID = "edit_poly_vertex_id";
constexpr const char *SESSION_ATTR_FACE_ID = "edit_poly_face_id";

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
 * replaced. Returns an error message, or an empty string.
 */
std::string session_commit(EditPolyModifierData &emd, const Mesh &input, const Mesh &mesh);

}  // namespace blender::edit_poly
