/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Edit Mode sessions on an Edit Poly modifier (phase 3 of the Edit Poly stack).
 *
 * A session edits a temporary mesh: the mesh as it arrives at the modifier, with the modifier's
 * moves applied. #session_prepare stamps every vertex with its stable ID and its position in the
 * incoming mesh. After editing, #session_commit turns the differences into moves.
 */

#pragma once

#include <string>

struct EditPolyModifierData;
struct Mesh;

namespace blender::edit_poly {

/** Generic (visible) attribute names, so they survive the Edit Mode round trip. */
constexpr const char *SESSION_ATTR_ID = "edit_poly_input_id";
constexpr const char *SESSION_ATTR_POSITION = "edit_poly_input_position";

/**
 * Store the stable ID and current position of every vertex of `mesh` in the session attributes.
 * Call on the mesh as it arrives at the modifier, before the modifier's moves are applied.
 */
void session_prepare(Mesh &mesh);

/**
 * Record the difference between each vertex's position and its session start position as a move
 * in `emd`. Vertices that are back at their start lose their move; moves of points that are not
 * in the session (paused moves) are kept. Returns an error message, or an empty string.
 */
std::string session_commit(EditPolyModifierData &emd, const Mesh &mesh);

}  // namespace blender::edit_poly
