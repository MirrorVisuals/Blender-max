/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Edit Poly modifier (experimental, Edit Poly stack).
 *
 * Stores edits against stable element IDs (see BKE_stable_id.hh) and replays them on the mesh
 * that arrives at the modifier:
 * 1. a topology patch (see MOD_edit_poly_patch.hh): removed elements and new geometry anchored
 *    to input vertices;
 * 2. vertex moves.
 * Because everything is found by ID rather than by index, the edits stay on the same elements
 * when modifiers below change. Edits whose elements are missing are paused, not deleted.
 */

#include <atomic>
#include <cstring>
#include <string>

#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_task.hh"
#include "BLI_vector.hh"

#include "BLT_translation.hh"

#include "DNA_mesh_types.h"
#include "DNA_modifier_types.h"
#include "DNA_screen_types.h"

#include "BKE_attribute.hh"
#include "BKE_deform.hh"
#include "BKE_lib_id.hh"
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
#include "MOD_edit_poly_attrs.hh"
#include "MOD_edit_poly_ops.hh"
#include "MOD_edit_poly_patch.hh"
#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Stored data helpers
 * \{ */

static Span<int> patch_ints(const EditPolyModifierData &emd)
{
  return emd.patch_ints ? Span<int>(emd.patch_ints, emd.patch_ints_num) : Span<int>();
}

static Span<float> patch_floats(const EditPolyModifierData &emd)
{
  return emd.patch_floats ? Span<float>(emd.patch_floats, emd.patch_floats_num) : Span<float>();
}

static void set_ops_text(EditPolyModifierData &emd, const std::string &text)
{
  if (emd.ops_text) {
    MEM_delete(emd.ops_text);
    emd.ops_text = nullptr;
  }
  if (!text.empty()) {
    emd.ops_text = BLI_strdup(text.c_str());
  }
}

static void free_arrays(EditPolyModifierData &emd)
{
  set_ops_text(emd, "");
  MEM_SAFE_DELETE(emd.move_ids);
  MEM_SAFE_DELETE(emd.move_offsets);
  MEM_SAFE_DELETE(emd.patch_ints);
  MEM_SAFE_DELETE(emd.patch_floats);
  MEM_SAFE_DELETE(emd.sel_face_ids);
  MEM_SAFE_DELETE(emd.sel_vert_ids);
  MEM_SAFE_DELETE(emd.sel_edge_ids);
  emd.sel_verts_num = 0;
  emd.sel_edges_num = 0;
  MEM_SAFE_DELETE(emd.attr_ints);
  MEM_SAFE_DELETE(emd.attr_floats);
  emd.attr_ints_num = 0;
  emd.attr_floats_num = 0;
  emd.moves_num = 0;
  emd.patch_ints_num = 0;
  emd.patch_floats_num = 0;
  emd.sel_faces_num = 0;
}

template<typename T> static T *dup_array(const T *src, const int64_t num)
{
  if (src == nullptr || num <= 0) {
    return nullptr;
  }
  T *dst = MEM_new_array_uninitialized<T>(size_t(num), __func__);
  memcpy(dst, src, sizeof(T) * size_t(num));
  return dst;
}

static void store_moves(EditPolyModifierData &emd,
                        const Span<int2> ids,
                        const Span<float3> offsets)
{
  MEM_SAFE_DELETE(emd.move_ids);
  MEM_SAFE_DELETE(emd.move_offsets);
  emd.moves_num = int(ids.size());
  if (ids.is_empty()) {
    return;
  }
  emd.move_ids = MEM_new_array_uninitialized<int>(size_t(ids.size()) * 2, __func__);
  emd.move_offsets = MEM_new_array_uninitialized<float>(size_t(ids.size()) * 3, __func__);
  for (const int i : ids.index_range()) {
    emd.move_ids[2 * i] = ids[i].x;
    emd.move_ids[2 * i + 1] = ids[i].y;
    emd.move_offsets[3 * i] = offsets[i].x;
    emd.move_offsets[3 * i + 1] = offsets[i].y;
    emd.move_offsets[3 * i + 2] = offsets[i].z;
  }
}

static void store_ints(int *&r_data, int &r_num, const Span<int> values, const int per_item)
{
  MEM_SAFE_DELETE(r_data);
  r_num = int(values.size()) / per_item;
  if (values.is_empty()) {
    return;
  }
  r_data = MEM_new_array_uninitialized<int>(size_t(values.size()), __func__);
  memcpy(r_data, values.data(), sizeof(int) * size_t(values.size()));
}

static void store_element_selection(EditPolyModifierData &emd,
                                    const Span<int> vert_ids,
                                    const Span<int> edge_ids)
{
  store_ints(emd.sel_vert_ids, emd.sel_verts_num, vert_ids, 2);
  store_ints(emd.sel_edge_ids, emd.sel_edges_num, edge_ids, 4);
  emd.flag |= MOD_EDITPOLY_SELECTION_ELEMENTS;
}

static void store_selection(EditPolyModifierData &emd, const Span<int2> ids)
{
  MEM_SAFE_DELETE(emd.sel_face_ids);
  emd.sel_faces_num = int(ids.size());
  emd.flag |= MOD_EDITPOLY_HAS_SELECTION;
  if (ids.is_empty()) {
    return;
  }
  emd.sel_face_ids = MEM_new_array_uninitialized<int>(size_t(ids.size()) * 2, __func__);
  for (const int i : ids.index_range()) {
    emd.sel_face_ids[2 * i] = ids[i].x;
    emd.sel_face_ids[2 * i + 1] = ids[i].y;
  }
}

static Span<int> attr_ints(const EditPolyModifierData &emd)
{
  return emd.attr_ints ? Span<int>(emd.attr_ints, emd.attr_ints_num) : Span<int>();
}

static Span<float> attr_floats(const EditPolyModifierData &emd)
{
  return emd.attr_floats ? Span<float>(emd.attr_floats, emd.attr_floats_num) : Span<float>();
}

static void store_attrs(EditPolyModifierData &emd, const Span<int> ints, const Span<float> floats)
{
  MEM_SAFE_DELETE(emd.attr_ints);
  MEM_SAFE_DELETE(emd.attr_floats);
  emd.attr_ints = dup_array(ints.data(), ints.size());
  emd.attr_floats = dup_array(floats.data(), floats.size());
  emd.attr_ints_num = emd.attr_ints ? int(ints.size()) : 0;
  emd.attr_floats_num = emd.attr_floats ? int(floats.size()) : 0;
}

static void store_patch(EditPolyModifierData &emd, const edit_poly::Patch &patch)
{
  Vector<int> ints;
  Vector<float> floats;
  edit_poly::patch_encode(patch, ints, floats);
  MEM_SAFE_DELETE(emd.patch_ints);
  MEM_SAFE_DELETE(emd.patch_floats);
  emd.patch_ints = dup_array(ints.data(), ints.size());
  emd.patch_floats = dup_array(floats.data(), floats.size());
  emd.patch_ints_num = emd.patch_ints ? int(ints.size()) : 0;
  emd.patch_floats_num = emd.patch_floats ? int(floats.size()) : 0;
}

static uint64_t key_of(const int2 id)
{
  return (uint64_t(uint32_t(id.y)) << 32) | uint32_t(id.x);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Edit Mode sessions
 * \{ */

namespace edit_poly {

void session_prepare(Mesh &input, Mesh &mesh)
{
  using namespace bke::stable_id;
  /* The input gets IDs for every domain, so the commit compares against the same IDs that the
   * modifier sees when it evaluates (root IDs are made from indices). */
  ensure_root_ids(input);

  const Array<StableID> vert_ids = read_or_make_root_ids(mesh, bke::AttrDomain::Point);
  const Array<StableID> face_ids = read_or_make_root_ids(mesh, bke::AttrDomain::Face);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  auto write_pair = [&](const char *name_lo,
                        const char *name_hi,
                        const bke::AttrDomain domain,
                        const Span<StableID> ids) {
    attributes.remove(name_lo);
    attributes.remove(name_hi);
    bke::SpanAttributeWriter<int> lo = attributes.lookup_or_add_for_write_only_span<int>(name_lo,
                                                                                         domain);
    bke::SpanAttributeWriter<int> hi = attributes.lookup_or_add_for_write_only_span<int>(name_hi,
                                                                                         domain);
    for (const int i : ids.index_range()) {
      lo.span[i] = int(ids[i].lo);
      hi.span[i] = int(ids[i].hi);
    }
    lo.finish();
    hi.finish();
  };
  write_pair(SESSION_ATTR_VERT_ID_LO, SESSION_ATTR_VERT_ID_HI, bke::AttrDomain::Point, vert_ids);
  write_pair(SESSION_ATTR_FACE_ID_LO, SESSION_ATTR_FACE_ID_HI, bke::AttrDomain::Face, face_ids);
}

std::string session_check(const EditPolyModifierData &emd, const Mesh &input)
{
  if (emd.patch_ints_num == 0) {
    return "";
  }
  Patch patch;
  if (!patch_decode(patch_ints(emd), patch_floats(emd), patch)) {
    return "This level's topology edits could not be read";
  }
  if (patch.is_empty()) {
    return "";
  }
  std::string reason;
  Mesh *result = patch_apply(input, patch, reason);
  if (result == nullptr) {
    return "This level's topology edits are paused (" + reason +
           "). Restore the modifiers below (e.g. the Subdivision level) to edit this level";
  }
  BKE_id_free(nullptr, result);
  return "";
}

std::string session_commit(EditPolyModifierData &emd,
                           const Mesh &input,
                           const Mesh &mesh,
                           const std::string &operations)
{
  Patch old_patch;
  patch_decode(patch_ints(emd), patch_floats(emd), old_patch);

  Patch patch;
  Vector<KeptMove> kept_moves;
  Vector<StableID> input_vert_ids;
  Array<StableID> edited_face_ids;
  Array<StableID> edited_vert_ids;
  const std::string error = patch_compute(
      input, mesh, old_patch, patch, kept_moves, input_vert_ids, edited_face_ids, edited_vert_ids);
  if (!error.empty()) {
    return error;
  }

  /* Element data (bevel weights, creases, sharp edges, seams, materials, flat shading). */
  {
    Vector<int> ints;
    Vector<float> floats;
    attrs_compute(input,
                  mesh,
                  edited_vert_ids,
                  edited_face_ids,
                  attr_ints(emd),
                  attr_floats(emd),
                  ints,
                  floats);
    store_attrs(emd, ints, floats);
  }

  /* Face selection: passed up the stack by ID, like the selection of an Edit Poly in 3ds Max. */
  {
    const bke::AttributeAccessor attributes = mesh.attributes();
    const VArraySpan<bool> select = *attributes.lookup_or_default<bool>(
        ".select_poly", bke::AttrDomain::Face, false);
    Vector<int2> selected;
    for (const int f : edited_face_ids.index_range()) {
      if (select[f] && edited_face_ids[f].is_valid()) {
        selected.append(int2(int(edited_face_ids[f].lo), int(edited_face_ids[f].hi)));
      }
    }
    store_selection(emd, selected);

    /* Vertices and edges too, so a selection of edges or vertices only (e.g. for Bevel with
     * Limit Method Selection) is passed up as well. Edges by the IDs of their two vertices. */
    const VArraySpan<bool> select_vert = *attributes.lookup_or_default<bool>(
        ".select_vert", bke::AttrDomain::Point, false);
    const VArraySpan<bool> select_edge = *attributes.lookup_or_default<bool>(
        ".select_edge", bke::AttrDomain::Edge, false);
    Vector<int> vert_ids;
    Vector<int> edge_ids;
    if (edited_vert_ids.size() == mesh.verts_num) {
      for (const int v : edited_vert_ids.index_range()) {
        if (select_vert[v] && edited_vert_ids[v].is_valid()) {
          vert_ids.extend({int(edited_vert_ids[v].lo), int(edited_vert_ids[v].hi)});
        }
      }
      const Span<int2> edges = mesh.edges();
      for (const int e : edges.index_range()) {
        const StableID a = edited_vert_ids[edges[e][0]];
        const StableID b = edited_vert_ids[edges[e][1]];
        if (select_edge[e] && a.is_valid() && b.is_valid()) {
          edge_ids.extend({int(a.lo), int(a.hi), int(b.lo), int(b.hi)});
        }
      }
    }
    store_element_selection(emd, vert_ids, edge_ids);
  }

  /* Moves: keep paused moves (points that are neither in the input nor new), replace the rest. */
  /* Which earlier moves are on points of this session: a table of the earlier moves, checked
   * against every point (instead of a table of every point). */
  Map<uint64_t, int> old_move_of;
  for (int i = 0; i < emd.moves_num; i++) {
    old_move_of.add(key_of(int2(emd.move_ids[2 * i], emd.move_ids[2 * i + 1])), i);
  }
  Array<bool> handled(emd.moves_num, false);
  if (!old_move_of.is_empty()) {
    auto mark = [&](const Span<StableID> list) {
      threading::parallel_for(list.index_range(), 8192, [&](const IndexRange range) {
        for (const int64_t v : range) {
          const int i = old_move_of.lookup_default(list[v].as_u64(), -1);
          if (i != -1) {
            handled[i] = true;
          }
        }
      });
    };
    mark(input_vert_ids);
    mark(patch.new_vert_ids);
  }
  Vector<int2> ids;
  Vector<float3> offsets;
  for (int i = 0; i < emd.moves_num; i++) {
    const int2 id(emd.move_ids[2 * i], emd.move_ids[2 * i + 1]);
    if (!handled[i]) {
      ids.append(id);
      offsets.append(float3(
          emd.move_offsets[3 * i], emd.move_offsets[3 * i + 1], emd.move_offsets[3 * i + 2]));
    }
  }
  for (const KeptMove &move : kept_moves) {
    if (math::length_squared(move.offset) > 1e-12f) {
      ids.append(int2(int(move.id.lo), int(move.id.hi)));
      offsets.append(move.offset);
    }
  }
  store_moves(emd, ids, offsets);
  store_patch(emd, patch);

  /* Operation log: earlier log plus this session's operations, kept only if replaying it on the
   * input explains the edited topology. */
  Vector<OpRecord> ops;
  std::string text = (emd.ops_text ? std::string(emd.ops_text) : std::string()) + operations;
  if (patch.is_empty() || !ops_parse(text, ops) || ops.is_empty()) {
    set_ops_text(emd, "");
    return "";
  }
  std::string reason;
  Mesh *replayed = ops_apply(input, ops, false, reason);
  const bool matches = replayed && replayed->verts_num == mesh.verts_num &&
                       replayed->edges_num == mesh.edges_num &&
                       replayed->faces_num == mesh.faces_num;
  if (replayed) {
    BKE_id_free(nullptr, replayed);
  }
  if (!matches) {
    set_ops_text(emd, "");
    return "";
  }
  ops_compute_footprints(input, ops);
  set_ops_text(emd, ops_to_text(ops));
  return "";
}

}  // namespace edit_poly

/** \} */

/* -------------------------------------------------------------------- */
/** \name Modifier
 * \{ */

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

  temd->move_ids = dup_array(emd->move_ids, int64_t(emd->moves_num) * 2);
  temd->move_offsets = dup_array(emd->move_offsets, int64_t(emd->moves_num) * 3);
  temd->patch_ints = dup_array(emd->patch_ints, emd->patch_ints_num);
  temd->patch_floats = dup_array(emd->patch_floats, emd->patch_floats_num);
  temd->ops_text = emd->ops_text ? BLI_strdup(emd->ops_text) : nullptr;
  temd->sel_face_ids = dup_array(emd->sel_face_ids, int64_t(emd->sel_faces_num) * 2);
  temd->sel_vert_ids = dup_array(emd->sel_vert_ids, int64_t(emd->sel_verts_num) * 2);
  temd->sel_edge_ids = dup_array(emd->sel_edge_ids, int64_t(emd->sel_edges_num) * 4);
  if (!temd->sel_vert_ids) {
    temd->sel_verts_num = 0;
  }
  if (!temd->sel_edge_ids) {
    temd->sel_edges_num = 0;
  }
  temd->attr_ints = dup_array(emd->attr_ints, emd->attr_ints_num);
  temd->attr_floats = dup_array(emd->attr_floats, emd->attr_floats_num);
  if (!temd->attr_ints || !temd->attr_floats) {
    MEM_SAFE_DELETE(temd->attr_ints);
    MEM_SAFE_DELETE(temd->attr_floats);
    temd->attr_ints_num = 0;
    temd->attr_floats_num = 0;
  }
  if (!temd->sel_face_ids) {
    temd->sel_faces_num = 0;
  }
  if (!temd->move_ids || !temd->move_offsets) {
    MEM_SAFE_DELETE(temd->move_ids);
    MEM_SAFE_DELETE(temd->move_offsets);
    temd->moves_num = 0;
  }
  if (!temd->patch_ints) {
    temd->patch_ints_num = 0;
  }
  if (!temd->patch_floats) {
    temd->patch_floats_num = 0;
  }
}

static void free_data(ModifierData *md)
{
  free_arrays(*reinterpret_cast<EditPolyModifierData *>(md));
}

static bool is_disabled(const Scene * /*scene*/, ModifierData *md, bool /*use_render_params*/)
{
  const EditPolyModifierData *emd = reinterpret_cast<const EditPolyModifierData *>(md);
  return emd->moves_num == 0 && emd->patch_ints_num == 0 && emd->attr_ints_num == 0 &&
         (emd->flag & MOD_EDITPOLY_HAS_SELECTION) == 0;
}

static uint64_t pair_hash(const uint64_t a, const uint64_t b)
{
  return a < b ? a * 0x9E3779B97F4A7C15ull ^ b : b * 0x9E3779B97F4A7C15ull ^ a;
}

/** The selection as a vertex group, for modifiers that take one (Solidify, Smooth, Displace...),
 * if the mesh has a vertex group of that name (Tab adds it to the object). */
static void write_selection_vertex_group(Mesh &mesh)
{
  if (BKE_id_defgroup_name_index(&mesh.id, edit_poly::SELECTION_VERTEX_GROUP) == -1) {
    return;
  }
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  const VArraySpan<bool> select = *attributes.lookup_or_default<bool>(
      ".select_vert", bke::AttrDomain::Point, false);
  bke::SpanAttributeWriter<float> group = attributes.lookup_for_write_span<float>(
      edit_poly::SELECTION_VERTEX_GROUP);
  if (!group || group.domain != bke::AttrDomain::Point) {
    return;
  }
  for (const int v : group.span.index_range()) {
    group.span[v] = select[v] ? 1.0f : 0.0f;
  }
  group.finish();
}

static void write_selection(const EditPolyModifierData &emd,
                            Mesh &mesh,
                            const Span<bke::stable_id::StableID> vert_ids,
                            const Span<bke::stable_id::StableID> ids)
{
  using namespace bke::stable_id;
  Set<uint64_t> selected;
  for (int i = 0; i < emd.sel_faces_num; i++) {
    selected.add(key_of(int2(emd.sel_face_ids[2 * i], emd.sel_face_ids[2 * i + 1])));
  }
  const bool elements = (emd.flag & MOD_EDITPOLY_SELECTION_ELEMENTS) != 0;
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  attributes.remove(".select_poly");
  attributes.remove(".select_vert");
  attributes.remove(".select_edge");

  if (!selected.is_empty()) {
    /* Faces split from a selected face (e.g. by a Subdivision added below later) are selected. */
    const VArraySpan<int2> parents = *attributes.lookup_or_default<int2>(
        ATTR_PARENT_FACE, bke::AttrDomain::Face, int2(0, 0));
    bke::SpanAttributeWriter<bool> select = attributes.lookup_or_add_for_write_only_span<bool>(
        ".select_poly", bke::AttrDomain::Face);
    for (const int f : ids.index_range()) {
      const int2 parent = parents[f];
      select.span[f] = selected.contains(ids[f].as_u64()) ||
                       ((parent.x != 0 || parent.y != 0) && selected.contains(key_of(parent)));
    }
    select.finish();
  }

  if (!elements) {
    /* Stored before vertex / edge selections were: take them from the faces. */
    if (!selected.is_empty()) {
      bke::mesh_select_face_flush(mesh);
    }
    write_selection_vertex_group(mesh);
    return;
  }

  if (emd.sel_verts_num > 0) {
    Set<uint64_t> verts;
    for (int i = 0; i < emd.sel_verts_num; i++) {
      verts.add(key_of(int2(emd.sel_vert_ids[2 * i], emd.sel_vert_ids[2 * i + 1])));
    }
    bke::SpanAttributeWriter<bool> select = attributes.lookup_or_add_for_write_only_span<bool>(
        ".select_vert", bke::AttrDomain::Point);
    for (const int v : vert_ids.index_range()) {
      select.span[v] = verts.contains(vert_ids[v].as_u64());
    }
    select.finish();
  }
  if (emd.sel_edges_num > 0) {
    Set<uint64_t> edges_selected;
    for (int i = 0; i < emd.sel_edges_num; i++) {
      const int *e = emd.sel_edge_ids + 4 * i;
      edges_selected.add(pair_hash(key_of(int2(e[0], e[1])), key_of(int2(e[2], e[3]))));
    }
    const Span<int2> edges = mesh.edges();
    bke::SpanAttributeWriter<bool> select = attributes.lookup_or_add_for_write_only_span<bool>(
        ".select_edge", bke::AttrDomain::Edge);
    for (const int e : edges.index_range()) {
      select.span[e] = edges_selected.contains(
          pair_hash(vert_ids[edges[e][0]].as_u64(), vert_ids[edges[e][1]].as_u64()));
    }
    select.finish();
  }
  write_selection_vertex_group(mesh);
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  using namespace bke::stable_id;
  EditPolyModifierData *emd = reinterpret_cast<EditPolyModifierData *>(md);
  std::string warning;

  /* 1. Topology patch. */
  Mesh *result = mesh;
  /* Moves recorded together with topology edits only make sense with that topology: when the
   * patch is paused (or replaced by re-run operations), the moves are paused too. */
  bool apply_moves = true;
  if (emd->patch_ints_num > 0) {
    edit_poly::Patch patch;
    if (!edit_poly::patch_decode(patch_ints(*emd), patch_floats(*emd), patch)) {
      warning = "Topology edits could not be read";
    }
    else if (!patch.is_empty()) {
      std::string reason;
      if (Mesh *patched = edit_poly::patch_apply(*mesh, patch, reason)) {
        result = patched;
      }
      else {
        /* The mesh below changed: re-run the logged operations on what covers their elements. */
        Vector<edit_poly::OpRecord> ops;
        std::string ops_reason;
        Mesh *replayed = nullptr;
        if (emd->ops_text && edit_poly::ops_parse(emd->ops_text, ops) &&
            edit_poly::ops_can_retarget(ops))
        {
          replayed = edit_poly::ops_apply(*mesh, ops, true, ops_reason);
        }
        apply_moves = false;
        if (replayed) {
          result = replayed;
          warning = "Mesh below changed: " + std::to_string(ops.size()) +
                    " operation(s) re-applied; other edits of this level are paused";
        }
        else {
          warning = "Edits paused: " + reason;
        }
      }
    }
  }

  /* IDs of the mesh being edited, read once for the steps below (without IDs from below, the
   * incoming mesh is the base mesh: root IDs). */
  const bool need_ids = (apply_moves && emd->moves_num > 0) || emd->attr_ints_num > 0 ||
                        (emd->flag & MOD_EDITPOLY_HAS_SELECTION);
  const Array<StableID> vert_ids = need_ids ?
                                       read_or_make_root_ids(*result, bke::AttrDomain::Point) :
                                       Array<StableID>();
  const Array<StableID> face_ids = need_ids ?
                                       read_or_make_root_ids(*result, bke::AttrDomain::Face) :
                                       Array<StableID>();

  /* 2. Moves: a small table of the moved IDs, checked against every vertex in parallel (much
   * faster than a table of every vertex when only some of them moved). */
  if (apply_moves && emd->moves_num > 0 && result->verts_num > 0) {
    Map<uint64_t, int> move_of;
    move_of.reserve(emd->moves_num);
    for (int i = 0; i < emd->moves_num; i++) {
      move_of.add(key_of(int2(emd->move_ids[2 * i], emd->move_ids[2 * i + 1])), i);
    }
    MutableSpan<float3> positions = result->vert_positions_for_write();
    std::atomic<int> found = 0;
    threading::parallel_for(vert_ids.index_range(), 4096, [&](const IndexRange range) {
      int local = 0;
      for (const int v : range) {
        const int i = move_of.lookup_default(vert_ids[v].as_u64(), -1);
        if (i != -1) {
          positions[v] += float3(emd->move_offsets[3 * i],
                                 emd->move_offsets[3 * i + 1],
                                 emd->move_offsets[3 * i + 2]);
          local++;
        }
      }
      found += local;
    });
    const int paused = int(move_of.size()) - found.load();
    result->tag_positions_changed();
    if (paused > 0) {
      if (!warning.empty()) {
        warning += "; ";
      }
      warning += std::to_string(paused) + " of " + std::to_string(emd->moves_num) +
                 " moves paused (their points are not in the mesh below)";
    }
  }

  /* 3. Element data (bevel weights, creases...). */
  if (emd->attr_ints_num > 0) {
    const int paused = edit_poly::attrs_apply(
        attr_ints(*emd), attr_floats(*emd), *result, vert_ids, face_ids);
    if (paused > 0) {
      if (!warning.empty()) {
        warning += "; ";
      }
      warning += std::to_string(paused) +
                 " edited weight(s) / crease(s) paused (their elements are not in the mesh below)";
    }
  }

  /* 4. Face selection, for modifiers above that work on selected faces (e.g. UVW Map). */
  if (emd->flag & MOD_EDITPOLY_HAS_SELECTION) {
    write_selection(*emd, *result, vert_ids, face_ids);
  }

  if (!warning.empty()) {
    BKE_modifier_set_warning(ctx->object, md, "%s", warning.c_str());
  }
  return result;
}

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, nullptr);
  const ModifierData *md = static_cast<const ModifierData *>(ptr->data);

  PointerRNA op_ptr = layout.op(
      "OBJECT_OT_edit_poly_level_edit", IFACE_("Edit Level"), ICON_EDITMODE_HLT);
  RNA_string_set(&op_ptr, "modifier", md->name);

  layout.use_property_split_set(true);
  layout.prop(ptr, "moves_num", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "patch_new_vertices", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "patch_new_faces", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "patch_removed_faces", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "operations_num", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "selected_faces_num", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "attribute_edits_num", UI_ITEM_NONE, std::nullopt, ICON_NONE);

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
  if (emd->patch_ints_num > 0 && emd->patch_ints) {
    writer->write_int32_array(emd->patch_ints_num, emd->patch_ints);
  }
  if (emd->patch_floats_num > 0 && emd->patch_floats) {
    writer->write_float_array(emd->patch_floats_num, emd->patch_floats);
  }
  writer->write_string(emd->ops_text);
  if (emd->sel_faces_num > 0 && emd->sel_face_ids) {
    writer->write_int32_array(int64_t(emd->sel_faces_num) * 2, emd->sel_face_ids);
  }
  if (emd->attr_ints_num > 0 && emd->attr_ints) {
    writer->write_int32_array(emd->attr_ints_num, emd->attr_ints);
  }
  if (emd->sel_verts_num > 0 && emd->sel_vert_ids) {
    writer->write_int32_array(int64_t(emd->sel_verts_num) * 2, emd->sel_vert_ids);
  }
  if (emd->sel_edges_num > 0 && emd->sel_edge_ids) {
    writer->write_int32_array(int64_t(emd->sel_edges_num) * 4, emd->sel_edge_ids);
  }
  if (emd->attr_floats_num > 0 && emd->attr_floats) {
    writer->write_float_array(emd->attr_floats_num, emd->attr_floats);
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
  BLO_read_array_and_validate_size(reader, &emd->patch_ints, &emd->patch_ints_num);
  BLO_read_array_and_validate_size(reader, &emd->patch_floats, &emd->patch_floats_num);
  if (!emd->patch_ints) {
    emd->patch_ints_num = 0;
  }
  if (!emd->patch_floats) {
    emd->patch_floats_num = 0;
  }
  BLO_read_string(reader, &emd->ops_text);
  int sel_num = emd->sel_faces_num;
  BLO_read_array_and_validate_size(reader, &emd->sel_face_ids, &sel_num, 2);
  if (sel_num != emd->sel_faces_num || !emd->sel_face_ids) {
    MEM_SAFE_DELETE(emd->sel_face_ids);
    emd->sel_faces_num = 0;
  }
  int verts_num = emd->sel_verts_num;
  BLO_read_array_and_validate_size(reader, &emd->sel_vert_ids, &verts_num, 2);
  if (verts_num != emd->sel_verts_num || !emd->sel_vert_ids) {
    MEM_SAFE_DELETE(emd->sel_vert_ids);
    emd->sel_verts_num = 0;
  }
  int edges_num = emd->sel_edges_num;
  BLO_read_array_and_validate_size(reader, &emd->sel_edge_ids, &edges_num, 4);
  if (edges_num != emd->sel_edges_num || !emd->sel_edge_ids) {
    MEM_SAFE_DELETE(emd->sel_edge_ids);
    emd->sel_edges_num = 0;
  }
  BLO_read_array_and_validate_size(reader, &emd->attr_ints, &emd->attr_ints_num);
  BLO_read_array_and_validate_size(reader, &emd->attr_floats, &emd->attr_floats_num);
  if (!emd->attr_ints || !emd->attr_floats) {
    MEM_SAFE_DELETE(emd->attr_ints);
    MEM_SAFE_DELETE(emd->attr_floats);
    emd->attr_ints_num = 0;
    emd->attr_floats_num = 0;
  }
}

/** \} */

ModifierTypeInfo modifierType_EditPoly = {
    /*idname*/ "EditPoly",
    /*name*/ N_("EditPoly"),
    /*struct_name*/ "EditPolyModifierData",
    /*struct_size*/ sizeof(EditPolyModifierData),
    /*srna*/ &RNA_EditPolyModifier,
    /*type*/ ModifierTypeType::Constructive,
    /*flags*/
    (eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsEditmode |
     eModifierTypeFlag_EnableInEditmode),
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
