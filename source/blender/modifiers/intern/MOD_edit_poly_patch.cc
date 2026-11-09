/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include <cfloat>
#include <optional>

#include "BLI_array.hh"
#include "BLI_index_mask.hh"
#include "BLI_kdtree.hh"
#include "BLI_map.hh"
#include "BLI_math_vector.hh"
#include "BLI_set.hh"
#include "BLI_task.hh"

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_math.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_stable_id_mesh.hh"

#include "MOD_edit_poly.hh"
#include "MOD_edit_poly_patch.hh"

namespace blender::edit_poly {

using namespace bke::stable_id;

static constexpr int PATCH_VERSION = 1;
static constexpr int HEADER_SIZE = 6;

static uint64_t key_of(const StableID id)
{
  return id.as_u64();
}

/* -------------------------------------------------------------------- */
/** \name Encoding
 * \{ */

static void append_ids(Vector<int> &ints, const Span<StableID> ids)
{
  for (const StableID id : ids) {
    ints.append(int(id.lo));
    ints.append(int(id.hi));
  }
}

void patch_encode(const Patch &patch, Vector<int> &r_ints, Vector<float> &r_floats)
{
  r_ints.clear();
  r_floats.clear();
  if (patch.is_empty()) {
    return;
  }
  r_ints.append(PATCH_VERSION);
  r_ints.append(int(patch.removed_verts.size()));
  r_ints.append(int(patch.removed_faces.size()));
  r_ints.append(int(patch.new_vert_ids.size()));
  r_ints.append(int(patch.new_face_sizes.size()));
  r_ints.append(int(patch.new_face_corner_verts.size()));
  append_ids(r_ints, patch.removed_verts);
  append_ids(r_ints, patch.removed_faces);
  append_ids(r_ints, patch.new_vert_ids);
  append_ids(r_ints, patch.new_vert_anchors);
  r_ints.extend(patch.new_face_sizes);
  append_ids(r_ints, patch.new_face_ids);
  append_ids(r_ints, patch.new_face_sources);
  append_ids(r_ints, patch.new_face_corner_verts);
  for (const float3 &offset : patch.new_vert_offsets) {
    r_floats.append(offset.x);
    r_floats.append(offset.y);
    r_floats.append(offset.z);
  }
}

bool patch_decode(const Span<int> ints, const Span<float> floats, Patch &r_patch)
{
  r_patch = {};
  if (ints.is_empty()) {
    return true;
  }
  if (ints.size() < HEADER_SIZE || ints[0] != PATCH_VERSION) {
    return false;
  }
  const int64_t n_rv = ints[1], n_rf = ints[2], n_nv = ints[3], n_nf = ints[4], n_c = ints[5];
  if (n_rv < 0 || n_rf < 0 || n_nv < 0 || n_nf < 0 || n_c < 0) {
    return false;
  }
  const int64_t expected = HEADER_SIZE + 2 * n_rv + 2 * n_rf + 4 * n_nv + n_nf + 4 * n_nf +
                           2 * n_c;
  if (ints.size() != expected || floats.size() != 3 * n_nv) {
    return false;
  }
  int64_t pos = HEADER_SIZE;
  auto read_ids = [&](const int64_t n, Vector<StableID> &r_ids) {
    r_ids.reserve(n);
    for (int64_t i = 0; i < n; i++) {
      r_ids.append(StableID{uint32_t(ints[pos]), uint32_t(ints[pos + 1])});
      pos += 2;
    }
  };
  read_ids(n_rv, r_patch.removed_verts);
  read_ids(n_rf, r_patch.removed_faces);
  read_ids(n_nv, r_patch.new_vert_ids);
  read_ids(n_nv, r_patch.new_vert_anchors);
  int64_t corner_total = 0;
  for (int64_t i = 0; i < n_nf; i++) {
    const int size = ints[pos++];
    if (size < 3) {
      r_patch = {};
      return false;
    }
    corner_total += size;
    r_patch.new_face_sizes.append(size);
  }
  if (corner_total != n_c) {
    r_patch = {};
    return false;
  }
  read_ids(n_nf, r_patch.new_face_ids);
  read_ids(n_nf, r_patch.new_face_sources);
  read_ids(n_c, r_patch.new_face_corner_verts);
  for (int64_t i = 0; i < n_nv; i++) {
    r_patch.new_vert_offsets.append(float3(floats[3 * i], floats[3 * i + 1], floats[3 * i + 2]));
  }
  return true;
}

int patch_encoded_new_verts_num(const Span<int> ints)
{
  return ints.size() >= HEADER_SIZE ? ints[3] : 0;
}
int patch_encoded_new_faces_num(const Span<int> ints)
{
  return ints.size() >= HEADER_SIZE ? ints[4] : 0;
}
int patch_encoded_removed_faces_num(const Span<int> ints)
{
  return ints.size() >= HEADER_SIZE ? ints[2] : 0;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Applying a patch
 * \{ */

static Map<uint64_t, int> index_map(const Span<StableID> ids)
{
  Map<uint64_t, int> map;
  map.reserve(ids.size());
  for (const int i : ids.index_range()) {
    map.add(key_of(ids[i]), i);
  }
  return map;
}

static bool is_skipped_attribute(const StringRef name)
{
  return ELEM(name,
              "position",
              ".edge_verts",
              ".corner_vert",
              ".corner_edge",
              ATTR_ID_POINT,
              ATTR_ID_EDGE,
              ATTR_ID_FACE,
              ATTR_PARENT_POINT,
              ATTR_PARENT_EDGE,
              ATTR_PARENT_FACE,
              SESSION_ATTR_VERT_ID_LO,
              SESSION_ATTR_VERT_ID_HI,
              SESSION_ATTR_FACE_ID_LO,
              SESSION_ATTR_FACE_ID_HI);
}

/** Copy every generic attribute of `domain`; `map[i]` is the source index, -1 keeps the default.
 */
static void copy_attributes(const Mesh &src,
                            Mesh &dst,
                            const bke::AttrDomain domain,
                            const Span<int> map)
{
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_predicate(
      IndexRange(map.size()), memory, [&](const int64_t i) { return map[i] >= 0; });
  const bke::AttributeAccessor src_attributes = src.attributes();
  bke::MutableAttributeAccessor dst_attributes = dst.attributes_for_write();
  src_attributes.foreach_attribute([&](const bke::AttributeIter &iter) {
    if (iter.domain != domain || iter.data_type == bke::AttrType::String ||
        is_skipped_attribute(iter.name))
    {
      return;
    }
    const GVArray src_values = *iter.get();
    bke::GSpanAttributeWriter dst_values = dst_attributes.lookup_or_add_for_write_span(
        iter.name, domain, iter.data_type);
    if (!dst_values) {
      return;
    }
    bke::attribute_math::gather(src_values, map, mask, dst_values.span);
    dst_values.finish();
  });
}

/**
 * Indices in `ids` of the `wanted` IDs (-1 for missing ones). Uses a small table of the wanted
 * IDs, checked against every element in parallel: much faster than a table of all elements when
 * a patch refers to only some of them.
 */
static Map<uint64_t, int> find_indices(const Span<StableID> ids, const Span<Span<StableID>> wanted)
{
  Map<uint64_t, int> found;
  for (const Span<StableID> list : wanted) {
    for (const StableID id : list) {
      found.add(key_of(id), -1);
    }
  }
  if (found.is_empty()) {
    return found;
  }
  threading::parallel_for(ids.index_range(), 8192, [&](const IndexRange range) {
    for (const int i : range) {
      if (int *index = found.lookup_ptr(key_of(ids[i]))) {
        *index = i; /* IDs are unique: each slot is written by one element. */
      }
    }
  });
  return found;
}

Mesh *patch_apply(const Mesh &input, const Patch &patch, std::string &r_reason)
{
  const Array<StableID> in_vert_ids = read_or_make_root_ids(input, bke::AttrDomain::Point);
  const Array<StableID> in_edge_ids = read_or_make_root_ids(input, bke::AttrDomain::Edge);
  const Array<StableID> in_face_ids = read_or_make_root_ids(input, bke::AttrDomain::Face);
  const Map<uint64_t, int> in_vert_of = find_indices(
      in_vert_ids, {patch.removed_verts.as_span(), patch.new_vert_anchors.as_span()});
  const Map<uint64_t, int> in_face_of = find_indices(
      in_face_ids, {patch.removed_faces.as_span(), patch.new_face_sources.as_span()});
  const Span<float3> in_positions = input.vert_positions();
  const OffsetIndices<int> in_faces = input.faces();
  const Span<int> in_corner_verts = input.corner_verts();
  const Span<int2> in_edges = input.edges();

  auto missing = [&](const char *what) {
    r_reason = std::string("the mesh below no longer has the ") + what + " the patch refers to";
    return nullptr;
  };

  /* Removed elements. */
  Array<bool> vert_removed(input.verts_num, false);
  for (const StableID id : patch.removed_verts) {
    const int i = in_vert_of.lookup_default(key_of(id), -1);
    if (i == -1) {
      return missing("vertices");
    }
    vert_removed[i] = true;
  }
  Array<bool> face_removed(input.faces_num, false);
  for (const StableID id : patch.removed_faces) {
    const int i = in_face_of.lookup_default(key_of(id), -1);
    if (i == -1) {
      return missing("faces");
    }
    face_removed[i] = true;
  }

  /* Result vertices: kept input vertices in order, then new vertices. */
  Vector<int> vert_src;          /* Input vertex to copy attributes from. */
  Vector<StableID> vert_ids;     /* Result vertex IDs. */
  Vector<StableID> vert_parents; /* Anchor for new vertices. */
  Vector<float3> positions;
  Array<int> result_of_input_vert(input.verts_num, -1);
  /* Result index of the vertices that new faces use (only those are looked up). */
  Map<uint64_t, int> result_vert_of;
  for (const StableID id : patch.new_face_corner_verts) {
    result_vert_of.add(key_of(id), -1);
  }
  Set<uint64_t> new_vert_keys;
  for (const StableID id : patch.new_vert_ids) {
    if (!new_vert_keys.add(key_of(id))) {
      r_reason = "the patch has a vertex ID that is already used";
      return nullptr;
    }
  }
  vert_src.reserve(input.verts_num + patch.new_vert_ids.size());
  vert_ids.reserve(vert_src.capacity());
  vert_parents.reserve(vert_src.capacity());
  positions.reserve(vert_src.capacity());
  for (const int i : IndexRange(input.verts_num)) {
    if (vert_removed[i]) {
      continue;
    }
    const uint64_t key = key_of(in_vert_ids[i]);
    if (!new_vert_keys.is_empty() && new_vert_keys.contains(key)) {
      r_reason = "the patch has a vertex ID that is already used";
      return nullptr;
    }
    if (int *index = result_vert_of.lookup_ptr(key)) {
      *index = int(vert_src.size());
    }
    result_of_input_vert[i] = int(vert_src.size());
    vert_src.append(i);
    vert_ids.append(in_vert_ids[i]);
    vert_parents.append(StableID{});
    positions.append(in_positions[i]);
  }
  const int kept_verts_num = int(vert_src.size());
  for (const int i : patch.new_vert_ids.index_range()) {
    const int anchor = in_vert_of.lookup_default(key_of(patch.new_vert_anchors[i]), -1);
    if (anchor == -1) {
      return missing("anchor vertices");
    }
    if (int *index = result_vert_of.lookup_ptr(key_of(patch.new_vert_ids[i]))) {
      *index = int(vert_src.size());
    }
    vert_src.append(anchor);
    vert_ids.append(patch.new_vert_ids[i]);
    vert_parents.append(patch.new_vert_anchors[i]);
    positions.append(in_positions[anchor] + patch.new_vert_offsets[i]);
  }

  /* Result faces: kept input faces in order, then new faces. */
  Vector<int> face_offsets;
  Vector<int> corner_verts;
  Vector<int> face_src;   /* Input face to copy attributes from, or -1. */
  Vector<int> corner_src; /* Input corner to copy attributes from, or -1. */
  Vector<StableID> face_ids;
  Vector<StableID> face_parents;
  for (const int f : in_faces.index_range()) {
    if (face_removed[f]) {
      continue;
    }
    face_offsets.append(int(corner_verts.size()));
    for (const int corner : in_faces[f]) {
      const int v = result_of_input_vert[in_corner_verts[corner]];
      if (v == -1) {
        r_reason = "the patch removes a vertex that a kept face uses";
        return nullptr;
      }
      corner_verts.append(v);
      corner_src.append(corner);
    }
    face_src.append(f);
    face_ids.append(in_face_ids[f]);
    face_parents.append(StableID{});
  }
  /* New face IDs must not be used by kept faces or by each other. */
  Set<uint64_t> used_face_ids;
  for (const StableID id : patch.new_face_ids) {
    if (!used_face_ids.add(key_of(id))) {
      r_reason = "the patch has a face ID that is already used";
      return nullptr;
    }
  }
  if (!used_face_ids.is_empty()) {
    for (const StableID id : face_ids) {
      if (used_face_ids.contains(key_of(id))) {
        r_reason = "the patch has a face ID that is already used";
        return nullptr;
      }
    }
  }
  int corner_pos = 0;
  for (const int nf : patch.new_face_sizes.index_range()) {
    const int size = patch.new_face_sizes[nf];
    const int src_face = in_face_of.lookup_default(key_of(patch.new_face_sources[nf]), -1);
    face_offsets.append(int(corner_verts.size()));
    for (int c = 0; c < size; c++) {
      const StableID vid = patch.new_face_corner_verts[corner_pos + c];
      const int v = result_vert_of.lookup_default(key_of(vid), -1);
      if (v == -1) {
        return missing("vertices");
      }
      /* A face that uses a point twice would be invalid (only possible when the IDs of the mesh
       * below are not unique). */
      const int face_start = face_offsets.last();
      for (int prev = face_start; prev < int(corner_verts.size()); prev++) {
        if (corner_verts[prev] == v) {
          r_reason = "a new face would use the same point twice";
          return nullptr;
        }
      }
      corner_verts.append(v);
      /* Corner attributes (e.g. UVs) from the source face's corner at the same input vertex,
       * else from its first corner. */
      int src_corner = -1;
      if (src_face != -1) {
        src_corner = in_faces[src_face].first();
        const int in_vert = vert_src[v];
        for (const int corner : in_faces[src_face]) {
          if (in_corner_verts[corner] == in_vert) {
            src_corner = corner;
            break;
          }
        }
      }
      corner_src.append(src_corner);
    }
    corner_pos += size;
    face_src.append(src_face);
    face_ids.append(patch.new_face_ids[nf]);
    face_parents.append(patch.new_face_sources[nf]);
  }
  face_offsets.append(int(corner_verts.size()));

  /* Loose input edges whose vertices are kept (face edges are rebuilt from the faces). */
  Vector<int2> loose_edges;
  Vector<int> loose_edge_src;
  input.loose_edges().foreach_index([&](const int e) {
    const int v0 = result_of_input_vert[in_edges[e][0]];
    const int v1 = result_of_input_vert[in_edges[e][1]];
    if (v0 != -1 && v1 != -1) {
      loose_edges.append(int2(v0, v1));
      loose_edge_src.append(e);
    }
  });

  Mesh *result = BKE_mesh_new_nomain(int(positions.size()),
                                     int(loose_edges.size()),
                                     int(face_src.size()),
                                     int(corner_verts.size()));
  BKE_mesh_copy_parameters_for_eval(result, &input);
  result->vert_positions_for_write().copy_from(positions);
  result->edges_for_write().copy_from(loose_edges);
  result->face_offsets_for_write().copy_from(face_offsets);
  result->corner_verts_for_write().copy_from(corner_verts);
  bke::mesh_calc_edges(*result, true, false);

  /* Edge sources: the input edge between the same two input vertices, found through the edges
   * of each input vertex. */
  Array<int> vert_to_edge_offsets;
  Array<int> vert_to_edge_indices;
  const GroupedSpan<int> in_vert_edges = bke::mesh::build_vert_to_edge_map(
      in_edges, input.verts_num, vert_to_edge_offsets, vert_to_edge_indices);
  const Span<int2> edges = result->edges();
  Array<int> edge_src(edges.size(), -1);
  Array<StableID> edge_ids(edges.size());
  threading::parallel_for(edges.index_range(), 4096, [&](const IndexRange range) {
    for (const int e : range) {
      const int r0 = edges[e][0], r1 = edges[e][1];
      if (r0 < kept_verts_num && r1 < kept_verts_num) {
        const int a = vert_src[r0], b = vert_src[r1];
        for (const int in_edge : in_vert_edges[a]) {
          const int2 ie = in_edges[in_edge];
          if ((ie[0] == a && ie[1] == b) || (ie[0] == b && ie[1] == a)) {
            edge_src[e] = in_edge;
            break;
          }
        }
      }
      edge_ids[e] = edge_src[e] != -1 ?
                        in_edge_ids[edge_src[e]] :
                        derive_unordered(Derivation::Generated, vert_ids[r0], vert_ids[r1], 3);
    }
  });

  copy_attributes(input, *result, bke::AttrDomain::Point, vert_src);
  copy_attributes(input, *result, bke::AttrDomain::Edge, edge_src);
  copy_attributes(input, *result, bke::AttrDomain::Face, face_src);
  copy_attributes(input, *result, bke::AttrDomain::Corner, corner_src);

  write_ids(*result, bke::AttrDomain::Point, vert_ids, vert_parents);
  write_ids(*result, bke::AttrDomain::Edge, edge_ids, {});
  write_ids(*result, bke::AttrDomain::Face, face_ids, face_parents);
  return result;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Computing a patch
 * \{ */

bool read_session_ids(const Mesh &mesh, const bke::AttrDomain domain, Array<StableID> &r_ids)
{
  const bool point = domain == bke::AttrDomain::Point;
  const bke::AttributeAccessor attributes = mesh.attributes();
  const bke::AttributeReader<int> lo = attributes.lookup<int>(point ? SESSION_ATTR_VERT_ID_LO :
                                                                      SESSION_ATTR_FACE_ID_LO);
  const bke::AttributeReader<int> hi = attributes.lookup<int>(point ? SESSION_ATTR_VERT_ID_HI :
                                                                      SESSION_ATTR_FACE_ID_HI);
  if (!lo || !hi || lo.domain != domain || hi.domain != domain) {
    r_ids = {};
    return false;
  }
  const VArraySpan<int> lo_span = *lo;
  const VArraySpan<int> hi_span = *hi;
  r_ids.reinitialize(lo_span.size());
  for (const int i : r_ids.index_range()) {
    r_ids[i] = StableID{uint32_t(lo_span[i]), uint32_t(hi_span[i])};
  }
  return true;
}

/**
 * Input index of an ID: tries the index the element is expected at first, and builds a table of
 * all IDs only the first time that guess is wrong.
 */
class HintedIndex {
 public:
  explicit HintedIndex(const Span<StableID> ids) : ids_(ids) {}

  int find(const StableID id, const int hint)
  {
    if (hint >= 0 && hint < ids_.size() && ids_[hint] == id) {
      return hint;
    }
    if (!map_) {
      map_.emplace(index_map(ids_));
    }
    return map_->lookup_default(key_of(id), -1);
  }

 private:
  Span<StableID> ids_;
  std::optional<Map<uint64_t, int>> map_;
};

std::string patch_compute(const Mesh &input,
                          const Mesh &edited,
                          const Patch &old_patch,
                          Patch &r_patch,
                          Vector<KeptMove> &r_moves,
                          Vector<StableID> &r_input_vert_ids,
                          Array<StableID> &r_edited_face_ids,
                          Array<StableID> &r_edited_vert_ids)
{
  r_patch = {};
  r_moves.clear();

  Array<StableID> session_vert_ids;
  Array<StableID> session_face_ids;
  if (!read_session_ids(edited, bke::AttrDomain::Point, session_vert_ids)) {
    return "This mesh is not an Edit Poly session";
  }
  read_session_ids(edited, bke::AttrDomain::Face, session_face_ids);

  const Array<StableID> in_vert_ids = read_or_make_root_ids(input, bke::AttrDomain::Point);
  const Array<StableID> in_face_ids = read_or_make_root_ids(input, bke::AttrDomain::Face);
  r_input_vert_ids = Vector<StableID>(in_vert_ids.as_span());
  /* Session elements usually sit at the same index as in the input (no topology change, or
   * before the first change): check that first, and only build a table of all input IDs when an
   * element is not found there. */
  HintedIndex in_vert_of(in_vert_ids);
  HintedIndex in_face_of(in_face_ids);
  const Span<float3> in_positions = input.vert_positions();
  const OffsetIndices<int> in_faces = input.faces();
  const Span<int> in_corner_verts = input.corner_verts();

  /* Earlier patch: anchors of its new vertices and sources of its new faces. */
  Map<uint64_t, StableID> old_anchor_of;
  for (const int i : old_patch.new_vert_ids.index_range()) {
    old_anchor_of.add(key_of(old_patch.new_vert_ids[i]), old_patch.new_vert_anchors[i]);
  }
  Map<uint64_t, StableID> old_source_of;
  for (const int i : old_patch.new_face_ids.index_range()) {
    old_source_of.add(key_of(old_patch.new_face_ids[i]), old_patch.new_face_sources[i]);
  }

  const Span<float3> positions = edited.vert_positions();
  const int verts_num = edited.verts_num;

  /* Nearest input vertex, for new vertices that carry no usable ID (e.g. merges). */
  KDTree<float3> *tree = nullptr;
  auto nearest_input_vert = [&](const float3 &co) {
    if (tree == nullptr) {
      tree = kdtree_new<float3>(uint(input.verts_num));
      for (const int i : in_positions.index_range()) {
        kdtree_insert<float3>(tree, i, in_positions[i]);
      }
      kdtree_balance<float3>(tree);
    }
    KDTreeNearest<float3> nearest;
    return kdtree_find_nearest<float3>(tree, co, &nearest);
  };

  /* Classify vertices. The first vertex (lowest index) with an input ID keeps it; copies made
   * by tools such as Extrude become new vertices anchored to that input vertex. */
  enum class Kind : int8_t { Kept, New, NewNeedsId };
  Array<Kind> kind(verts_num);
  Array<StableID> result_ids(verts_num);
  Array<StableID> anchors(verts_num);
  Array<int> input_of(verts_num, -1); /* Input vertex of each kept vertex. */
  Array<bool> input_taken(input.verts_num, false);
  Set<uint64_t> used_new; /* IDs given to new vertices. */
  for (const int i : IndexRange(verts_num)) {
    const StableID id = session_vert_ids[i];
    const uint64_t key = key_of(id);
    const int in = id.is_valid() ? in_vert_of.find(id, i) : -1;
    if (in != -1 && !input_taken[in]) {
      input_taken[in] = true;
      input_of[i] = in;
      kind[i] = Kind::Kept;
      result_ids[i] = id;
      continue;
    }
    if (const StableID *old_anchor = old_anchor_of.lookup_ptr(key)) {
      if (in_vert_of.find(*old_anchor, -1) != -1) {
        anchors[i] = *old_anchor;
        if (used_new.add(key)) {
          kind[i] = Kind::New;
          result_ids[i] = id;
          continue;
        }
        kind[i] = Kind::NewNeedsId;
        continue;
      }
    }
    kind[i] = Kind::NewNeedsId;
    if (in != -1) {
      anchors[i] = id;
    }
    else {
      const int nearest = nearest_input_vert(positions[i]);
      if (nearest == -1) {
        if (tree) {
          kdtree_free(tree);
        }
        return "Could not anchor new vertices: the mesh below has no vertices";
      }
      anchors[i] = in_vert_ids[nearest];
    }
  }
  if (tree) {
    kdtree_free(tree);
  }
  uint32_t counter = 0;
  for (const int i : IndexRange(verts_num)) {
    if (kind[i] != Kind::NewNeedsId) {
      continue;
    }
    StableID id;
    do {
      id = derive(Derivation::Generated, anchors[i], 0xED17, ++counter);
    } while (in_vert_of.find(id, -1) != -1 || !used_new.add(key_of(id)));
    result_ids[i] = id;
    kind[i] = Kind::New;
  }

  /* Vertices: moves for kept ones, anchored offsets for new ones, removals. */
  r_moves.reserve(verts_num);
  for (const int i : IndexRange(verts_num)) {
    if (kind[i] == Kind::Kept) {
      r_moves.append({result_ids[i], positions[i] - in_positions[input_of[i]]});
    }
    else {
      const int anchor = in_vert_of.find(anchors[i], -1);
      r_patch.new_vert_ids.append(result_ids[i]);
      r_patch.new_vert_anchors.append(anchors[i]);
      r_patch.new_vert_offsets.append(positions[i] - in_positions[anchor]);
    }
  }
  for (const int j : in_vert_ids.index_range()) {
    if (!input_taken[j]) {
      r_patch.removed_verts.append(in_vert_ids[j]);
    }
  }

  /* Faces: kept when the face keeps its input ID and the same ring of kept vertices. */
  const OffsetIndices<int> faces = edited.faces();
  const Span<int> corner_verts = edited.corner_verts();
  Array<bool> face_taken(input.faces_num, false);
  Vector<int> new_faces;
  Array<StableID> face_keys(faces.size());
  r_edited_face_ids.reinitialize(faces.size());
  r_edited_face_ids.fill(StableID{});
  for (const int f : faces.index_range()) {
    const StableID fid = session_face_ids.is_empty() ? StableID{} : session_face_ids[f];
    face_keys[f] = fid;
    const int in_face = fid.is_valid() ? in_face_of.find(fid, f) : -1;
    bool kept = false;
    if (in_face != -1 && !face_taken[in_face] && in_faces[in_face].size() == faces[f].size()) {
      const IndexRange ring = in_faces[in_face];
      const IndexRange edited_ring = faces[f];
      const int size = int(ring.size());
      kept = true;
      for (const int c : IndexRange(size)) {
        const int v = corner_verts[edited_ring[c]];
        if (kind[v] != Kind::Kept) {
          kept = false;
          break;
        }
      }
      if (kept) {
        /* Same cyclic order. */
        const StableID first = result_ids[corner_verts[edited_ring[0]]];
        int start = -1;
        for (const int c : IndexRange(size)) {
          if (in_vert_ids[in_corner_verts[ring[c]]] == first) {
            start = c;
            break;
          }
        }
        kept = start != -1;
        for (int c = 0; kept && c < size; c++) {
          const StableID a = result_ids[corner_verts[edited_ring[c]]];
          const StableID b = in_vert_ids[in_corner_verts[ring[(start + c) % size]]];
          kept = a == b;
        }
      }
    }
    if (kept) {
      face_taken[in_face] = true;
      r_edited_face_ids[f] = fid;
    }
    else {
      new_faces.append(f);
    }
  }
  for (const int j : in_face_ids.index_range()) {
    if (!face_taken[j]) {
      r_patch.removed_faces.append(in_face_ids[j]);
    }
  }
  /* IDs given to new faces; an input face ID is taken when its face is kept or reused. */
  Set<uint64_t> taken_new;
  auto take = [&](const StableID id) {
    const int j = in_face_of.find(id, -1);
    if (j != -1) {
      if (face_taken[j]) {
        return false;
      }
      face_taken[j] = true;
      return true;
    }
    return taken_new.add(key_of(id));
  };
  for (const int f : new_faces) {
    const StableID fid = face_keys[f];
    const uint64_t key = key_of(fid);
    const bool is_input_face = fid.is_valid() && in_face_of.find(fid, -1) != -1;
    StableID source{};
    if (is_input_face) {
      source = fid;
    }
    else if (const StableID *old_source = old_source_of.lookup_ptr(key)) {
      source = *old_source;
    }
    StableID id;
    const bool reusable = fid.is_valid() && (is_input_face || old_source_of.contains(key));
    if (reusable && take(fid)) {
      /* The first replacement of a face keeps its ID, so later levels still find it. */
      id = fid;
    }
    else {
      do {
        const StableID base = source.is_valid() ? source :
                                                  result_ids[corner_verts[faces[f].first()]];
        id = derive(Derivation::Generated, base, 0xFACE, ++counter);
      } while (in_face_of.find(id, -1) != -1 || !taken_new.add(key_of(id)));
    }
    r_patch.new_face_sizes.append(int(faces[f].size()));
    r_patch.new_face_ids.append(id);
    r_patch.new_face_sources.append(source);
    r_edited_face_ids[f] = id;
    for (const int corner : faces[f]) {
      r_patch.new_face_corner_verts.append(result_ids[corner_verts[corner]]);
    }
  }
  r_edited_vert_ids = result_ids;
  return "";
}

/** \} */

}  // namespace blender::edit_poly
