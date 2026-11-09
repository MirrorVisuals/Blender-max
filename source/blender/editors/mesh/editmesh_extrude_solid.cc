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
#include <string>

#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BLI_math_matrix_types.hh"

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_math_vector.h"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_string.h"
#include "BLI_time.h"
#include "BLI_vector.hh"
#include "BLI_vector_set.hh"

#include "BKE_context.hh"
#include "BKE_customdata.hh"
#include "BKE_editmesh.hh"
#include "BKE_global.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.h"
#include "BKE_report.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "ED_mesh.hh"
#include "ED_screen.hh"
#include "ED_transform.hh"

#include "mesh_intern.hh" /* own include */

#include "GEO_mesh_boolean.hh"

#include "bmesh_tools.hh"
#include "tools/bmesh_boolean.hh"
#include "tools/bmesh_intersect.hh"

namespace blender {

/**
 * Meshes whose extrude was started as a solid, between the first and the last step of the
 * macro. (The move between them is a translation, so where the faces started is found from the
 * mesh itself: nothing is stored on the elements.)
 */
static Set<const Mesh *> &solid_pending()
{
  static Set<const Mesh *> meshes;
  return meshes;
}
/** Outward direction of the faces of the swept solid, zero on other faces (internal). */
static constexpr const char *CUTTER_NO_LAYER = ".extrude_solid_no";

/**
 * With Debug Value 701 (F3 > Debug Menu, or `bpy.app.debug_value` in Python), print how long
 * each stage takes, to the console.
 */
struct StageTimer {
  bool on = G.debug_value == 701;
  double last = on ? BLI_time_now_seconds() : 0.0;
  std::string line;

  void mark(const char *stage)
  {
    if (!on) {
      return;
    }
    const double now = BLI_time_now_seconds();
    char buf[96];
    SNPRINTF(buf, "  %s %.1f", stage, (now - last) * 1000.0);
    line += buf;
    last = now;
  }
  void print(const char *title) const
  {
    if (on) {
      printf("Extrude Manifold %s (ms):%s\n", title, line.c_str());
    }
  }
};

/** The vertices, edges and faces of a connected part of the mesh. */
struct Part {
  Vector<BMVert *> verts;
  Vector<BMEdge *> edges;
  Vector<BMFace *> faces;
};

/**
 * Tag (#BM_ELEM_TAG) every vertex, edge and face connected to `faces` by vertices, and return
 * them.
 */
static Part tag_connected(BMesh *bm, const Span<BMFace *> faces)
{
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_TAG, false);
  Part part;
  for (BMFace *f : faces) {
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      if (!BM_elem_flag_test(l_iter->v, BM_ELEM_TAG)) {
        BM_elem_flag_enable(l_iter->v, BM_ELEM_TAG);
        part.verts.append(l_iter->v);
      }
    } while ((l_iter = l_iter->next) != l_first);
  }
  for (int i = 0; i < part.verts.size(); i++) {
    BMVert *v = part.verts[i];
    BMIter iter;
    BMEdge *e;
    BM_ITER_ELEM (e, &iter, v, BM_EDGES_OF_VERT) {
      if (!BM_elem_flag_test(e, BM_ELEM_TAG)) {
        BM_elem_flag_enable(e, BM_ELEM_TAG);
        part.edges.append(e);
      }
      BMVert *v_other = BM_edge_other_vert(e, v);
      if (!BM_elem_flag_test(v_other, BM_ELEM_TAG)) {
        BM_elem_flag_enable(v_other, BM_ELEM_TAG);
        part.verts.append(v_other);
      }
    }
    BMFace *f;
    BM_ITER_ELEM (f, &iter, v, BM_FACES_OF_VERT) {
      if (!BM_elem_flag_test(f, BM_ELEM_TAG)) {
        BM_elem_flag_enable(f, BM_ELEM_TAG);
        part.faces.append(f);
      }
    }
  }
  return part;
}

/** True when the part is a closed surface that can be treated as a solid. */
static bool part_is_closed(const Part &part)
{
  for (BMEdge *e : part.edges) {
    if (BM_elem_flag_test(e, BM_ELEM_HIDDEN) || !BM_edge_is_manifold(e)) {
      return false;
    }
  }
  for (BMFace *f : part.faces) {
    if (BM_elem_flag_test(f, BM_ELEM_HIDDEN)) {
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

/**
 * Copies elements of `src` into `dst`, each once. Elements of `src` are found by index, so
 * `src` must not change while copying.
 */
struct CopyMaps {
  BMCustomDataCopyMap vert, edge, face, loop;
  Array<BMVert *> verts;
  Array<BMEdge *> edges;

  CopyMaps(BMesh &src, const BMesh &dst)
      : vert(CustomData_bmesh_copy_map_calc(src.vdata, dst.vdata)),
        edge(CustomData_bmesh_copy_map_calc(src.edata, dst.edata)),
        face(CustomData_bmesh_copy_map_calc(src.pdata, dst.pdata)),
        loop(CustomData_bmesh_copy_map_calc(src.ldata, dst.ldata)),
        verts(src.totvert, nullptr),
        edges(src.totedge, nullptr)
  {
    BM_mesh_elem_index_ensure(&src, BM_VERT | BM_EDGE);
  }
};

/** Copy flags without the selection (set through the selection API so counts stay right). */
static char copy_hflag(const char hflag)
{
  return hflag & ~(BM_ELEM_SELECT | BM_ELEM_TAG);
}

static BMVert *copy_vert(BMesh *dst, CopyMaps &maps, BMVert *v)
{
  BMVert *&v_new = maps.verts[BM_elem_index_get(v)];
  if (v_new == nullptr) {
    v_new = BM_vert_create(dst, v->co, nullptr, BM_CREATE_SKIP_CD);
    CustomData_bmesh_copy_block(dst->vdata, maps.vert, v->head.data, &v_new->head.data);
    copy_v3_v3(v_new->no, v->no);
    v_new->head.hflag = copy_hflag(v->head.hflag);
  }
  return v_new;
}

static BMEdge *copy_edge(BMesh *dst, CopyMaps &maps, BMEdge *e)
{
  BMEdge *&e_new = maps.edges[BM_elem_index_get(e)];
  if (e_new == nullptr) {
    e_new = BM_edge_create(
        dst, copy_vert(dst, maps, e->v1), copy_vert(dst, maps, e->v2), nullptr, BM_CREATE_SKIP_CD);
    CustomData_bmesh_copy_block(dst->edata, maps.edge, e->head.data, &e_new->head.data);
    e_new->head.hflag = copy_hflag(e->head.hflag);
  }
  return e_new;
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

/** Copy every face of `src` into `dst`. */
static void copy_all_faces(BMesh *dst, BMesh *src)
{
  CopyMaps maps(*src, *dst);
  BMIter iter;
  BMFace *f;
  BM_ITER_MESH (f, &iter, src, BM_FACES_OF_MESH) {
    copy_face(dst, maps, f);
  }
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
  const bool closed = part_is_closed(tag_connected(bm, faces));
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
    const Mesh *mesh = id_cast<const Mesh *>(obedit->data);
    solid_pending().remove(mesh);
    if (bm->totvertsel == 0) {
      continue;
    }

    StageTimer timer;
    bool solid = false;
#ifdef WITH_GMP
    solid = use_solid && bm->totfacesel > 0 && selection_is_closed_solid(bm);
#else
    UNUSED_VARS(use_solid);
#endif
    timer.mark("closed check");
    if (solid) {
      solid_pending().add(mesh);
    }

    /* A solid is rebuilt from the plain extrude, so its side edges must not be dissolved. */
    if (!edbm_extrude_mesh_ex(
            obedit, em, op, use_normal_flip, solid ? false : use_dissolve_ortho_edges))
    {
      solid_pending().remove(mesh);
      continue;
    }
    timer.mark("extrude");

    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    params.is_destructive = true;
    EDBM_update(id_cast<Mesh *>(obedit->data), &params);
    timer.mark("update");
    timer.print("extrude step");
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

struct BooleanShapes {
  const Set<BMFace *> *cutter;
};

static int extrude_solid_shape_fn(BMFace *f, void *user_data)
{
  const BooleanShapes *shapes = static_cast<const BooleanShapes *>(user_data);
  return shapes->cutter->contains(f) ? 1 : 0;
}

/** Convert a #BMesh to a new #Mesh (not in Main). */
static Mesh *mesh_from_bmesh(BMesh *bm)
{
  Mesh *mesh = BKE_mesh_new_nomain(0, 0, 0, 0);
  BMeshToMeshParams params{};
  BM_mesh_bm_to_me(nullptr, bm, mesh, &params);
  return mesh;
}

/**
 * The Boolean with the fast Manifold solver, the result added to `bm`.
 * False when the solver could not do it; `bm` is then unchanged.
 */
static bool solid_boolean_manifold(
    BMesh *bm, BMesh *part, BMesh *cutter_bm, const bool push_in, StageTimer &timer)
{
  Mesh *mesh_part = mesh_from_bmesh(part);
  Mesh *mesh_cutter = mesh_from_bmesh(cutter_bm);
  timer.mark("to meshes");
  const Mesh *meshes[2] = {mesh_part, mesh_cutter};
  const float4x4 transforms[2] = {float4x4::identity(), float4x4::identity()};
  geometry::boolean::BooleanOpParameters op_params;
  op_params.boolean_mode = push_in ? geometry::boolean::Operation::Difference :
                                     geometry::boolean::Operation::Union;
  geometry::boolean::BooleanError error;
  Mesh *result = geometry::boolean::mesh_boolean(Span<const Mesh *>(meshes, 2),
                                                 Span<float4x4>(transforms, 2),
                                                 {},
                                                 op_params,
                                                 geometry::boolean::Solver::Manifold,
                                                 nullptr,
                                                 &error);
  BKE_id_free(nullptr, mesh_part);
  BKE_id_free(nullptr, mesh_cutter);
  timer.mark("manifold boolean");
  if (result == nullptr || error.type != geometry::boolean::BooleanErrorType::NoError) {
    if (result) {
      BKE_id_free(nullptr, result);
    }
    return false;
  }

  BMeshCreateParams create_params{};
  BMesh *bm_result = BM_mesh_create(&bm_mesh_allocsize_default, &create_params);
  BMeshFromMeshParams from_params{};
  from_params.calc_face_normal = true;
  from_params.calc_vert_normal = true;
  BM_mesh_bm_from_me(bm_result, result, &from_params);
  BKE_id_free(nullptr, result);
  timer.mark("from mesh");
  copy_all_faces(bm, bm_result);
  BM_mesh_free(bm_result);
  timer.mark("copy back");
  return true;
}

/**
 * The Boolean with the exact solver, done in `part` (the swept solid is copied into it).
 */
static void solid_boolean_exact(BMesh *part, BMesh *cutter_bm, const bool push_in)
{
  /* The faces of the solid, copied into the part. */
  const int part_faces = part->totface;
  copy_all_faces(part, cutter_bm);
  Set<BMFace *> cutter;
  {
    BMIter iter;
    BMFace *f;
    int i = 0;
    BM_ITER_MESH_INDEX (f, &iter, part, BM_FACES_OF_MESH, i) {
      if (i >= part_faces) {
        cutter.add(f);
      }
    }
  }

  /* Remember which way each face of the solid points. The Boolean keeps a face that it does
   * not cut as it is, even where a Difference needs it turned around (the floor of a cut that
   * touches nothing), so those faces are flipped afterwards. Faces it creates copy the value. */
  BM_data_layer_add_named(part, &part->pdata, CD_PROP_FLOAT3, CUTTER_NO_LAYER);
  const int no_offset = CustomData_get_offset_named(&part->pdata, CD_PROP_FLOAT3, CUTTER_NO_LAYER);
  {
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, part, BM_FACES_OF_MESH) {
      BM_face_normal_update(f);
      float *no = static_cast<float *>(BM_ELEM_CD_GET_VOID_P(f, no_offset));
      if (cutter.contains(f)) {
        copy_v3_v3(no, f->no);
      }
      else {
        zero_v3(no);
      }
    }
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
}

static SolidResult extrude_solid_resolve(BMEditMesh *em, StageTimer &timer)
{
  BMesh *bm = em->bm;

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

  /* (The first step checked that the part is closed; extruding it keeps it closed.) */

  VectorSet<BMVert *> cap_verts;
  for (BMFace *f : caps) {
    BMLoop *l_iter, *l_first;
    l_iter = l_first = BM_FACE_FIRST_LOOP(f);
    do {
      cap_verts.add(l_iter->v);
    } while ((l_iter = l_iter->next) != l_first);
  }

  /* Each vertex on the border of the caps is joined by a side edge to the vertex it was
   * extruded from, which did not move. */
  Map<BMVert *, BMVert *> source_of;
  Vector<BMEdge *> side_edges;
  for (BMVert *v : cap_verts) {
    BMIter iter;
    BMEdge *e;
    BM_ITER_ELEM (e, &iter, v, BM_EDGES_OF_VERT) {
      BMVert *v_other = BM_edge_other_vert(e, v);
      if (!cap_verts.contains(v_other)) {
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
  /* The move is a translation: every vertex moved as far as the border ones. */
  const auto first = *source_of.items().begin();
  const float3 move = float3(first.key->co) - float3(first.value->co);
  for (const auto item : source_of.items()) {
    const float3 d = float3(item.key->co) - float3(item.value->co);
    if (math::distance(d, move) > 1e-4f * math::length(move) + 1e-6f) {
      /* Not one translation (not what this macro does): keep the plain extrude. */
      return SolidResult::None;
    }
  }
  auto orig_co = [&](BMVert *v) -> float3 { return float3(v->co) - move; };

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
    if (e_dst != e_src && BM_vert_in_edge(e_src, e_dst->v1) && BM_vert_in_edge(e_src, e_dst->v2)) {
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

  timer.mark("rebuild");
  /* Copy the edited part into its own mesh: the Boolean then only works on that part, however
   * big the rest of the mesh is. */
  const Part edited = tag_connected(bm, caps);
  /* The part is the whole mesh (one piece): it is converted directly and the result replaces the
   * mesh. Otherwise the part is copied into its own mesh and removed, and the result added. */
  const bool whole_mesh = edited.verts.size() == bm->totvert && edited.faces.size() == bm->totface;
  BMeshCreateParams create_params{};
  create_params.use_toolflags = true;
  /* Where the edited part is: the mesh itself, or a copy. */
  BMesh *source = bm;
  BMesh *part = nullptr;
  Vector<BMFace *> caps_source = caps;
  if (!whole_mesh) {
    part = BM_mesh_create(&bm_mesh_allocsize_default, &create_params);
    BM_mesh_copy_init_customdata(part, bm, &bm_mesh_allocsize_default);
    CopyMaps to_part(*bm, *part);
    Set<BMFace *> caps_set(caps);
    caps_source.clear();
    for (BMFace *f : edited.faces) {
      BMFace *f_new = copy_face(part, to_part, f);
      if (caps_set.contains(f)) {
        caps_source.append(f_new);
      }
    }
    Map<BMVert *, float3> moved_part;
    for (const auto item : moved_kept.items()) {
      moved_part.add(to_part.verts[BM_elem_index_get(item.key)], item.value);
    }
    moved_kept = std::move(moved_part);
    source = part;
    timer.mark("copy out");
    /* Remove the part from the mesh; the result is added at the end. */
    BM_select_history_clear(bm);
    for (BMVert *v_kill : edited.verts) {
      BM_vert_kill(bm, v_kill);
    }
    timer.mark("remove part");
  }
  else {
    BM_select_history_clear(bm);
  }

  /* The swept solid, in a mesh of its own. It starts a little behind the faces, so the faces
   * themselves end up inside it and are removed. */
  BMesh *cutter_bm = BM_mesh_create(&bm_mesh_allocsize_default, &create_params);
  BM_mesh_copy_init_customdata(cutter_bm, source, &bm_mesh_allocsize_default);
  Vector<BMFace *> new_tops;
  {
    const BMCustomDataCopyMap face_map = CustomData_bmesh_copy_map_calc(source->pdata,
                                                                        cutter_bm->pdata);
    const BMCustomDataCopyMap loop_map = CustomData_bmesh_copy_map_calc(source->ldata,
                                                                        cutter_bm->ldata);
    const float eps = std::max(max_move * 1e-3f, max_coord * 1e-6f);
    const float3 fallback_dir = math::normalize(sum_move);
    Map<BMVert *, BMVert *> bottom, top;
    for (const auto item : moved_kept.items()) {
      BMVert *v = item.key;
      const float3 d = item.value;
      const float len = math::length(d);
      const float3 dir = len > 1e-8f ? d / len : fallback_dir;
      bottom.add(v, BM_vert_create(cutter_bm, float3(v->co) - dir * eps, nullptr, BM_CREATE_NOP));
      top.add(v, BM_vert_create(cutter_bm, float3(v->co) + d, nullptr, BM_CREATE_NOP));
    }
    /* A face of the solid, with the face data of `f_src` and the corner data of `loops_src`. */
    auto add_face = [&](const Span<BMVert *> verts,
                        BMFace *f_src,
                        const Span<BMLoop *> loops_src) {
      BMFace *f_new = BM_face_create_verts(cutter_bm,
                                           const_cast<BMVert **>(verts.data()),
                                           verts.size(),
                                           nullptr,
                                           BM_CREATE_NOP,
                                           true);
      if (f_new == nullptr) {
        return f_new;
      }
      CustomData_bmesh_copy_block(cutter_bm->pdata, face_map, f_src->head.data, &f_new->head.data);
      f_new->mat_nr = f_src->mat_nr;
      BMLoop *l_new = BM_FACE_FIRST_LOOP(f_new);
      for (BMLoop *l_src : loops_src) {
        CustomData_bmesh_copy_block(
            cutter_bm->ldata, loop_map, l_src->head.data, &l_new->head.data);
        l_new = l_new->next;
      }
      return f_new;
    };

    Set<BMFace *> caps_set(caps_source);
    for (BMFace *f : caps_source) {
      Vector<BMVert *, 8> verts_bottom, verts_top;
      Vector<BMLoop *, 8> loops;
      BMLoop *l_iter, *l_first;
      l_iter = l_first = BM_FACE_FIRST_LOOP(f);
      do {
        verts_bottom.append(bottom.lookup(l_iter->v));
        verts_top.append(top.lookup(l_iter->v));
        loops.append(l_iter);
      } while ((l_iter = l_iter->next) != l_first);
      add_face(verts_bottom, f, loops);
      if (BMFace *f_top = add_face(verts_top, f, loops)) {
        new_tops.append(f_top);
      }

      /* Side walls along the border of the caps. */
      l_iter = l_first;
      do {
        if (l_iter->radial_next != l_iter && caps_set.contains(l_iter->radial_next->f)) {
          continue;
        }
        BMVert *a = l_iter->v, *b = l_iter->next->v;
        const BMVert *quad[4] = {bottom.lookup(a), bottom.lookup(b), top.lookup(b), top.lookup(a)};
        BMLoop *quad_loops[4] = {l_iter, l_iter->next, l_iter->next, l_iter};
        add_face(Span<BMVert *>(const_cast<BMVert **>(quad), 4), f, Span<BMLoop *>(quad_loops, 4));
      } while ((l_iter = l_iter->next) != l_first);
    }
  }
  /* Outward normals for the solid. */
  {
    BMIter iter;
    BMFace *f;
    BM_ITER_MESH (f, &iter, cutter_bm, BM_FACES_OF_MESH) {
      /* New faces have no normal yet; recalculating the directions needs one. */
      BM_face_normal_update(f);
      BM_elem_flag_enable(f, BM_ELEM_TAG);
    }
    BMO_op_callf(cutter_bm, BMO_FLAG_DEFAULTS, "recalc_face_normals faces=%hf", BM_ELEM_TAG);
  }
  /* The result keeps the selection of the faces it comes from: select the new caps only. */
  BM_mesh_elem_hflag_disable_all(source, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
  BM_mesh_elem_hflag_disable_all(cutter_bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
  for (BMFace *f : new_tops) {
    BM_face_select_set(cutter_bm, f, true);
  }

  const bool push_in = math::dot(sum_move, sum_normal) < 0.0f;
  timer.mark("swept solid");
  /* Where the result goes: the mesh, or a new mesh with the same layout that replaces it. */
  BMesh *target = bm;
  if (whole_mesh) {
    BMeshCreateParams target_params{};
    target_params.use_toolflags = bm->use_toolflags;
    target = BM_mesh_create(&bm_mesh_allocsize_default, &target_params);
    BM_mesh_copy_init_customdata(target, bm, &bm_mesh_allocsize_default);
  }
  bool done = solid_boolean_manifold(target, source, cutter_bm, push_in, timer);
  if (!done) {
    /* The fast solver could not do it (e.g. the swept solid crosses itself): use the exact one,
     * on a copy of the part. */
    if (part == nullptr) {
      part = BM_mesh_create(&bm_mesh_allocsize_default, &create_params);
      BM_mesh_copy_init_customdata(part, bm, &bm_mesh_allocsize_default);
      copy_all_faces(part, bm);
    }
    solid_boolean_exact(part, cutter_bm, push_in);
    timer.mark("exact boolean");
    copy_all_faces(target, part);
    timer.mark("copy back");
  }
  BM_mesh_free(cutter_bm);
  if (part) {
    BM_mesh_free(part);
  }
  if (whole_mesh) {
    /* Swap the contents, keeping what belongs to the edit-mesh itself. */
    std::swap(*bm, *target);
    std::swap(bm->py_handle, target->py_handle);
    bm->selectmode = target->selectmode;
    bm->shapenr = target->shapenr;
    bm->spacearr_dirty = BM_SPACEARR_DIRTY_ALL;
    BM_mesh_free(target);
  }
  timer.mark("free");
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
    const bool is_solid = solid_pending().remove(id_cast<const Mesh *>(obedit->data));
    StageTimer timer;
    const SolidResult result = is_solid ? extrude_solid_resolve(em, timer) : SolidResult::None;
    any_open |= result == SolidResult::Open;
    no_solver |= result == SolidResult::NoSolver;
    if (result == SolidResult::Done && em->selectmode != SCE_SELECT_FACE) {
      /* In face select mode the selection set on the faces is already complete. */
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
    timer.mark("merge/flush");
    EDBMUpdate_Params params{};
    params.calc_looptris = true;
    params.calc_normals = true;
    params.is_destructive = true;
    EDBM_update(id_cast<Mesh *>(obedit->data), &params);
    timer.mark("update");
    timer.print("finish step");
  }
  if (no_solver) {
    BKE_report(op->reports, RPT_WARNING, "Built without the exact solver: normal extrude kept");
  }
  else if (any_open) {
    BKE_report(
        op->reports, RPT_INFO, "The mesh is open (not a closed solid): normal extrude kept");
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
