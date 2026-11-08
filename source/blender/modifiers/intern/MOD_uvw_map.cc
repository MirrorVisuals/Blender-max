/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * UVW Map modifier (experimental, Edit Poly stack), modelled on 3ds Max's UVW Map.
 *
 * Texture coordinates are projected from a gizmo: a box in object space given by a location,
 * rotation and size, or by another object (its transform and empty display size). The gizmo is
 * aligned to X, Y or Z, which picks the projection axis. With "Selected Faces Only", only the
 * faces selected in the Edit Poly level below (or in the mesh) are mapped and the others keep
 * their UVs, so several UVW Maps can map different parts of a mesh.
 */

#include <cmath>

#include "BLI_math_base.hh"
#include "BLI_math_euler.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_lib_query.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"

#include "DEG_depsgraph_build.hh"

namespace blender {

static constexpr float PI_F = 3.14159265358979323846f;

static void init_data(ModifierData *md)
{
  UVWMapModifierData *umd = reinterpret_cast<UVWMapModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(umd, modifier);
}

static void required_data_mask(ModifierData * /*md*/, CustomData_MeshMasks *r_cddata_masks)
{
  r_cddata_masks->lmask |= CD_MASK_PROP_FLOAT2;
}

static void foreach_ID_link(ModifierData *md, Object *ob, IDWalkFunc walk, void *user_data)
{
  UVWMapModifierData *umd = reinterpret_cast<UVWMapModifierData *>(md);
  walk(user_data, ob, reinterpret_cast<ID **>(&umd->gizmo_object), IDWALK_CB_NOP);
}

static void update_depsgraph(ModifierData *md, const ModifierUpdateDepsgraphContext *ctx)
{
  UVWMapModifierData *umd = reinterpret_cast<UVWMapModifierData *>(md);
  if (umd->gizmo_object != nullptr) {
    DEG_add_object_relation(
        ctx->node, umd->gizmo_object, DEG_OB_COMP_TRANSFORM, "UVW Map Modifier");
    DEG_add_depends_on_transform_relation(ctx->node, "UVW Map Modifier");
  }
}

namespace uvw_map {

/** Object space to gizmo space, where the gizmo box is [-1, 1] on every axis. */
static float4x4 object_to_gizmo(const UVWMapModifierData &umd, const Object *ob)
{
  float4x4 gizmo;
  if (umd.gizmo_object != nullptr && ob != nullptr) {
    const float size = umd.gizmo_object->type == OB_EMPTY ?
                           std::max(umd.gizmo_object->empty_drawsize, 1e-6f) :
                           1.0f;
    gizmo = math::invert(ob->object_to_world()) * umd.gizmo_object->object_to_world() *
            math::from_scale<float4x4>(float3(size));
  }
  else {
    float3 half;
    for (int i = 0; i < 3; i++) {
      const float s = umd.gizmo_size[i] * 0.5f;
      half[i] = std::abs(s) < 1e-6f ? (s < 0.0f ? -1e-6f : 1e-6f) : s;
    }
    gizmo = math::from_loc_rot_scale<float4x4>(float3(umd.gizmo_location),
                                               math::EulerXYZ(float3(umd.gizmo_rotation)),
                                               half);
  }
  return math::invert(gizmo);
}

/** Reorder gizmo axes so that the projection axis is the last one (right-handed). */
static float3 align(const float3 &q, const int axis)
{
  switch (axis) {
    case 0:
      return float3(q.y, q.z, q.x);
    case 1:
      return float3(q.x, q.z, -q.y);
    default:
      return q;
  }
}

static float2 planar(const float3 &q)
{
  return float2((q.x + 1.0f) * 0.5f, (q.y + 1.0f) * 0.5f);
}

static float wrap_u(const float3 &q)
{
  return std::atan2(q.y, q.x) / (2.0f * PI_F) + 0.5f;
}

/** Faces that cross the seam get the low side moved by one, so they do not span the texture. */
static void fix_seam(MutableSpan<float2> uvs)
{
  float lo = 1.0f, hi = 0.0f;
  for (const float2 &uv : uvs) {
    lo = std::min(lo, uv.x);
    hi = std::max(hi, uv.x);
  }
  if (hi - lo > 0.5f) {
    for (float2 &uv : uvs) {
      if (uv.x < 0.5f) {
        uv.x += 1.0f;
      }
    }
  }
}

/** Box: each face is projected from the side of the gizmo its normal points at most. */
static float2 box(const float3 &q, const float3 &n)
{
  const float3 a = math::abs(n);
  if (a.x >= a.y && a.x >= a.z) {
    return n.x >= 0.0f ? float2((q.y + 1.0f) * 0.5f, (q.z + 1.0f) * 0.5f) :
                         float2((1.0f - q.y) * 0.5f, (q.z + 1.0f) * 0.5f);
  }
  if (a.y >= a.z) {
    return n.y >= 0.0f ? float2((1.0f - q.x) * 0.5f, (q.z + 1.0f) * 0.5f) :
                         float2((q.x + 1.0f) * 0.5f, (q.z + 1.0f) * 0.5f);
  }
  return n.z >= 0.0f ? float2((q.x + 1.0f) * 0.5f, (q.y + 1.0f) * 0.5f) :
                       float2((1.0f - q.x) * 0.5f, (q.y + 1.0f) * 0.5f);
}

static float2 face_corner(const int k, const int n)
{
  if (n == 4) {
    static const float2 quad[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    return quad[k];
  }
  if (n == 3) {
    static const float2 tri[3] = {{0, 0}, {1, 0}, {1, 1}};
    return tri[k];
  }
  const float angle = 2.0f * PI_F * float(k) / float(n) - PI_F * 0.75f;
  return float2(0.5f + 0.5f * std::cos(angle), 0.5f + 0.5f * std::sin(angle));
}

/**
 * Compute the UVs of the given faces. `positions` are in object space. Returns false if the
 * mapping type is unknown.
 */
static void compute(const UVWMapModifierData &umd,
             const float4x4 &to_gizmo,
             const Span<float3> positions,
             const OffsetIndices<int> faces,
             const Span<int> corner_verts,
             const IndexMask &face_mask,
             MutableSpan<float2> r_uvs)
{
  Array<float3> q(positions.size());
  for (const int v : positions.index_range()) {
    q[v] = align(math::transform_point(to_gizmo, positions[v]), umd.axis);
  }
  const int type = umd.map_type;
  const bool cap = (umd.flag & MOD_UVWMAP_CAP) != 0;

  face_mask.foreach_index([&](const int f) {
    const IndexRange face = faces[f];
    const Span<int> verts = corner_verts.slice(face);
    MutableSpan<float2> uvs = r_uvs.slice(face);
    switch (type) {
      case MOD_UVWMAP_PLANAR:
        for (const int i : verts.index_range()) {
          uvs[i] = planar(q[verts[i]]);
        }
        break;
      case MOD_UVWMAP_BOX: {
        const float3 n = bke::mesh::face_normal_calc(q.as_span(), verts);
        for (const int i : verts.index_range()) {
          uvs[i] = box(q[verts[i]], n);
        }
        break;
      }
      case MOD_UVWMAP_CYLINDRICAL: {
        if (cap) {
          const float3 n = bke::mesh::face_normal_calc(q.as_span(), verts);
          if (std::abs(n.z) >= 0.7071f) {
            for (const int i : verts.index_range()) {
              uvs[i] = planar(q[verts[i]]);
            }
            break;
          }
        }
        for (const int i : verts.index_range()) {
          const float3 &p = q[verts[i]];
          uvs[i] = float2(wrap_u(p), (p.z + 1.0f) * 0.5f);
        }
        fix_seam(uvs);
        break;
      }
      case MOD_UVWMAP_SPHERICAL: {
        int on_pole = 0;
        for (const int i : verts.index_range()) {
          const float3 &p = q[verts[i]];
          const float len = math::length(p);
          const float z = len > 1e-12f ? std::clamp(p.z / len, -1.0f, 1.0f) : 0.0f;
          uvs[i] = float2(wrap_u(p), std::asin(z) / PI_F + 0.5f);
          if (p.x * p.x + p.y * p.y < 1e-12f) {
            on_pole++;
          }
        }
        fix_seam(uvs);
        if (on_pole > 0 && on_pole < int(verts.size())) {
          /* A corner on a pole has no direction: use the face's other corners. */
          float sum = 0.0f;
          for (const int i : verts.index_range()) {
            const float3 &p = q[verts[i]];
            if (p.x * p.x + p.y * p.y >= 1e-12f) {
              sum += uvs[i].x;
            }
          }
          const float u = sum / float(int(verts.size()) - on_pole);
          for (const int i : verts.index_range()) {
            const float3 &p = q[verts[i]];
            if (p.x * p.x + p.y * p.y < 1e-12f) {
              uvs[i].x = u;
            }
          }
        }
        break;
      }
      case MOD_UVWMAP_SHRINKWRAP:
        for (const int i : verts.index_range()) {
          const float3 &p = q[verts[i]];
          const float len = math::length(p);
          const float z = len > 1e-12f ? std::clamp(p.z / len, -1.0f, 1.0f) : 1.0f;
          const float r = std::acos(z) / PI_F;
          const float phi = std::atan2(p.y, p.x);
          uvs[i] = float2(0.5f + 0.5f * r * std::cos(phi), 0.5f + 0.5f * r * std::sin(phi));
        }
        break;
      case MOD_UVWMAP_FACE:
      default:
        for (const int i : verts.index_range()) {
          uvs[i] = face_corner(i, int(verts.size()));
        }
        break;
    }

    for (float2 &uv : uvs) {
      if (umd.flag & MOD_UVWMAP_FLIP_U) {
        uv.x = 1.0f - uv.x;
      }
      if (umd.flag & MOD_UVWMAP_FLIP_V) {
        uv.y = 1.0f - uv.y;
      }
      uv.x = uv.x * umd.tile[0] + umd.offset[0];
      uv.y = uv.y * umd.tile[1] + umd.offset[1];
    }
  });
}

}  // namespace uvw_map

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  UVWMapModifierData *umd = reinterpret_cast<UVWMapModifierData *>(md);
  if (mesh->faces_num == 0) {
    return mesh;
  }

  /* Which faces. */
  IndexMaskMemory memory;
  IndexMask face_mask = IndexMask(mesh->faces_num);
  if (umd->flag & MOD_UVWMAP_SELECTION) {
    const bke::AttributeAccessor attributes = mesh->attributes();
    const VArray<bool> select = *attributes.lookup_or_default<bool>(
        ".select_poly", bke::AttrDomain::Face, false);
    face_mask = IndexMask::from_bools(select, memory);
    if (face_mask.is_empty()) {
      BKE_modifier_set_warning(ctx->object, md, "No selected faces");
      return mesh;
    }
  }

  /* Which UV map. */
  std::string name = umd->uvmap_name;
  bool make_active = false;
  if (name.empty()) {
    name = mesh->active_or_default_uv_map_name();
    if (name.empty()) {
      name = "UVMap";
      make_active = true;
    }
  }
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  bke::SpanAttributeWriter<float2> uv = attributes.lookup_or_add_for_write_span<float2>(
      name, bke::AttrDomain::Corner);
  if (!uv) {
    BKE_modifier_set_warning(
        ctx->object, md, "\"%s\" is not a UV map (face corner 2D vector)", name.c_str());
    return mesh;
  }

  const float4x4 to_gizmo = uvw_map::object_to_gizmo(*umd, ctx->object);
  uvw_map::compute(*umd,
                   to_gizmo,
                   mesh->vert_positions(),
                   mesh->faces(),
                   mesh->corner_verts(),
                   face_mask,
                   uv.span);
  uv.finish();
  if (make_active && mesh->active_uv_map_name().is_empty()) {
    mesh->uv_maps_active_set(name);
    mesh->uv_maps_default_set(name);
  }
  return mesh;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  const ModifierData *md = static_cast<const ModifierData *>(ptr->data);
  PointerRNA obj_data_ptr = RNA_pointer_get(&ob_ptr, "data");

  layout.use_property_split_set(true);
  layout.prop(ptr, "map_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "map_type") == MOD_UVWMAP_CYLINDRICAL) {
    layout.prop(ptr, "use_cap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "axis", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  layout.prop_search(ptr, "uv_layer", &obj_data_ptr, "uv_layers", std::nullopt, ICON_GROUP_UVS);
  layout.prop(ptr, "use_selection", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  ui::Layout *col = &layout.column(true);
  col->prop(ptr, "tile", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col = &layout.column(true);
  col->prop(ptr, "offset", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  ui::Layout *row = &layout.row(true, IFACE_("Flip"));
  row->prop(ptr, "use_flip_u", ui::ITEM_R_TOGGLE, IFACE_("U"), ICON_NONE);
  row->prop(ptr, "use_flip_v", ui::ITEM_R_TOGGLE, IFACE_("V"), ICON_NONE);

  layout.separator();
  layout.use_property_split_set(false);
  row = &layout.row(true);
  PointerRNA op_ptr = row->op(
      "OBJECT_OT_uvw_map_gizmo_edit", IFACE_("Edit Gizmo"), ICON_OBJECT_ORIGIN);
  RNA_string_set(&op_ptr, "modifier", md->name);
  op_ptr = row->op("OBJECT_OT_uvw_map_fit", IFACE_("Fit"), ICON_NONE);
  RNA_string_set(&op_ptr, "modifier", md->name);
  op_ptr = row->op("OBJECT_OT_uvw_map_center", IFACE_("Center"), ICON_NONE);
  RNA_string_set(&op_ptr, "modifier", md->name);
  op_ptr = row->op("OBJECT_OT_uvw_map_view_align", IFACE_("View Align"), ICON_NONE);
  RNA_string_set(&op_ptr, "modifier", md->name);
  layout.use_property_split_set(true);

  layout.prop(ptr, "gizmo_object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  const PointerRNA gizmo_ptr = RNA_pointer_get(ptr, "gizmo_object");
  const bool has_object = !RNA_pointer_is_null(&gizmo_ptr);
  col = &layout.column(false);
  col->active_set(!has_object);
  col->prop(ptr, "gizmo_location", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(ptr, "gizmo_rotation", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(ptr, "gizmo_size", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_UVWMap, panel_draw);
}

ModifierTypeInfo modifierType_UVWMap = {
    /*idname*/ "UVWMap",
    /*name*/ N_("UVWMap"),
    /*struct_name*/ "UVWMapModifierData",
    /*struct_size*/ sizeof(UVWMapModifierData),
    /*srna*/ &RNA_UVWMapModifier,
    /*type*/ ModifierTypeType::NonGeometrical,
    /*flags*/
    (eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsMapping |
     eModifierTypeFlag_SupportsEditmode | eModifierTypeFlag_EnableInEditmode),
    /*icon*/ ICON_MOD_UVPROJECT,

    /*copy_data*/ BKE_modifier_copydata_generic,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ required_data_mask,
    /*free_data*/ nullptr,
    /*is_disabled*/ nullptr,
    /*update_depsgraph*/ update_depsgraph,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ foreach_ID_link,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
