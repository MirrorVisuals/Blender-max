/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include <cmath>
#include <cstdlib>
#include <sstream>

#include "BLI_array.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_string_ref.hh"

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_customdata.hh"
#include "BKE_lib_id.hh"
#include "BKE_mesh.h"
#include "BKE_mesh.hh"
#include "BKE_stable_id_mesh.hh"

#include "bmesh.hh"

#include "MOD_edit_poly_ops.hh"

namespace blender::edit_poly {

using namespace bke::stable_id;

/* -------------------------------------------------------------------- */
/** \name Text format
 * \{ */

static Vector<std::string> split(const std::string &text, const char sep)
{
  Vector<std::string> parts;
  std::string current;
  for (const char c : text) {
    if (c == sep) {
      parts.append(current);
      current.clear();
    }
    else {
      current += c;
    }
  }
  parts.append(current);
  return parts;
}

static bool parse_int(const std::string &s, int64_t &r_value)
{
  if (s.empty()) {
    return false;
  }
  char *end = nullptr;
  r_value = std::strtoll(s.c_str(), &end, 10);
  return end && *end == '\0';
}

static bool parse_float(const std::string &s, float &r_value)
{
  if (s.empty()) {
    return false;
  }
  char *end = nullptr;
  r_value = std::strtof(s.c_str(), &end);
  return end && *end == '\0';
}

static bool parse_id(const std::string &lo, const std::string &hi, StableID &r_id)
{
  int64_t a, b;
  if (!parse_int(lo, a) || !parse_int(hi, b)) {
    return false;
  }
  r_id = StableID{uint32_t(a), uint32_t(b)};
  return true;
}

static bool parse_footprints(const std::string &value, OpRecord &op)
{
  for (const std::string &item : split(value, ';')) {
    Footprint fp;
    if (item != "-") {
      const Vector<std::string> fields = split(item, ':');
      if (fields.size() < 2 || fields.size() % 2 != 0) {
        return false;
      }
      if (!parse_id(fields[0], fields[1], fp.parent)) {
        return false;
      }
      for (int i = 2; i < fields.size(); i += 2) {
        float u, v;
        if (!parse_float(fields[i], u) || !parse_float(fields[i + 1], v)) {
          return false;
        }
        fp.uv.append(float2(u, v));
      }
    }
    op.footprints.append(std::move(fp));
  }
  return true;
}

bool ops_parse(const StringRef text, Vector<OpRecord> &r_ops)
{
  r_ops.clear();
  for (std::string line : split(std::string(text), '\n')) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    const Vector<std::string> tokens = split(line, ' ');
    OpRecord op;
    if (tokens[0] == "EXTRUDE") {
      op.type = OpType::Extrude;
    }
    else if (tokens[0] == "INSET") {
      op.type = OpType::Inset;
    }
    else if (tokens[0] == "BEVEL") {
      op.type = OpType::Bevel;
    }
    else {
      r_ops.clear();
      return false;
    }
    for (int t = 1; t < tokens.size(); t++) {
      const std::string &token = tokens[t];
      const size_t eq = token.find('=');
      if (eq == std::string::npos) {
        continue;
      }
      const std::string key = token.substr(0, eq);
      const std::string value = token.substr(eq + 1);
      bool ok = true;
      if (key == "params") {
        for (const std::string &p : split(value, ',')) {
          float f;
          ok &= parse_float(p, f);
          op.params.append(f);
        }
      }
      else if (key == "faces" && !value.empty()) {
        for (const std::string &item : split(value, ',')) {
          const Vector<std::string> f = split(item, ':');
          StableID id;
          ok &= f.size() == 2 && parse_id(f[0], f[1], id);
          op.faces.append(id);
        }
      }
      else if (key == "edges" && !value.empty()) {
        for (const std::string &item : split(value, ',')) {
          const Vector<std::string> f = split(item, ':');
          StableID a, b;
          ok &= f.size() == 4 && parse_id(f[0], f[1], a) && parse_id(f[2], f[3], b);
          op.edges.append({a, b});
        }
      }
      else if (key == "fp") {
        ok &= parse_footprints(value, op);
      }
      if (!ok) {
        r_ops.clear();
        return false;
      }
    }
    const int64_t expected_params = op.type == OpType::Extrude ? 3 :
                                    op.type == OpType::Inset   ? 9 :
                                                                 15;
    if (op.params.size() != expected_params || op.elements_num() == 0 ||
        (!op.footprints.is_empty() && op.footprints.size() != op.elements_num()))
    {
      r_ops.clear();
      return false;
    }
    r_ops.append(std::move(op));
  }
  return true;
}

std::string ops_to_text(const Span<OpRecord> ops)
{
  std::ostringstream out;
  out.precision(9);
  for (const OpRecord &op : ops) {
    out << (op.type == OpType::Extrude ? "EXTRUDE" :
            op.type == OpType::Inset   ? "INSET" :
                                         "BEVEL");
    out << " params=";
    for (const int i : op.params.index_range()) {
      out << (i ? "," : "") << op.params[i];
    }
    if (op.type == OpType::Bevel) {
      out << " edges=";
      for (const int i : op.edges.index_range()) {
        out << (i ? "," : "") << op.edges[i].first.lo << ":" << op.edges[i].first.hi << ":"
            << op.edges[i].second.lo << ":" << op.edges[i].second.hi;
      }
    }
    else {
      out << " faces=";
      for (const int i : op.faces.index_range()) {
        out << (i ? "," : "") << op.faces[i].lo << ":" << op.faces[i].hi;
      }
    }
    if (!op.footprints.is_empty()) {
      out << " fp=";
      for (const int i : op.footprints.index_range()) {
        const Footprint &fp = op.footprints[i];
        out << (i ? ";" : "");
        if (!fp.is_valid()) {
          out << "-";
          continue;
        }
        out << fp.parent.lo << ":" << fp.parent.hi;
        for (const float2 &uv : fp.uv) {
          out << ":" << uv.x << ":" << uv.y;
        }
      }
    }
    out << "\n";
  }
  return out.str();
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Footprints
 * \{ */

/** Mesh data needed to compute and match footprints. */
struct FootprintData {
  Array<StableID> vert_ids;
  Array<StableID> face_ids;
  VArraySpan<int2> face_parents;
  VArraySpan<float2> corner_uv;
  OffsetIndices<int> faces;
  Span<int> corner_verts;
  bool valid = false;

  explicit FootprintData(const Mesh &mesh)
  {
    vert_ids = read_or_make_root_ids(mesh, bke::AttrDomain::Point);
    face_ids = read_or_make_root_ids(mesh, bke::AttrDomain::Face);
    faces = mesh.faces();
    corner_verts = mesh.corner_verts();
    const bke::AttributeAccessor attributes = mesh.attributes();
    const bke::AttributeReader<int2> parents = attributes.lookup<int2>(ATTR_PARENT_FACE);
    const bke::AttributeReader<float2> uv = attributes.lookup<float2>(ATTR_SUBD_UV);
    if (parents && uv && parents.domain == bke::AttrDomain::Face &&
        uv.domain == bke::AttrDomain::Corner)
    {
      face_parents = *parents;
      corner_uv = *uv;
      valid = true;
    }
  }

  StableID parent(const int face) const
  {
    return StableID{uint32_t(face_parents[face].x), uint32_t(face_parents[face].y)};
  }
};

void ops_compute_footprints(const Mesh &input, MutableSpan<OpRecord> ops)
{
  const FootprintData data(input);
  Map<uint64_t, int> face_of;
  Map<uint64_t, int> vert_of;
  for (const int i : data.face_ids.index_range()) {
    face_of.add(data.face_ids[i].as_u64(), i);
  }
  for (const int i : data.vert_ids.index_range()) {
    vert_of.add(data.vert_ids[i].as_u64(), i);
  }
  const GroupedSpan<int> vert_to_face = input.vert_to_face_map();

  for (OpRecord &op : ops) {
    op.footprints.clear();
    for (int64_t e = 0; e < op.elements_num(); e++) {
      Footprint fp;
      if (data.valid) {
        if (op.type != OpType::Bevel) {
          const int face = face_of.lookup_default(op.faces[e].as_u64(), -1);
          if (face != -1) {
            fp.parent = data.parent(face);
            for (const int corner : data.faces[face]) {
              fp.uv.append(data.corner_uv[corner]);
            }
          }
        }
        else {
          const int a = vert_of.lookup_default(op.edges[e].first.as_u64(), -1);
          const int b = vert_of.lookup_default(op.edges[e].second.as_u64(), -1);
          if (a != -1 && b != -1) {
            /* Any face that has this edge; its corners give both positions. */
            for (const int face : vert_to_face[a]) {
              const IndexRange corners = data.faces[face];
              int ca = -1, cb = -1;
              for (const int c : corners) {
                ca = data.corner_verts[c] == a ? c : ca;
                cb = data.corner_verts[c] == b ? c : cb;
              }
              if (ca != -1 && cb != -1) {
                fp.parent = data.parent(face);
                fp.uv = {data.corner_uv[ca], data.corner_uv[cb]};
                break;
              }
            }
          }
        }
      }
      op.footprints.append(std::move(fp));
    }
  }
}

bool ops_can_retarget(const Span<OpRecord> ops)
{
  for (const OpRecord &op : ops) {
    if (op.footprints.size() != op.elements_num()) {
      return false;
    }
    for (const Footprint &fp : op.footprints) {
      if (!fp.is_valid()) {
        return false;
      }
    }
  }
  return !ops.is_empty();
}

static bool point_in_polygon(const float2 &p, const Span<float2> polygon)
{
  bool inside = false;
  const int n = int(polygon.size());
  for (int i = 0, j = n - 1; i < n; j = i++) {
    const float2 &a = polygon[i];
    const float2 &b = polygon[j];
    if (((a.y > p.y) != (b.y > p.y)) && (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)) {
      inside = !inside;
    }
  }
  return inside;
}

static float2 centroid(const Span<float2> points)
{
  float2 sum(0.0f);
  for (const float2 &p : points) {
    sum += p;
  }
  return points.is_empty() ? sum : sum / float(points.size());
}

static bool on_segment(const float2 &p, const float2 &a, const float2 &b)
{
  const float eps = 1e-4f;
  const float2 ab = b - a;
  const float len_sq = math::length_squared(ab);
  if (len_sq < eps * eps) {
    return math::distance(p, a) < eps;
  }
  const float cross = ab.x * (p.y - a.y) - ab.y * (p.x - a.x);
  if (std::abs(cross) > eps * std::sqrt(len_sq)) {
    return false;
  }
  const float t = math::dot(p - a, ab) / len_sq;
  return t >= -eps && t <= 1.0f + eps;
}

/** IDs of the faces of `mesh` covered by a face footprint. */
static void retarget_faces(const FootprintData &data,
                           const Map<uint64_t, Vector<int>> &faces_of_parent,
                           const Footprint &fp,
                           Vector<StableID> &r_faces)
{
  const Vector<int> *candidates = faces_of_parent.lookup_ptr(fp.parent.as_u64());
  if (!candidates) {
    return;
  }
  const float2 fp_center = centroid(fp.uv);
  Vector<float2, 8> uv;
  for (const int face : *candidates) {
    uv.clear();
    for (const int corner : data.faces[face]) {
      uv.append(data.corner_uv[corner]);
    }
    /* Finer level: the face lies inside the footprint. Coarser level: the footprint lies inside
     * the face. */
    if (point_in_polygon(centroid(uv), fp.uv) || point_in_polygon(fp_center, uv)) {
      r_faces.append(data.face_ids[face]);
    }
  }
}

/** Edges (as vertex ID pairs) of `mesh` covered by an edge footprint. */
static void retarget_edges(const FootprintData &data,
                           const Map<uint64_t, Vector<int>> &faces_of_parent,
                           const Footprint &fp,
                           Vector<std::pair<StableID, StableID>> &r_edges)
{
  const Vector<int> *candidates = faces_of_parent.lookup_ptr(fp.parent.as_u64());
  if (!candidates || fp.uv.size() != 2) {
    return;
  }
  Set<std::pair<uint64_t, uint64_t>> seen;
  for (const int face : *candidates) {
    const IndexRange corners = data.faces[face];
    for (const int i : IndexRange(corners.size())) {
      const int c0 = corners[i];
      const int c1 = corners[(i + 1) % corners.size()];
      if (on_segment(data.corner_uv[c0], fp.uv[0], fp.uv[1]) &&
          on_segment(data.corner_uv[c1], fp.uv[0], fp.uv[1]))
      {
        const StableID a = data.vert_ids[data.corner_verts[c0]];
        const StableID b = data.vert_ids[data.corner_verts[c1]];
        const uint64_t ka = a.as_u64(), kb = b.as_u64();
        if (seen.add({std::min(ka, kb), std::max(ka, kb)})) {
          r_edges.append({a, b});
        }
      }
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Replay
 * \{ */

static StableID read_id(const void *ptr)
{
  const int2 &value = *static_cast<const int2 *>(ptr);
  return StableID{uint32_t(value.x), uint32_t(value.y)};
}

/** Give every element of the result a unique ID: an element that is the first with an input ID
 * keeps it, copies made by the operations get IDs derived from the one they copied. */
static void assign_result_ids(Mesh &result,
                              const Span<StableID> in_vert_ids,
                              const Span<StableID> in_edge_ids,
                              const Span<int2> in_edges)
{
  for (const bke::AttrDomain domain : {bke::AttrDomain::Point, bke::AttrDomain::Face}) {
    const Array<StableID> copied = read_or_make_root_ids(result, domain);
    Array<StableID> ids(copied.size());
    Array<StableID> parents(copied.size(), StableID{});
    Set<uint64_t> used;
    for (const int i : copied.index_range()) {
      if (copied[i].is_valid() && used.add(copied[i].as_u64())) {
        ids[i] = copied[i];
      }
    }
    Map<uint64_t, uint32_t> copies;
    for (const int i : copied.index_range()) {
      if (ids[i].is_valid()) {
        continue;
      }
      uint32_t &count = copies.lookup_or_add(copied[i].as_u64(), 0);
      StableID id;
      do {
        id = derive(Derivation::Generated, copied[i], 0x0B5, ++count);
      } while (!used.add(id.as_u64()));
      ids[i] = id;
      parents[i] = copied[i];
    }
    write_ids(result, domain, ids, parents);
  }

  const Array<StableID> vert_ids = read_or_make_root_ids(result, bke::AttrDomain::Point);
  Map<std::pair<uint64_t, uint64_t>, StableID> input_edge;
  for (const int e : in_edges.index_range()) {
    const uint64_t a = in_vert_ids[in_edges[e][0]].as_u64();
    const uint64_t b = in_vert_ids[in_edges[e][1]].as_u64();
    input_edge.add({std::min(a, b), std::max(a, b)}, in_edge_ids[e]);
  }
  const Span<int2> edges = result.edges();
  Array<StableID> edge_ids(edges.size());
  Set<uint64_t> used;
  for (const int e : edges.index_range()) {
    const StableID a = vert_ids[edges[e][0]], b = vert_ids[edges[e][1]];
    const uint64_t ka = a.as_u64(), kb = b.as_u64();
    StableID id = input_edge.lookup_default({std::min(ka, kb), std::max(ka, kb)}, StableID{});
    if (!id.is_valid() || !used.add(id.as_u64())) {
      uint32_t salt = 3;
      do {
        id = derive_unordered(Derivation::Generated, a, b, salt++);
      } while (!used.add(id.as_u64()));
    }
    edge_ids[e] = id;
  }
  write_ids(result, bke::AttrDomain::Edge, edge_ids, {});
}

static bool run_operation(BMesh *bm, const OpRecord &op)
{
  const Span<float> p = op.params;
  switch (op.type) {
    case OpType::Extrude: {
      BMOperator bmop;
      BMO_op_init(bm, &bmop, BMO_FLAG_DEFAULTS, "extrude_face_region");
      BMO_slot_buffer_from_enabled_hflag(
          bm, &bmop, bmop.slots_in, "geom", BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT);
      BMO_op_exec(bm, &bmop);
      const float3 offset(p[0], p[1], p[2]);
      BMOIter siter;
      BMVert *v;
      BMO_ITER (v, &siter, bmop.slots_out, "geom.out", BM_VERT) {
        v->co[0] += offset.x;
        v->co[1] += offset.y;
        v->co[2] += offset.z;
      }
      BMO_op_finish(bm, &bmop);
      return true;
    }
    case OpType::Inset: {
      if (p[0] != 0.0f) {
        return BMO_op_callf(bm,
                            BMO_FLAG_DEFAULTS,
                            "inset_individual faces=%hf use_even_offset=%b "
                            "use_relative_offset=%b use_interpolate=%b thickness=%f depth=%f",
                            BM_ELEM_SELECT,
                            p[3] != 0.0f,
                            p[4] != 0.0f,
                            p[5] != 0.0f,
                            p[1],
                            p[2]);
      }
      return BMO_op_callf(bm,
                          BMO_FLAG_DEFAULTS,
                          "inset_region faces=%hf use_boundary=%b use_even_offset=%b "
                          "use_relative_offset=%b use_interpolate=%b thickness=%f depth=%f "
                          "use_outset=%b use_edge_rail=%b",
                          BM_ELEM_SELECT,
                          p[6] != 0.0f,
                          p[3] != 0.0f,
                          p[4] != 0.0f,
                          p[5] != 0.0f,
                          p[1],
                          p[2],
                          p[7] != 0.0f,
                          p[8] != 0.0f);
    }
    case OpType::Bevel: {
      return BMO_op_callf(bm,
                          BMO_FLAG_DEFAULTS,
                          "bevel geom=%hev offset=%f segments=%i affect=%i offset_type=%i "
                          "profile_type=%i profile=%f clamp_overlap=%b material=%i "
                          "loop_slide=%b mark_seam=%b mark_sharp=%b harden_normals=%b "
                          "face_strength_mode=%i miter_outer=%i miter_inner=%i spread=%f "
                          "custom_profile=%p vmesh_method=%i",
                          BM_ELEM_SELECT,
                          p[0],
                          int(p[2]),
                          /* BEVEL_AFFECT_EDGES */ 1,
                          int(p[1]),
                          /* Superellipse only: custom profiles are not logged. */ 0,
                          p[4],
                          p[5] != 0.0f,
                          int(p[13]),
                          p[6] != 0.0f,
                          p[11] != 0.0f,
                          p[12] != 0.0f,
                          false,
                          int(p[14]),
                          int(p[7]),
                          int(p[8]),
                          p[9],
                          nullptr,
                          int(p[10]));
    }
  }
  return false;
}

Mesh *ops_apply(const Mesh &input,
                const Span<OpRecord> ops,
                const bool retarget,
                std::string &r_reason)
{
  if (ops.is_empty()) {
    r_reason = "no operations";
    return nullptr;
  }
  if (retarget && !ops_can_retarget(ops)) {
    r_reason = "the operations cannot be re-targeted";
    return nullptr;
  }

  /* Work on a copy with explicit IDs, so they travel through BMesh as attributes. */
  Mesh *src = BKE_mesh_copy_for_eval(input);
  ensure_root_ids(*src);
  const Array<StableID> in_vert_ids = read_or_make_root_ids(*src, bke::AttrDomain::Point);
  const Array<StableID> in_edge_ids = read_or_make_root_ids(*src, bke::AttrDomain::Edge);
  const Array<int2> in_edges(src->edges());

  /* Targets of every operation, as input IDs. */
  Vector<Vector<StableID>> op_faces(ops.size());
  Vector<Vector<std::pair<StableID, StableID>>> op_edges(ops.size());
  if (retarget) {
    const FootprintData data(*src);
    if (!data.valid) {
      BKE_id_free(nullptr, src);
      r_reason = "the mesh below is not subdivided";
      return nullptr;
    }
    Map<uint64_t, Vector<int>> faces_of_parent;
    for (const int f : data.faces.index_range()) {
      faces_of_parent.lookup_or_add_default(data.parent(f).as_u64()).append(f);
    }
    for (const int i : ops.index_range()) {
      for (const Footprint &fp : ops[i].footprints) {
        if (ops[i].type == OpType::Bevel) {
          retarget_edges(data, faces_of_parent, fp, op_edges[i]);
        }
        else {
          retarget_faces(data, faces_of_parent, fp, op_faces[i]);
        }
      }
    }
  }
  else {
    for (const int i : ops.index_range()) {
      op_faces[i] = ops[i].faces;
      op_edges[i] = ops[i].edges;
    }
  }

  BMeshCreateParams create_params{};
  create_params.use_toolflags = true;
  BMeshFromMeshParams convert_params{};
  convert_params.calc_face_normal = true;
  convert_params.calc_vert_normal = true;
  BMesh *bm = BKE_mesh_to_bmesh_ex(src, &create_params, &convert_params);
  const int vert_offset = CustomData_get_offset_named(
      &bm->vdata, CD_PROP_INT32_2D, ATTR_ID_POINT);
  const int face_offset = CustomData_get_offset_named(
      &bm->pdata, CD_PROP_INT32_2D, ATTR_ID_FACE);
  if (vert_offset == -1 || face_offset == -1) {
    BM_mesh_free(bm);
    BKE_id_free(nullptr, src);
    r_reason = "stable IDs are missing";
    return nullptr;
  }

  for (const int i : ops.index_range()) {
    BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);
    int selected = 0;
    if (ops[i].type == OpType::Bevel) {
      Map<uint64_t, Vector<BMVert *>> verts_of;
      BMIter iter;
      BMVert *v;
      BM_ITER_MESH (v, &iter, bm, BM_VERTS_OF_MESH) {
        verts_of.lookup_or_add_default(read_id(BM_ELEM_CD_GET_VOID_P(v, vert_offset)).as_u64())
            .append(v);
      }
      for (const auto &[a, b] : op_edges[i]) {
        const Vector<BMVert *> *va = verts_of.lookup_ptr(a.as_u64());
        const Vector<BMVert *> *vb = verts_of.lookup_ptr(b.as_u64());
        if (!va || !vb) {
          continue;
        }
        for (BMVert *x : *va) {
          for (BMVert *y : *vb) {
            if (BMEdge *e = BM_edge_exists(x, y)) {
              BM_edge_select_set(bm, e, true);
              selected++;
            }
          }
        }
      }
    }
    else {
      Set<uint64_t> wanted;
      for (const StableID id : op_faces[i]) {
        wanted.add(id.as_u64());
      }
      BMIter iter;
      BMFace *f;
      BM_ITER_MESH (f, &iter, bm, BM_FACES_OF_MESH) {
        if (wanted.contains(read_id(BM_ELEM_CD_GET_VOID_P(f, face_offset)).as_u64())) {
          BM_face_select_set(bm, f, true);
          selected++;
        }
      }
    }
    if (selected == 0) {
      BM_mesh_free(bm);
      BKE_id_free(nullptr, src);
      r_reason = "the elements of an operation were not found";
      return nullptr;
    }
    if (!run_operation(bm, ops[i])) {
      BM_mesh_free(bm);
      BKE_id_free(nullptr, src);
      r_reason = "an operation failed";
      return nullptr;
    }
    BM_mesh_normals_update(bm);
  }
  BM_mesh_elem_hflag_disable_all(bm, BM_VERT | BM_EDGE | BM_FACE, BM_ELEM_SELECT, false);

  Mesh *result = BKE_mesh_from_bmesh_for_eval_nomain(bm, nullptr, src);
  BM_mesh_free(bm);
  assign_result_ids(*result, in_vert_ids, in_edge_ids, in_edges);
  BKE_id_free(nullptr, src);
  return result;
}

/** \} */

}  // namespace blender::edit_poly
