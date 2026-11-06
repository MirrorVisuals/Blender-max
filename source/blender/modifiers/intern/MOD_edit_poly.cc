/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Edit Poly modifier (experimental, phase 2 of the Edit Poly stack).
 *
 * Stores vertex moves against stable vertex IDs (see BKE_stable_id.hh) and replays them on the
 * mesh that arrives at the modifier. Because the moves are found by ID rather than by index, they
 * stay on the same points when modifiers below change, e.g. a Subdivision level. A move whose
 * point does not exist in the incoming mesh is paused, not deleted: it applies again as soon as
 * the point comes back.
 */

#include <cstring>
#include <string>

#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_screen.hh"
#include "BKE_stable_id_mesh.hh"

#include "BLO_read_write.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MEM_guardedalloc.h"

#include "MOD_edit_poly.hh"
#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Edit Mode sessions
 * \{ */

namespace edit_poly {

void session_prepare(Mesh &mesh)
{
  using namespace bke::stable_id;
  const Array<StableID> ids = read_or_make_root_ids(mesh, bke::AttrDomain::Point);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  attributes.remove(SESSION_ATTR_ID);
  attributes.remove(SESSION_ATTR_POSITION);

  bke::SpanAttributeWriter<int2> id_writer = attributes.lookup_or_add_for_write_only_span<int2>(
      SESSION_ATTR_ID, bke::AttrDomain::Point);
  for (const int i : ids.index_range()) {
    id_writer.span[i] = int2(int(ids[i].lo), int(ids[i].hi));
  }
  id_writer.finish();

  bke::SpanAttributeWriter<float3> pos_writer =
      attributes.lookup_or_add_for_write_only_span<float3>(SESSION_ATTR_POSITION,
                                                           bke::AttrDomain::Point);
  pos_writer.span.copy_from(mesh.vert_positions());
  pos_writer.finish();
}

std::string session_commit(EditPolyModifierData &emd, const Mesh &mesh)
{
  const bke::AttributeAccessor attributes = mesh.attributes();
  const bke::AttributeReader<int2> id_reader = attributes.lookup<int2>(SESSION_ATTR_ID);
  const bke::AttributeReader<float3> pos_reader = attributes.lookup<float3>(
      SESSION_ATTR_POSITION);
  if (!id_reader || !pos_reader || id_reader.domain != bke::AttrDomain::Point ||
      pos_reader.domain != bke::AttrDomain::Point)
  {
    return "This mesh is not an Edit Poly session";
  }
  const VArraySpan<int2> ids = *id_reader;
  const VArraySpan<float3> start = *pos_reader;
  const Span<float3> positions = mesh.vert_positions();

  /* Vertices added in Edit Mode copy or interpolate the attributes, which gives duplicate or
   * empty IDs. Topology changes are not recorded yet, so refuse rather than store wrong moves. */
  Set<int64_t> seen;
  for (const int i : ids.index_range()) {
    const int64_t key = (int64_t(uint32_t(ids[i].y)) << 32) | uint32_t(ids[i].x);
    if (key == 0 || !seen.add(key)) {
      return "Topology was changed (vertices added, merged or removed). The Edit Poly modifier "
             "only records moves so far: undo the topology change, then leave Edit Mode";
    }
  }

  /* Existing moves, keyed by ID. */
  Vector<int2> move_ids;
  Vector<float3> move_offsets;
  Map<int64_t, int> index_of;
  for (int i = 0; i < emd.moves_num; i++) {
    const int2 id(emd.move_ids[2 * i], emd.move_ids[2 * i + 1]);
    index_of.add((int64_t(uint32_t(id.y)) << 32) | uint32_t(id.x), int(move_ids.size()));
    move_ids.append(id);
    move_offsets.append(float3(emd.move_offsets[3 * i],
                               emd.move_offsets[3 * i + 1],
                               emd.move_offsets[3 * i + 2]));
  }
  Vector<bool> removed(move_ids.size(), false);

  for (const int i : ids.index_range()) {
    const int64_t key = (int64_t(uint32_t(ids[i].y)) << 32) | uint32_t(ids[i].x);
    const float3 offset = positions[i] - start[i];
    const bool moved = math::length_squared(offset) > 1e-12f;
    if (const int *existing = index_of.lookup_ptr(key)) {
      move_offsets[*existing] = offset;
      removed[*existing] = !moved;
    }
    else if (moved) {
      index_of.add(key, int(move_ids.size()));
      move_ids.append(ids[i]);
      move_offsets.append(offset);
      removed.append(false);
    }
  }

  int num = 0;
  for (const int i : move_ids.index_range()) {
    num += removed[i] ? 0 : 1;
  }
  MEM_SAFE_DELETE(emd.move_ids);
  MEM_SAFE_DELETE(emd.move_offsets);
  emd.moves_num = 0;
  if (num == 0) {
    return "";
  }
  emd.move_ids = MEM_new_array_uninitialized<int>(size_t(num) * 2, __func__);
  emd.move_offsets = MEM_new_array_uninitialized<float>(size_t(num) * 3, __func__);
  int j = 0;
  for (const int i : move_ids.index_range()) {
    if (removed[i]) {
      continue;
    }
    emd.move_ids[2 * j] = move_ids[i].x;
    emd.move_ids[2 * j + 1] = move_ids[i].y;
    emd.move_offsets[3 * j] = move_offsets[i].x;
    emd.move_offsets[3 * j + 1] = move_offsets[i].y;
    emd.move_offsets[3 * j + 2] = move_offsets[i].z;
    j++;
  }
  emd.moves_num = num;
  return "";
}

}  // namespace edit_poly

/** \} */

static void init_data(ModifierData *md)
{
  EditPolyModifierData *emd = reinterpret_cast<EditPolyModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(emd, modifier);
}

static void copy_data(const ModifierData *md, ModifierData *target, const int flag)
{
  const EditPolyModifierData *emd = reinterpret_cast<const EditPolyModifierData *>(md);
  EditPolyModifierData *temd = reinterpret_cast<EditPolyModifierData *>(target);

  BKE_modifier_copydata_generic(md, target, flag);

  temd->move_ids = nullptr;
  temd->move_offsets = nullptr;
  temd->moves_num = 0;
  if (emd->moves_num > 0 && emd->move_ids && emd->move_offsets) {
    temd->move_ids = MEM_new_array_uninitialized<int>(size_t(emd->moves_num) * 2, __func__);
    temd->move_offsets = MEM_new_array_uninitialized<float>(size_t(emd->moves_num) * 3, __func__);
    memcpy(temd->move_ids, emd->move_ids, sizeof(int) * 2 * emd->moves_num);
    memcpy(temd->move_offsets, emd->move_offsets, sizeof(float) * 3 * emd->moves_num);
    temd->moves_num = emd->moves_num;
  }
}

static void free_data(ModifierData *md)
{
  EditPolyModifierData *emd = reinterpret_cast<EditPolyModifierData *>(md);
  MEM_SAFE_DELETE(emd->move_ids);
  MEM_SAFE_DELETE(emd->move_offsets);
  emd->moves_num = 0;
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const EditPolyModifierData *emd = reinterpret_cast<const EditPolyModifierData *>(md);
  return emd->moves_num == 0;
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  using namespace bke::stable_id;
  EditPolyModifierData *emd = reinterpret_cast<EditPolyModifierData *>(md);
  if (emd->moves_num == 0 || mesh->verts_num == 0) {
    return mesh;
  }

  /* Without IDs from a modifier below, the incoming mesh is the base mesh: use root IDs. */
  const Array<StableID> ids = read_or_make_root_ids(*mesh, bke::AttrDomain::Point);
  const IDIndex index(ids.data(), ids.size());

  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  int paused = 0;
  for (int i = 0; i < emd->moves_num; i++) {
    const StableID id{uint32_t(emd->move_ids[2 * i]), uint32_t(emd->move_ids[2 * i + 1])};
    const int vert = index.find(id);
    if (vert == -1) {
      paused++;
      continue;
    }
    positions[vert] += float3(emd->move_offsets[3 * i],
                              emd->move_offsets[3 * i + 1],
                              emd->move_offsets[3 * i + 2]);
  }
  mesh->tag_positions_changed();

  if (paused > 0) {
    BKE_modifier_set_warning(ctx->object,
                             md,
                             "%d of %d moves paused (their points are not in the mesh below)",
                             paused,
                             emd->moves_num);
  }
  return mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);

  layout.use_property_split_set(true);
  layout.prop(ptr, "moves_num", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.label(IFACE_("Tab to edit this level (moves only for now)"), ICON_INFO);

  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_EditPoly, panel_draw);
}

static void blend_write(BlendWriter *writer, const ID * /*id_owner*/, const ModifierData *md)
{
  const EditPolyModifierData *emd = reinterpret_cast<const EditPolyModifierData *>(md);
  writer->write_struct(emd);
  if (emd->moves_num > 0 && emd->move_ids && emd->move_offsets) {
    writer->write_int32_array(int64_t(emd->moves_num) * 2, emd->move_ids);
    writer->write_float3_array(emd->moves_num, emd->move_offsets);
  }
}

static void blend_read(BlendDataReader *reader, ModifierData *md)
{
  EditPolyModifierData *emd = reinterpret_cast<EditPolyModifierData *>(md);
  int ids_num = emd->moves_num;
  int offsets_num = emd->moves_num;
  BLO_read_array_and_validate_size(reader, &emd->move_ids, &ids_num, 2);
  BLO_read_array_and_validate_size(reader, &emd->move_offsets, &offsets_num, 3);
  if (ids_num != emd->moves_num || offsets_num != emd->moves_num || !emd->move_ids ||
      !emd->move_offsets)
  {
    MEM_SAFE_DELETE(emd->move_ids);
    MEM_SAFE_DELETE(emd->move_offsets);
    emd->moves_num = 0;
  }
}

ModifierTypeInfo modifierType_EditPoly = {
    /*idname*/ "EditPoly",
    /*name*/ N_("EditPoly"),
    /*struct_name*/ "EditPolyModifierData",
    /*struct_size*/ sizeof(EditPolyModifierData),
    /*srna*/ &RNA_EditPolyModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/
    (eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsMapping |
     eModifierTypeFlag_SupportsEditmode | eModifierTypeFlag_EnableInEditmode),
    /*icon*/ ICON_EDITMODE_HLT,

    /*copy_data*/ copy_data,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ nullptr,
    /*free_data*/ free_data,
    /*is_disabled*/ is_disabled,
    /*update_depsgraph*/ nullptr,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ nullptr,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register,
    /*blend_write*/ blend_write,
    /*blend_read*/ blend_read,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
