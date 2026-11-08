/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Unwrap modifier (Edit Poly stack): UVs stored per face corner by stable IDs.
 */

#pragma once

struct Mesh;
struct UnwrapModifierData;

namespace blender::unwrap {

/** Name of the face attribute that marks faces without stored UVs on the modifier's result. */
constexpr const char *ATTR_NEW_FACE = ".unwrap_new";

/**
 * Store the UVs of `mesh` (a copy of the modifier's result, edited in a UV session): every face
 * by its stable ID. Replaces what was stored. Returns the number of faces stored.
 */
int session_commit(UnwrapModifierData &umd, const Mesh &mesh);

/** Remove all stored UVs. */
void clear(UnwrapModifierData &umd);

}  // namespace blender::unwrap
