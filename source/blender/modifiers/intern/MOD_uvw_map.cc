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
#include "BLI_string.h"

#include "BLT_translation.hh"

#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_lib_query.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"

#include "BLO_read_write.hh"

#include "MEM_guardedalloc.h"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_uvw_map.hh"

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
  }
  /* The gizmo object is used relative to the object; real-world size uses its scale. */
  if (umd->gizmo_object != nullptr || umd->size_mode != MOD_UVWMAP_SIZE_GIZMO) {
    DEG_add_depends_on_transform_relation(ctx->node, "UVW Map Modifier");
  }
}

namespace uvw_map {

/** Gizmo space (the gizmo box is [-1, 1] on every axis) to object space. */
static float4x4 gizmo_to_object(const UVWMapModifierData &umd, const Object *ob)
{
  if (umd.gizmo_object != nullptr && ob != nullptr) {
    const float size = umd.gizmo_object->type == OB_EMPTY ?
                           std::max(umd.gizmo_object->empty_drawsize, 1e-6f) :
                           1.0f;
    return math::invert(ob->object_to_world()) * umd.gizmo_object->object_to_world() *
           math::from_scale<float4x4>(float3(size));
  }
  float3 half;
  for (int i = 0; i < 3; i++) {
    const float s = umd.gizmo_size[i] * 0.5f;
    half[i] = std::abs(s) < 1e-6f ? (s < 0.0f ? -1e-6f : 1e-6f) : s;
  }
  return math::from_loc_rot_scale<float4x4>(
      float3(umd.gizmo_location), math::EulerXYZ(float3(umd.gizmo_rotation)), half);
}

/** Size of the map in the world: gizmo lengths and the size of one texture tile. */
struct Sizing {
  bool active = false;
  /** Full world length of the gizmo along the aligned U, V and W axes. */
  float3 len = float3(1.0f);
  /** World width and height of one texture tile. */
  float2 tile = float2(1.0f);
};

static Sizing sizing(const UVWMapModifierData &umd, const float4x4 &gizmo, const Object *ob)
{
  Sizing r;
  const int mode = umd.size_mode;
  if (mode == MOD_UVWMAP_SIZE_REAL_WORLD) {
    r.tile = float2(std::max(umd.real_size[0], 1e-6f), std::max(umd.real_size[1], 1e-6f));
  }
  else if (mode == MOD_UVWMAP_SIZE_PIXEL_DENSITY && umd.texel_density > 0.0f &&
           umd.texture_size > 0)
  {
    r.tile = float2(float(umd.texture_size) / umd.texel_density);
  }
  else {
    return r;
  }
  r.active = true;
  const float3x3 m = (ob ? float3x3(ob->object_to_world()) : float3x3::identity()) *
                     float3x3(gizmo);
  const float3 len(2.0f * math::length(m.x_axis()),
                   2.0f * math::length(m.y_axis()),
                   2.0f * math::length(m.z_axis()));
  switch (umd.axis) {
    case 0:
      r.len = float3(len.y, len.z, len.x);
      break;
    case 1:
      r.len = float3(len.x, len.z, len.y);
      break;
    default:
      r.len = len;
      break;
  }
  return r;
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
                    const Sizing &size,
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
    /* Real-world size: how many texture tiles the 0..1 range is, per face (world length of the
     * mapped direction / tile width or height). */
    const float3 &L = size.len;
    const float around = PI_F * (L.x + L.y) * 0.5f;
    auto tiles = [&](const float u_len, const float v_len) {
      return size.active ? float2(u_len / size.tile.x, v_len / size.tile.y) : float2(1.0f);
    };
    float2 face_scale = tiles(L.x, L.y);
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
        const float3 a = math::abs(n);
        if (a.x >= a.y && a.x >= a.z) {
          face_scale = tiles(L.y, L.z);
        }
        else if (a.y >= a.z) {
          face_scale = tiles(L.x, L.z);
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
        face_scale = tiles(around, L.z);
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
        face_scale = tiles(around, PI_F * L.z * 0.5f);
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
        face_scale = tiles(around, around);
        break;
      case MOD_UVWMAP_FACE:
      default:
        for (const int i : verts.index_range()) {
          uvs[i] = face_corner(i, int(verts.size()));
        }
        face_scale = float2(1.0f);
        break;
    }

    for (float2 &uv : uvs) {
      if (umd.flag & MOD_UVWMAP_FLIP_U) {
        uv.x = 1.0f - uv.x;
      }
      if (umd.flag & MOD_UVWMAP_FLIP_V) {
        uv.y = 1.0f - uv.y;
      }
      uv *= face_scale;
      uv.x = uv.x * umd.tile[0] + umd.offset[0];
      uv.y = uv.y * umd.tile[1] + umd.offset[1];
    }
  });
}

}  // namespace uvw_map

namespace uvw_map {

std::string apply(const UVWMapModifierData &umd,
                  const Object *ob,
                  Mesh &mesh,
                  const IndexMask *face_selection)
{
  if (mesh.faces_num == 0) {
    return "";
  }

  /* Which faces. */
  IndexMaskMemory memory;
  IndexMask face_mask = face_selection ? *face_selection : IndexMask(mesh.faces_num);
  if (face_mask.is_empty()) {
    return "No faces in the selection";
  }
  if (umd.flag & MOD_UVWMAP_SELECTION) {
    const bke::AttributeAccessor attributes = mesh.attributes();
    const VArraySpan<bool> select = *attributes.lookup_or_default<bool>(
        ".select_poly", bke::AttrDomain::Face, false);
    face_mask = IndexMask::from_predicate(
        face_mask, memory, [&](const int f) { return select[f]; });
    if (face_mask.is_empty()) {
      return "No selected faces";
    }
  }
  if (umd.flag & MOD_UVWMAP_MATERIAL) {
    /* Only faces with one material (as a material ID in 3ds Max). */
    const VArraySpan<int> material = *mesh.attributes().lookup_or_default<int>(
        "material_index", bke::AttrDomain::Face, 0);
    const int wanted = umd.material_index;
    face_mask = IndexMask::from_predicate(
        face_mask, memory, [&](const int f) { return material[f] == wanted; });
    if (face_mask.is_empty()) {
      return "No faces with this material";
    }
  }

  /* Which UV map. */
  std::string name = umd.uvmap_name;
  bool make_active = false;
  if (name.empty()) {
    name = mesh.active_or_default_uv_map_name();
    if (name.empty()) {
      name = "UVMap";
      make_active = true;
    }
  }
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  bke::SpanAttributeWriter<float2> uv = attributes.lookup_or_add_for_write_span<float2>(
      name, bke::AttrDomain::Corner);
  if (!uv) {
    return "\"" + name + "\" is not a UV map (face corner 2D vector)";
  }

  const float4x4 gizmo = gizmo_to_object(umd, ob);
  const Sizing size = sizing(umd, gizmo, ob);
  compute(umd,
          math::invert(gizmo),
          size,
          mesh.vert_positions(),
          mesh.faces(),
          mesh.corner_verts(),
          face_mask,
          uv.span);
  uv.finish();
  if (make_active && mesh.active_uv_map_name().is_empty()) {
    mesh.uv_maps_active_set(name);
    mesh.uv_maps_default_set(name);
  }
  return "";
}

}  // namespace uvw_map

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  const UVWMapModifierData *umd = reinterpret_cast<const UVWMapModifierData *>(md);
  const std::string warning = uvw_map::apply(*umd, ctx->object, *mesh, nullptr);
  if (!warning.empty()) {
    BKE_modifier_set_warning(ctx->object, md, "%s", warning.c_str());
  }
  return mesh;
}

namespace uvw_map {

void draw_settings(ui::Layout &layout,
                   PointerRNA *ptr,
                   PointerRNA *ob_ptr,
                   const char *modifier_name,
                   const int node_id)
{
  const auto set_target = [&](PointerRNA &op_ptr) {
    RNA_string_set(&op_ptr, "modifier", modifier_name);
    RNA_int_set(&op_ptr, "node_id", node_id);
  };

  layout.use_property_split_set(true);
  layout.prop(ptr, "map_type", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  if (RNA_enum_get(ptr, "map_type") == MOD_UVWMAP_CYLINDRICAL) {
    layout.prop(ptr, "use_cap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "axis", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);
  modifier_uv_map_channel_draw(layout, ptr, ob_ptr, modifier_name, node_id);
  if (node_id < 0) {
    /* A UVW Map node has a Selection input instead. */
    layout.prop(ptr, "use_selection", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }

  {
    ui::Layout &row_mat = layout.row(true, IFACE_("Material"));
    row_mat.prop(ptr, "use_material", UI_ITEM_NONE, "", ICON_NONE);
    ui::Layout &sub = row_mat.row(true);
    sub.active_set(RNA_boolean_get(ptr, "use_material"));
    sub.prop(ptr, "material_slot", UI_ITEM_NONE, "", ICON_NONE);
  }

  layout.prop(ptr, "size_mode", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  const int mode = RNA_enum_get(ptr, "size_mode");
  if (mode == MOD_UVWMAP_SIZE_REAL_WORLD) {
    ui::Layout &rw = layout.column(true);
    rw.prop(ptr, "real_size", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else if (mode == MOD_UVWMAP_SIZE_PIXEL_DENSITY) {
    ui::Layout &pd = layout.column(true);
    pd.prop(ptr, "texture_size_preset", UI_ITEM_NONE, IFACE_("Texture"), ICON_NONE);
    if (RNA_enum_get(ptr, "texture_size_preset") == 0) {
      pd.prop(ptr, "texture_size", UI_ITEM_NONE, IFACE_("Pixels"), ICON_NONE);
    }
    ui::Layout &pd2 = layout.column(true);
    pd2.prop(ptr, "texel_density_preset", UI_ITEM_NONE, IFACE_("Density"), ICON_NONE);
    if (RNA_enum_get(ptr, "texel_density_preset") == 0) {
      pd2.prop(ptr, "texel_density", UI_ITEM_NONE, IFACE_("Pixels per Meter"), ICON_NONE);
    }
    const float density = RNA_float_get(ptr, "texel_density");
    if (density > 0.0f) {
      char label[64];
      SNPRINTF(label,
               IFACE_("One texture tile = %.3g m"),
               float(RNA_int_get(ptr, "texture_size")) / density);
      layout.label(label, ICON_INFO);
    }
  }

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
  set_target(op_ptr);
  op_ptr = row->op("OBJECT_OT_uvw_map_fit", IFACE_("Fit"), ICON_NONE);
  set_target(op_ptr);
  op_ptr = row->op("OBJECT_OT_uvw_map_center", IFACE_("Center"), ICON_NONE);
  set_target(op_ptr);
  op_ptr = row->op("OBJECT_OT_uvw_map_view_align", IFACE_("View Align"), ICON_NONE);
  set_target(op_ptr);
  layout.use_property_split_set(true);

  layout.prop(ptr, "gizmo_object", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  const PointerRNA gizmo_ptr = RNA_pointer_get(ptr, "gizmo_object");
  const bool has_object = !RNA_pointer_is_null(&gizmo_ptr);
  col = &layout.column(false);
  col->active_set(!has_object);
  col->prop(ptr, "gizmo_location", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(ptr, "gizmo_rotation", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  col->prop(ptr, "gizmo_size", UI_ITEM_NONE, std::nullopt, ICON_NONE);
}

}  // namespace uvw_map

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);
  const ModifierData *md = static_cast<const ModifierData *>(ptr->data);
  uvw_map::draw_settings(layout, ptr, &ob_ptr, md->name, -1);
  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_UVWMap, panel_draw);
}

namespace uvw_map {

UVWMapModifierData *data_new()
{
  UVWMapModifierData *umd = MEM_new<UVWMapModifierData>(__func__);
  umd->modifier.type = eModifierType_UVWMap;
  return umd;
}

UVWMapModifierData *data_copy(const UVWMapModifierData &src)
{
  UVWMapModifierData *umd = data_new();
  BKE_modifier_copydata_generic(&src.modifier, &umd->modifier, 0);
  return umd;
}

void data_free(UVWMapModifierData *umd)
{
  MEM_delete(umd);
}

void data_blend_write(BlendWriter *writer, const UVWMapModifierData &umd)
{
  writer->write_struct(&umd);
}

void data_blend_read(BlendDataReader *reader, UVWMapModifierData *&umd)
{
  BLO_read_struct(reader, UVWMapModifierData, &umd);
  if (umd) {
    umd->modifier.next = nullptr;
    umd->modifier.prev = nullptr;
    umd->modifier.type = eModifierType_UVWMap;
    umd->modifier.error = nullptr;
    umd->modifier.system_properties = nullptr;
    umd->modifier.runtime = nullptr;
  }
}

}  // namespace uvw_map

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
