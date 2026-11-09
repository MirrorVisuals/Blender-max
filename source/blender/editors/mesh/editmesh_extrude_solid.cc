/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edmesh
 *
 * Extrude Solid (3ds Max style extrude).
 *
 * A normal region extrude and move, then the result is rebuilt as a solid operation:
 * - Pushing faces into the mesh removes the volume they sweep through (Boolean Difference), so
 *   side walls that end up against existing faces disappear and pushing through the mesh opens
 *   a hole.
 * - Pulling faces out adds the swept volume (Boolean Union), so it merges with any part of the
 *   mesh it runs into.
 *
 * It is a macro: #MESH_OT_extrude_solid_prepare stores every vertex position in a temporary
 * attribute, #MESH_OT_extrude_region and #TRANSFORM_OT_translate do the usual interactive
 * extrude, and #MESH_OT_extrude_solid_resolve reads where the extruded faces came from, puts
 * the mesh back as it was and applies the swept solid with the exact Boolean solver.
 *
 * Only closed (manifold) parts of the mesh can be treated as solids; on an open part the normal
 * extrude is kept.
 */

#include <algorithm>

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"

#include "BLI_map.hh"
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

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_mesh.hh"
#include "ED_screen.hh"

#include "mesh_intern.hh" /* own include */

#include "tools/bmesh_boolean.hh"
#include "tools/bmesh_intersect.hh"

namespace blender {

/** Position of every vertex before the extrude (internal, never shown). */
static constexpr const char *ORIG_CO_LAYER = ".extrude_solid_co";

static void orig_co_layer_free(BMesh *bm)
{
  if (CustomData_get_offset_named(&bm->vdata, CD_PROP_FLOAT3, ORIG_CO_LAYER) != -1) {
    BM_data_layer_free_named(bm, &bm->vdata, ORIG_CO_LAYER);
  }
}

/* -------------------------------------------------------------------- */
/** \name Prepare
 * \{ */

static wmOperatorStatus edbm_extrude_solid_prepare_exec(bContext *C, wmOperator * /*op*/)
{
  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  for (Object *obedit : objects) {
    BMesh *bm = BKE_editmesh_from_object(obedit)->bm;
    orig_co_layer_free(bm);
    if (bm->totfacesel == 0) {
      continue;
    }
    BM_data_layer_add_named(bm, &bm->vdata, CD_PROP_FLOAT3, ORIG_CO_LAYER);
    const int offset = CustomData_get_offset_named(&bm->vdata, CD_PROP_FLOAT3, ORIG_CO_LAYER);
    BMIter iter;
    BMVert *v;
    BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
      copy_v3_v3(static_cast<float *>(BM_ELEM_CD_GET_VOID_P(v, offset)), v->co);
    }
  }
  return OPERATOR_FINISHED;
}

void MESH_OT_extrude_solid_prepare(wmOperatorType *ot)
{
  ot->name = "Extrude Solid Prepare";
  ot->idname = "MESH_OT_extrude_solid_prepare";
  ot->description = "Store vertex positions for Extrude Solid (internal step of the macro)";

  ot->exec = edbm_extrude_solid_prepare_exec;
  ot->poll = ED_operator_editmesh;

  ot->flag = OPTYPE_INTERNAL;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Resolve
 * \{ */

enum class SolidResult {
  /** Nothing extruded (or nothing to do). */
  None,
  /** The extruded part of the mesh is not closed: the normal extrude is kept. */
  Open,
  /** Built without the exact solver: the normal extrude is kept. */
  NoSolver,
  Done,
};

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
  if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
    return -1;
  }
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
    const float3 co_bottom = float3(v->co) - dir * eps;
    const float3 co_top = float3(v->co) + d;
    bottom.add(v, BM_vert_create(bm, co_bottom, v, BM_CREATE_NOP));
    top.add(v, BM_vert_create(bm, co_top, v, BM_CREATE_NOP));
  }

  Set<BMFace *> caps_set(caps);
  Set<BMFace *> cutter;
  Vector<BMFace *> cutter_faces;
  Vector<BMFace *> new_tops;
  for (BMFace *f : caps) {
    Vector<BMVert *, 8> verts_bottom, verts_top;
    Vector<BMLoop *, 8> loops_src;
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      verts_bottom.append(bottom.lookup(l_iter->v));
      verts_top.append(top.lookup(l_iter->v));
    } while ((l_iter = l_iter->next) != l_first);

    for (const bool is_top : {false, true}) {
      Vector<BMVert *, 8> &verts = is_top ? verts_top : verts_bottom;
      BMFace *f_new = BM_face_create_verts(
          bm, verts.data(), verts.size(), f, BM_CREATE_NOP, true);
      if (f_new == nullptr) {
        continue;
      }
      Vector<BMLoop *, 8> loops;
      BMLoop *l_new = BM_FACE_FIRST_LOOP(f_new);
      for (int i = 0; i < verts.size(); i++, l_new = l_new->next) {
        loops.append(l_new);
      }
      copy_face_loops(bm, f, loops);
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
      BMFace *f_side = BM_face_create_verts(bm, quad, 4, f, BM_CREATE_NOP, true);
      if (f_side == nullptr) {
        continue;
      }
      BMLoop *l_side = BM_FACE_FIRST_LOOP(f_side);
      BM_elem_attrs_copy(bm, l_iter, l_side);
      BM_elem_attrs_copy(bm, l_iter->next, l_side->next);
      BM_elem_attrs_copy(bm, l_iter->next, l_side->next->next);
      BM_elem_attrs_copy(bm, l_iter, l_side->next->next->next);
      cutter.add(f_side);
      cutter_faces.append(f_side);
    } while ((l_iter = l_iter->next) != l_first);
  }

  /* Outward normals for the solid. */
  BM_mesh_elem_hflag_disable_all(bm, BM_FACE, BM_ELEM_TAG, false);
  for (BMFace *f : cutter_faces) {
    BM_elem_flag_enable(f, BM_ELEM_TAG);
  }
  BMO_op_callf(bm, BMO_FLAG_DEFAULTS, "recalc_face_normals faces=%hf", BM_ELEM_TAG);

  /* The result keeps the selection of the faces it comes from: select the new caps only. */
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
  for (BMFace *f : new_tops) {
    BM_face_select_set(bm, f, true);
  }

  /* Keep the rest of the mesh out of the operation: hide it for the duration. */
  tag_connected(bm, caps);
  Vector<BMElem *> hidden_here;
  {
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
      if (!BM_elem_flag_test(f, BM_ELEM_TAG) && !cutter.contains(f) &&
          !BM_elem_flag_test(f, BM_ELEM_HIDDEN))
      {
        BM_elem_flag_enable(f, BM_ELEM_HIDDEN);
        hidden_here.append(reinterpret_cast<BMElem *>(f));
      }
    }
  }

  const bool push_in = math::dot(sum_move, sum_normal) < 0.0f;
  BKE_editmesh_looptris_calc(em);
  BooleanShapes shapes{&cutter};
  BM_mesh_boolean(bm,
                  em->looptris,
                  extrude_solid_shape_fn,
                  &shapes,
                  2,
                  false,
                  true,
                  false,
                  push_in ? BMESH_ISECT_BOOLEAN_DIFFERENCE : BMESH_ISECT_BOOLEAN_UNION);

  for (BMElem *ele : hidden_here) {
    BM_elem_flag_disable(ele, BM_ELEM_HIDDEN);
  }
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  return SolidResult::Done;
#endif
}

static wmOperatorStatus edbm_extrude_solid_resolve_exec(bContext *C, wmOperator *op)
{
  const Main *bmain = CTX_data_main(C);
  const Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  Vector<Object *> objects = BKE_view_layer_array_from_objects_in_edit_mode_unique_data(
      *bmain, scene, view_layer, CTX_wm_view3d(C));

  bool any_open = false, no_solver = false;
  for (Object *obedit : objects) {
    BMEditMesh *em = BKE_editmesh_from_object(obedit);
    const SolidResult result = extrude_solid_resolve(em);
    orig_co_layer_free(em->bm);
    any_open |= result == SolidResult::Open;
    no_solver |= result == SolidResult::NoSolver;
    if (result == SolidResult::Done) {
      EDBM_selectmode_flush(em);
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

void MESH_OT_extrude_solid_resolve(wmOperatorType *ot)
{
  ot->name = "Extrude Solid Resolve";
  ot->idname = "MESH_OT_extrude_solid_resolve";
  ot->description =
      "Turn the extrude into a solid operation: pushing in cuts, pulling out adds (internal step "
      "of the macro)";

  ot->exec = edbm_extrude_solid_resolve_exec;
  ot->poll = ED_operator_editmesh;

  ot->flag = OPTYPE_INTERNAL;
}

/** \} */

}  // namespace blender
