/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * UVW Map projections, shared by the UVW Map modifier and the UVW Map node (whose settings are
 * stored per object in the Geometry Nodes modifier, in the same struct).
 */

#pragma once

#include <string>

#include "BLI_index_mask_fwd.hh"

struct BlendDataReader;
struct BlendWriter;
struct Mesh;
struct Object;
struct PointerRNA;
struct UVWMapModifierData;

namespace blender::ui {
class Layout;
}

namespace blender::uvw_map {

/**
 * Write the projected UVs into `mesh`. `ob` is the evaluated object (for a gizmo object and real
 * world sizes). `face_selection` limits the faces further (null: all faces). Returns a warning,
 * or empty.
 */
std::string apply(const UVWMapModifierData &umd,
                  const Object *ob,
                  Mesh &mesh,
                  const IndexMask *face_selection);

/**
 * The settings of a UVW Map, for the modifier panel and the Geometry Nodes modifier panel.
 * `modifier_name` and `node_id` (-1 for a UVW Map modifier) are passed to the gizmo operators.
 * Nodes do not show Selected Faces Only: they have a Selection input.
 */
void draw_settings(ui::Layout &layout,
                   PointerRNA *ptr,
                   PointerRNA *ob_ptr,
                   const char *modifier_name,
                   int node_id);

/** Settings stored outside a modifier stack (UVW Map nodes); the modifier header is unused. */
UVWMapModifierData *data_new();
UVWMapModifierData *data_copy(const UVWMapModifierData &src);
void data_free(UVWMapModifierData *umd);
void data_blend_write(BlendWriter *writer, const UVWMapModifierData &umd);
/** Reads the struct a pointer refers to. */
void data_blend_read(BlendDataReader *reader, UVWMapModifierData *&umd);

}  // namespace blender::uvw_map
