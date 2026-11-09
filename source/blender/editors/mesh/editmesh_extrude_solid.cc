/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 *
 * Extrude Manifold, solid version (3ds Max style).
 *
 * On a closed part of the mesh the extrude is a solid operation:
 * - Pushing faces in removes the volume they sweep through (Boolean Difference): side walls
 *   that end up against existing faces disappear, and pushing through the mesh opens a hole.
 * - Pulling faces out adds the swept volume (Boolean Union), merging with any part of the mesh
 *   it runs into.
 * On an open part, or with Solid turned off, it is Blender's Extrude Manifold as before
 * (dissolve flat side edges, then merge and split where the moved geometry lands).
 *
 * #MESH_OT_extrude_manifold is a macro of three steps:
 * - #MESH_OT_extrude_manifold_region extrudes. For a solid it stores every vertex position in a
 *   temporary attribute first, and does not dissolve side edges.
 * - #TRANSFORM_OT_translate moves the result, interactively.
 * - #MESH_OT_extrude_manifold_resolve finishes. For a solid it puts the mesh back as it was
 *   (from the stored positions), copies the edited part into a separate mesh, builds the solid
 *   swept by the faces there and applies it with the exact Boolean solver, then puts the part
 *   back. Otherwise it merges and splits, as the move used to.
 */

#include <algorithm>

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_editmesh.hh"
#include "BKE_layer.hh"
#include "BKE_report.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_mesh.hh"
#include "ED_screen.hh"
#include "ED_transform.hh"

#include "mesh_intern.hh" /* own include */

#include "tools/bmesh_boolean.hh"
#include "tools/bmesh_intersect.hh"

namespace blender {

/** Position of every vertex before the extrude (internal, never shown). */
static constexpr const char *ORIG_CO_LAYER = ".extrude_solid_co";
/** Outward direction of the faces of the swept solid, zero on other faces (internal). */
static constexpr const char *CUTTER_NO_LAYER = ".extrude_solid_no";

static void orig_co_layer_free(BMesh *bm)
{
  if (CustomData_get_offset_named(&bm->vdata, CD_PROP_FLOAT3, ORIG_CO_LAYER) != -1) {
    BM_data_layer_free_named(bm, &bm->vdata, ORIG_CO_LAYER);
  }
}

/** Tag (#BM_ELEM_TAG) every vertex, edge and face connected to `faces` by vertices. */
static void tag_connected(BMesh *bm, const Span<BMFace *> faces)
{
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  Vector<BMVert *> stack;
  for (BMFace *f : faces) {
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      if (!BM_elem_flag_test(l_iter->v, BM_ELEM_TAG)) {
        BM_elem_flag_enable(l_iter->v, BM_ELEM_TAG);
        stack.append(l_iter->v);
      }
    } while ((l_iter = l_iter->next) != l_first);
  }
  while (!stack.is_empty()) {
    BMVert *v = stack.pop_last();
    BMIter iter;
    BMEdge *e;
    BM_ITER_ELEM (e, &iter, v, BM_EDGES_OF_VERT) {
      BM_elem_flag_enable(e, BM_ELEM_TAG);
      BMVert *v_other = BM_edge_other_vert(e, v);
      if (!BM_elem_flag_test(v_other, BM_ELEM_TAG)) {
        BM_elem_flag_enable(v_other, BM_ELEM_TAG);
        stack.append(v_other);
      }
    }
    BMFace *f;
    BM_ITER_ELEM (f, &iter, v, BM_FACES_OF_VERT) {
      BM_elem_flag_enable(f, BM_ELEM_TAG);
    }
  }
}

/** True when the tagged part is a closed surface that can be treated as a solid. */
static bool tagged_part_is_closed(BMesh *bm)
{
  BMIter iter;
  BMEdge *e;
  BM_ITER_MESH (e, &iter, bm, BM_EDGES_OF_MESH) {
    if (BM_elem_flag_test(e, BM_ELEM_TAG) &&
        (BM_elem_flag_test(e, BM_ELEM_HIDDEN) || !BM_edge_is_manifold(e)))
    {
      return false;
    }
  }
  BMFace *f;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_TAG) && BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      return false;
    }
  }
  return true;
}

static Vector<BMFace *> selected_faces(BMesh *bm)
{
  Vector<BMFace *> faces;
  BMIter iter;
  BMFace *f;
  BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
    if (BM_elem_flag_test(f, BM_ELEM_SELECT) && !BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
      faces.append(f);
    }
  }
  return faces;
}

/* -------------------------------------------------------------------- */
/** \name Copying a part between meshes
 * \{ */

struct CopyMaps {
  BMCustomDataCopyMap vert, edge, face, loop;
  Map<BMVert *, BMVert *> verts;
  Map<BMEdge *, BMEdge *> edges;

  CopyMaps(const BMesh &src, const BMesh &dst)
      : vert(CustomData_bmesh_copy_map_calc(src.vdata, dst.vdata)),
        edge(CustomData_bmesh_copy_map_calc(src.edata, dst.edata)),
        face(CustomData_bmesh_copy_map_calc(src.pdata, dst.pdata)),
        loop(CustomData_bmesh_copy_map_calc(src.ldata, dst.ldata))
  {
  }
};

/** Copy flags without the selection (set through the selection API so counts stay right). */
static char copy_hflag(const char hflag)
{
  return hflag & ~(BM_ELEM_SELECT | BM_ELEM_TAG);
}

static BMVert *copy_vert(BMesh *dst, CopyMaps &maps, BMVert *v)
{
  return maps.verts.lookup_or_add_cb(v, [&]() {
    BMVert *v_new = BM_vert_create(dst, v->co, nullptr, BM_CREATE_SKIP_CD);
    CustomData_bmesh_copy_block(dst->vdata, maps.vert, v->head.data, &v_new->head.data);
    copy_v3_v3(v_new->no, v->no);
    v_new->head.hflag = copy_hflag(v->head.hflag);
    return v_new;
  });
}

static BMEdge *copy_edge(BMesh *dst, CopyMaps &maps, BMEdge *e)
{
  return maps.edges.lookup_or_add_cb(e, [&]() {
    BMEdge *e_new = BM_edge_create(
        dst, copy_vert(dst, maps, e->v1), copy_vert(dst, maps, e->v2), nullptr, BM_CREATE_SKIP_CD);
    CustomData_bmesh_copy_block(dst->edata, maps.edge, e->head.data, &e_new->head.data);
    e_new->head.hflag = copy_hflag(e->head.hflag);
    return e_new;
  });
}

/** Copy a face (with its vertices, edges and corner data) into `dst`. */
static BMFace *copy_face(BMesh *dst, CopyMaps &maps, BMFace *f)
{
  Vector<BMVert *, 8> verts;
  Vector<BMEdge *, 8> edges;
  BMLoop *l_iter, *l_first;
  l_iter = l_first = BM_FACE_FIRST_LOOP(f);
  do {
    verts.append(copy_vert(dst, maps, l_iter->v));
    edges.append(copy_edge(dst, maps, l_iter->e));
  } while ((l_iter = l_iter->next) != l_first);

  BMFace *f_new = BM_face_create(
      dst, verts.data(), edges.data(), verts.size(), nullptr, BM_CREATE_SKIP_CD);
  CustomData_bmesh_copy_block(dst->pdata, maps.face, f->head.data, &f_new->head.data);
  f_new->head.hflag = copy_hflag(f->head.hflag);
  f_new->mat_nr = f->mat_nr;
  copy_v3_v3(f_new->no, f->no);

  BMLoop *l_new = BM_FACE_FIRST_LOOP(f_new);
  l_iter = l_first;
  do {
    CustomData_bmesh_copy_block(dst->ldata, maps.loop, l_iter->head.data, &l_new->head.data);
    l_new = l_new->next;
  } while ((l_iter = l_iter->next) != l_first);

  if (BM_elem_flag_test(f, BM_ELEM_SELECT)) {
    BM_face_select_set(dst, f_new, true);
  }
  return f_new;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name First step: extrude
 * \{ */

static bool selection_is_closed_solid(BMesh *bm)
{
  const Vector<BMFace *> faces = selected_faces(bm);
  if (faces.is_empty()) {
    return false;
  }
  tag_connected(bm, faces);
  const bool closed = tagged_part_is_closed(bm);
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  return closed;
}

static wmOperatorStatus edbm_extrude_manifold_region_exec(bContext *C, wmOperator *op)
{
  const bool use_solid = RNA_boolean_get(op->ptr, "use_solid");
  const bool use_normal_flip = RNA_boolean_get(op->ptr, "use_normal_flip");
  const bool use_dissolve_ortho_edges = RNA_boolean_get(op->ptr, "use_dissolve_ortho_edges");

  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    BMesh *bm = em->bm;
    orig_co_layer_free(bm);
    if (bm->totvertsel == 0) {
      continue;
    }

    bool solid = false;
#ifdef WITH_GMP
    solid = use_solid && bm->totfacesel > 0 && selection_is_closed_solid(bm);
#else
    UNUSED_VARS(use_solid);
#endif
    if (solid) {
      BM_data_layer_add_named(bm, &bm->vdata, CD_PROP_FLOAT3, ORIG_CO_LAYER);
      const int offset = CustomData_get_offset_named(&bm->vdata, CD_PROP_FLOAT3, ORIG_CO_LAYER);
      BMIter iter;
      BMVert *v;
      BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
        copy_v3_v3(static_cast<float *>(BM_ELEM_CD_GET_VOID_P(v, offset)), v->co);
      }
    }

    /* A solid is rebuilt from the plain extrude, so its side edges must not be dissolved. */
    if (!edbm_extrude_mesh_ex(
            obedit, em, op, use_normal_flip, solid ? false : use_dissolve_ortho_edges))
    {
      orig_co_layer_free(bm);
      continue;
    }

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    params.is_destructive = true;
    EDBM_update(id_cast<Mesh *>(obedit->data), &params);
  }
  return OPERATOR_FINISHED;
}

void MESH_OT_extrude_manifold_region(wmOperatorType *ot)
{
  ot->name = "Extrude Manifold Region";
  ot->idname = "MESH_OT_extrude_manifold_region";
  ot->description = "Extrude region of faces (first step of Extrude Manifold)";

  ot->exec = edbm_extrude_manifold_region_exec;
  ot->poll = ED_operator_editmesh;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  RNA_def_boolean(ot->srna,
                  "use_solid",
                  true,
                  "Solid",
                  "On a closed mesh, pushing faces in cuts away the volume they pass through "
                  "(opening a hole when they go through) and pulling them out adds it, as in "
                  "3ds Max. Open meshes extrude as before");
  RNA_def_boolean(ot->srna, "use_normal_flip", false, "Flip Normals", "");
  RNA_def_boolean(ot->srna,
                  "use_dissolve_ortho_edges",
                  true,
                  "Dissolve Orthogonal Edges",
                  "Dissolve side edges that end up flat with the faces around them (when not "
                  "solid)");
  ed::transform::properties_register(ot, P_NO_DEFAULTS | P_MIRROR_DUMMY);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Last step: resolve
 * \{ */

enum class SolidResult {
  /** Not a solid extrude (or nothing to do). */
  None,
  /** The extruded part of the mesh is not closed: the normal extrude is kept. */
  Open,
  /** Built without the exact solver: the normal extrude is kept. */
  NoSolver,
  Done,
};

static void copy_face_loops(BMesh *bm, BMFace *src, const Span<BMLoop *> dst_loops)
{
  BMLoop *l_src = BM_FACE_FIRST_LOOP(src);
  for (BMLoop *l_dst : dst_loops) {
    BM_elem_attrs_copy(bm, l_src, l_dst);
    l_src = l_src->next;
  }
}

struct BooleanShapes {
  const Set<BMFace *> *cutter;
};

static int extrude_solid_shape_fn(BMFace *f, void *user_data)
{
  const BooleanShapes *shapes = static_cast<const BooleanShapes *>(user_data);
  return shapes->cutter->contains(f) ? 1 : 0;
}

static SolidResult extrude_solid_resolve(BMEditMesh *em)
{
  BMesh *bm = em->bm;
  const int offset = CustomData_get_offset_named(&bm->vdata, CD_PROP_FLOAT3, ORIG_CO_LAYER);
  if (offset == -1) {
    return SolidResult::None;
  }
  auto orig_co = [&](BMVert *v) -> float3 {
    return float3(static_cast<const float *>(BM_ELEM_CD_GET_VOID_P(v, offset)));
  };

  /* The extruded caps are the selected faces. */
  Vector<BMFace *> caps;
  {
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_SELECT) && !BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
        caps.append(f);
      }
    }
  }
  if (caps.is_empty()) {
    return SolidResult::None;
  }
#ifndef WITH_GMP
  return SolidResult::NoSolver;
#else

  tag_connected(bm, caps);
  if (!tagged_part_is_closed(bm)) {
    return SolidResult::Open;
  }

  VectorSet<BMVert *> cap_verts;
  for (BMFace *f : caps) {
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      cap_verts.add(l_iter->v);
    } while ((l_iter = l_iter->next) != l_first);
  }

  /* Each vertex on the border of the caps is joined by a side edge to the vertex it was
   * extruded from, which is still where the cap vertex started. */
  Map<BMVert *, BMVert *> source_of;
  Vector<BMEdge *> side_edges;
  for (BMVert *v : cap_verts) {
    const float3 start = orig_co(v);
    BMIter iter;
    BMEdge *e;
    BM_ITER_ELEM (e, &iter, v, BM_EDGES_OF_VERT) {
      BMVert *v_other = BM_edge_other_vert(e, v);
      if (!cap_verts.contains(v_other) && float3(v_other->co) == start) {
        source_of.add(v, v_other);
        side_edges.append(e);
        break;
      }
    }
  }
  if (source_of.is_empty()) {
    /* The whole part was extruded: there are no side walls, it was simply moved. */
    return SolidResult::None;
  }

  /* How far each vertex moved. */
  Map<BMVert *, float3> moved;
  float max_move = 0.0f;
  float max_coord = 0.0f;
  float3 sum_move(0.0f);
  for (BMVert *v : cap_verts) {
    const float3 d = float3(v->co) - orig_co(v);
    moved.add(v, d);
    sum_move += d;
    max_move = std::max(max_move, math::length(d));
    max_coord = std::max(max_coord, math::reduce_max(math::abs(float3(v->co))));
  }
  float3 sum_normal(0.0f);
  for (BMFace *f : caps) {
    float no[3];
    BM_face_calc_normal(f, no);
    sum_normal += float3(no) * BM_face_calc_area(f);
  }

  /* Put the mesh back as it was before the extrude: remove the side walls and join the caps
   * to the faces around them again. */
  {
    Vector<BMFace *> side_faces;
    for (BMEdge *e : side_edges) {
      BMIter iter;
      BMFace *f;
      BM_ITER_ELEM (f, &iter, e, BM_FACES_OF_EDGE) {
        side_faces.append_non_duplicates(f);
      }
    }
    for (BMFace *f : side_faces) {
      BM_face_kill(bm, f);
    }
    for (BMEdge *e : side_edges) {
      BM_edge_kill(bm, e);
    }
  }
  for (BMVert *v : cap_verts) {
    copy_v3_v3(v->co, orig_co(v));
  }
  /* Cap border edges and the edges they were extruded from. */
  Vector<std::pair<BMEdge *, BMEdge *>> edge_pairs;
  for (BMFace *f : caps) {
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      BMVert *a = source_of.lookup_default(l_iter->v, nullptr);
      BMVert *b = source_of.lookup_default(l_iter->next->v, nullptr);
      if (a && b) {
        if (BMEdge *e_source = BM_edge_exists(a, b)) {
          edge_pairs.append({e_source, l_iter->e});
        }
      }
    } while ((l_iter = l_iter->next) != l_first);
  }
  Map<BMVert *, float3> moved_kept;
  for (BMVert *v : cap_verts) {
    BMVert *source = source_of.lookup_default(v, nullptr);
    moved_kept.add(source ? source : v, moved.lookup(v));
  }
  for (const auto item : source_of.items()) {
    BM_vert_splice(bm, item.value, item.key);
  }
  for (const auto &[e_dst, e_src] : edge_pairs) {
    if (e_dst != e_src && BM_vert_in_edge(e_src, e_dst->v1) && BM_vert_in_edge(e_src, e_dst->v2))
    {
      BM_edge_splice(bm, e_dst, e_src);
    }
  }

  if (max_move < 1e-6f) {
    /* Not moved: the mesh is back as it was. */
    for (BMFace *f : caps) {
      BM_face_select_set(bm, f, true);
    }
    return SolidResult::Done;
  }

  /* Copy the edited part into its own mesh: the Boolean then only works on that part, however
   * big the rest of the mesh is. */
  tag_connected(bm, caps);
  BMeshCreateParams create_params{};
  create_params.use_toolflags = true;
  BMesh *part = BM_mesh_create(&bm_mesh_allocsize_default, &create_params);
  BM_mesh_copy_init_customdata(part, bm, &bm_mesh_allocsize_default);
  Vector<BMFace *> caps_part;
  {
    CopyMaps to_part(*bm, *part);
    Set<BMFace *> caps_set(caps);
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (BM_elem_flag_test(f, BM_ELEM_TAG)) {
        BMFace *f_new = copy_face(part, to_part, f);
        if (caps_set.contains(f)) {
          caps_part.append(f_new);
        }
      }
    }
    Map<BMVert *, float3> moved_part;
    for (const auto item : moved_kept.items()) {
      moved_part.add(to_part.verts.lookup(item.key), item.value);
    }
    moved_kept = std::move(moved_part);
  }
  /* Remove the part from the mesh; the result is put back at the end. */
  BM_select_history_clear(bm);
  {
    Vector<BMVert *> part_verts;
    BMIter iter;
    BMVert *v;
    BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
      if (BM_elem_flag_test(v, BM_ELEM_TAG)) {
        part_verts.append(v);
      }
    }
    for (BMVert *v_kill : part_verts) {
      BM_vert_kill(bm, v_kill);
    }
  }

  /* The swept solid. It starts a little behind the faces, so the faces themselves end up
   * inside it and are removed. */
  const float eps = std::max(max_move * 1e-3f, max_coord * 1e-6f);
  const float3 fallback_dir = math::normalize(sum_move);
  Map<BMVert *, BMVert *> bottom, top;
  for (const auto item : moved_kept.items()) {
    BMVert *v = item.key;
    const float3 d = item.value;
    const float len = math::length(d);
    const float3 dir = len > 1e-8f ? d / len : fallback_dir;
    bottom.add(v, BM_vert_create(part, float3(v->co) - dir * eps, v, BM_CREATE_NOP));
    top.add(v, BM_vert_create(part, float3(v->co) + d, v, BM_CREATE_NOP));
  }

  Set<BMFace *> caps_set(caps_part);
  Set<BMFace *> cutter;
  Vector<BMFace *> cutter_faces;
  Vector<BMFace *> new_tops;
  for (BMFace *f : caps_part) {
    Vector<BMVert *, 8> verts_bottom, verts_top;
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      verts_bottom.append(bottom.lookup(l_iter->v));
      verts_top.append(top.lookup(l_iter->v));
    } while ((l_iter = l_iter->next) != l_first);

    for (const bool is_top : {false, true}) {
      Vector<BMVert *, 8> &verts = is_top ? verts_top : verts_bottom;
      BMFace *f_new = BM_face_create_verts(
          part, verts.data(), verts.size(), f, BM_CREATE_NOP, true);
      if (f_new == nullptr) {
        continue;
      }
      Vector<BMLoop *, 8> loops;
      BMLoop *l_new = BM_FACE_FIRST_LOOP(f_new);
      for (int i = 0; i < verts.size(); i++, l_new = l_new->next) {
        loops.append(l_new);
      }
      copy_face_loops(part, f, loops);
      cutter.add(f_new);
      cutter_faces.append(f_new);
      if (is_top) {
        new_tops.append(f_new);
      }
    }

    /* Side walls along the border of the caps. */
    l_iter = l_first;
    do {
      BMFace *f_other = l_iter->radial_next->f;
      if (l_iter->radial_next != l_iter && caps_set.contains(f_other)) {
        continue;
      }
      BMVert *a = l_iter->v, *b = l_iter->next->v;
      BMVert *quad[4] = {bottom.lookup(a), bottom.lookup(b), top.lookup(b), top.lookup(a)};
      BMFace *f_side = BM_face_create_verts(part, quad, 4, f, BM_CREATE_NOP, true);
      if (f_side == nullptr) {
        continue;
      }
      BMLoop *l_side = BM_FACE_FIRST_LOOP(f_side);
      BM_elem_attrs_copy(part, l_iter, l_side);
      BM_elem_attrs_copy(part, l_iter->next, l_side->next);
      BM_elem_attrs_copy(part, l_iter->next, l_side->next->next);
      BM_elem_attrs_copy(part, l_iter, l_side->next->next->next);
      cutter.add(f_side);
      cutter_faces.append(f_side);
    } while ((l_iter = l_iter->next) != l_first);
  }

  /* Outward normals for the solid. */
  BM_mesh_elem_hflag_disable_all(part, BM_FACE, BM_ELEM_TAG, false);
  for (BMFace *f : cutter_faces) {
    /* New faces have no normal yet; recalculating the directions needs one. */
    BM_face_normal_update(f);
    BM_elem_flag_enable(f, BM_ELEM_TAG);
  }
  BMO_op_callf(part, BMO_FLAG_DEFAULTS, "recalc_face_normals faces=%hf", BM_ELEM_TAG);

  /* Remember which way each face of the solid points. The Boolean keeps a face that it does
   * not cut as it is, even where a Difference needs it turned around (the floor of a cut that
   * touches nothing), so those faces are flipped afterwards. Faces it creates copy the value. */
  const bool push_in = math::dot(sum_move, sum_normal) < 0.0f;
  BM_data_layer_add_named(part, &part->pdata, CD_PROP_FLOAT3, CUTTER_NO_LAYER);
  const int no_offset = CustomData_get_offset_named(
      &part->pdata, CD_PROP_FLOAT3, CUTTER_NO_LAYER);
  {
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, part, BM_FACES_OF_MESH) {
      zero_v3(static_cast<float *>(BM_ELEM_CD_GET_VOID_P(f, no_offset)));
    }
  }
  for (BMFace *f : cutter_faces) {
    BM_face_normal_update(f);
    copy_v3_v3(static_cast<float *>(BM_ELEM_CD_GET_VOID_P(f, no_offset)), f->no);
  }

  /* The result keeps the selection of the faces it comes from: select the new caps only. */
  BM_mesh_elem_hflag_disable_all(part, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
  for (BMFace *f : new_tops) {
    BM_face_select_set(part, f, true);
  }

  BM_mesh_normals_update(part);
  Array<std::array<BMLoop *, 3>> looptris(poly_to_tri_count(part->totface, part->totloop));
  BM_mesh_calc_tessellation(part, looptris);
  BooleanShapes shapes{&cutter};
  BM_mesh_boolean(part,
                  looptris,
                  extrude_solid_shape_fn,
                  &shapes,
                  2,
                  false,
                  true,
                  false,
                  push_in ? BMESH_ISECT_BOOLEAN_DIFFERENCE : BMESH_ISECT_BOOLEAN_UNION);

  if (push_in) {
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, part, BM_FACES_OF_MESH) {
      const float3 cutter_no(static_cast<const float *>(BM_ELEM_CD_GET_VOID_P(f, no_offset)));
      if (math::is_zero(cutter_no)) {
        continue;
      }
      BM_face_normal_update(f);
      if (math::dot(float3(f->no), cutter_no) > 0.0f) {
        BM_face_normal_flip(part, f);
      }
    }
  }
  BM_data_layer_free_named(part, &part->pdata, CUTTER_NO_LAYER);

  /* Put the result back. */
  {
    CopyMaps to_mesh(*part, *bm);
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, part, BM_FACES_OF_MESH) {
      copy_face(bm, to_mesh, f);
    }
  }
  BM_mesh_free(part);
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  return SolidResult::Done;
#endif
}

static wmOperatorStatus edbm_extrude_manifold_resolve_exec(bContext *C, wmOperator *op)
{
  const bool use_automerge_and_split = RNA_boolean_get(op->ptr, "use_automerge_and_split");
  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  bool any_open = false, no_solver = false;
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    const bool is_solid =
        CustomData_get_offset_named(&em->bm->vdata, CD_PROP_FLOAT3, ORIG_CO_LAYER) != -1;
    const SolidResult result = extrude_solid_resolve(em);
    orig_co_layer_free(em->bm);
    any_open |= result == SolidResult::Open;
    no_solver |= result == SolidResult::NoSolver;
    if (result == SolidResult::Done) {
      EDBM_selectmode_flush(em);
    }
    else if (!is_solid && use_automerge_and_split && em->bm->totvertsel != 0) {
      /* Blender's Extrude Manifold: merge and split where the moved geometry lands. */
      const bool has_face_sel = em->bm->totfacesel != 0;
      EDBM_automerge_and_split(
          obedit, true, true, true, BM_ELEM_SELECT, scene->toolsettings->doublimit);
      if (has_face_sel && (em->selectmode == SCE_SELECT_FACE)) {
        EDBM_selectmode_flush_ex(em, SCE_SELECT_EDGE);
      }
    }
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    params.is_destructive = true;
    EDBM_update(id_cast<Mesh *>(obedit->data), &params);
  }
  if (no_solver) {
    BKE_report(op->reports, RPT_WARNING, "Built without the exact solver: normal extrude kept");
  }
  else if (any_open) {
    BKE_report(op->reports, RPT_INFO, "The mesh is open (not a closed solid): normal extrude kept");
  }
  return OPERATOR_FINISHED;
}

void MESH_OT_extrude_manifold_resolve(wmOperatorType *ot)
{
  ot->name = "Extrude Manifold Resolve";
  ot->idname = "MESH_OT_extrude_manifold_resolve";
  ot->description =
      "Finish Extrude Manifold: solid cut or add on closed meshes, otherwise merge and split "
      "(last step of Extrude Manifold)";

  ot->exec = edbm_extrude_manifold_resolve_exec;
  ot->poll = ED_operator_editmesh;

  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_INTERNAL;

  RNA_def_boolean(ot->srna,
                  "use_automerge_and_split",
                  true,
                  "Merge and Split",
                  "When not solid, merge and split where the moved geometry lands on the mesh");
}

/** \} */

}  // namespace blender
