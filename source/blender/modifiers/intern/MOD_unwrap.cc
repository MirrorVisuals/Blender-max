/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 *
 * Unwrap modifier (experimental, Edit Poly stack), modelled on 3ds Max's Unwrap UVW.
 *
 * The UVs edited in a UV session (Tab) are stored per face corner by stable IDs (face ID +
 * vertex ID) and put back on the mesh that arrives, so they stay on the same faces when the
 * modifiers below change. Faces without stored UVs of their own get them from their history:
 * - a coarser face (e.g. Subdivision level lowered): from the stored faces split from it,
 *   by vertex ID;
 * - a face split from a stored face (Subdivision added or raised): interpolated in the stored
 *   face using the subdivision layout positions (`.stable_subd_uv`);
 * - a Mirror / Array copy: from its source face, by the source vertex IDs.
 * Faces with no history (new topology) are "new": unwrapped and attached to the stored UVs
 * around them, or left with the UVs that arrive. They are flagged (`.unwrap_new`), counted in
 * the modifier's message and selected when the level is edited.
 */

#include <cfloat>
#include <cmath>
#include <cstring>

#include "BLI_array.hh"
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
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_stable_id_mesh.hh"

#include "BLO_read_write.hh"

#include "GEO_uv_parametrizer.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MEM_guardedalloc.h"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"
#include "MOD_unwrap.hh"

namespace blender {

using bke::stable_id::StableID;

static constexpr float PI_F = 3.14159265358979323846f;

namespace unwrap {

/* -------------------------------------------------------------------- */
/** \name Stored data
 * \{ */

struct StoredFace {
  StableID id;
  StableID parent;
  int n = 0;
  int ints = 0;
  int floats = 0;
  bool has_layout = false;
};

struct Stored {
  Span<int> ints;
  Span<float> floats;
  Vector<StoredFace> faces;
  Map<uint64_t, int> by_id;
  Map<uint64_t, Vector<int>> by_parent;

  StableID vert(const StoredFace &f, const int k) const
  {
    return StableID{uint32_t(ints[f.ints + 2 * k]), uint32_t(ints[f.ints + 2 * k + 1])};
  }
  float2 uv(const StoredFace &f, const int k) const
  {
    return float2(floats[f.floats + 2 * k], floats[f.floats + 2 * k + 1]);
  }
  float2 layout(const StoredFace &f, const int k) const
  {
    const int i = f.floats + 2 * f.n + 2 * k;
    return float2(floats[i], floats[i + 1]);
  }
  int find_vert(const StoredFace &f, const StableID v) const
  {
    for (int k = 0; k < f.n; k++) {
      if (this->vert(f, k) == v) {
        return k;
      }
    }
    return -1;
  }
};

static StableID id_from_pair(const int a, const int b)
{
  return StableID{uint32_t(a), uint32_t(b)};
}

static bool decode(const UnwrapModifierData &umd, Stored &r)
{
  r.ints = umd.face_ints ? Span<int>(umd.face_ints, umd.face_ints_num) : Span<int>();
  r.floats = umd.face_floats ? Span<float>(umd.face_floats, umd.face_floats_num) :
                               Span<float>();
  int pi = 0, pf = 0;
  for (int i = 0; i < umd.faces_num; i++) {
    if (pi + 6 > r.ints.size()) {
      return false;
    }
    StoredFace f;
    f.id = id_from_pair(r.ints[pi], r.ints[pi + 1]);
    f.parent = id_from_pair(r.ints[pi + 2], r.ints[pi + 3]);
    f.n = r.ints[pi + 4];
    f.has_layout = r.ints[pi + 5] != 0;
    f.ints = pi + 6;
    f.floats = pf;
    if (f.n < 3) {
      return false;
    }
    pi += 6 + 2 * f.n;
    pf += 2 * f.n * (f.has_layout ? 2 : 1);
    if (pi > r.ints.size() || pf > r.floats.size()) {
      return false;
    }
    const int index = int(r.faces.append_and_get_index(f));
    r.by_id.add(f.id.as_u64(), index);
    if (f.parent.is_valid()) {
      r.by_parent.lookup_or_add_default(f.parent.as_u64()).append(index);
    }
  }
  return true;
}

void clear(UnwrapModifierData &umd)
{
  MEM_SAFE_DELETE(umd.face_ints);
  MEM_SAFE_DELETE(umd.face_floats);
  umd.face_ints_num = 0;
  umd.face_floats_num = 0;
  umd.faces_num = 0;
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

static std::string uv_map_name(const UnwrapModifierData &umd, const Mesh &mesh)
{
  if (umd.uvmap_name[0]) {
    return umd.uvmap_name;
  }
  return mesh.active_or_default_uv_map_name();
}

int session_commit(UnwrapModifierData &umd, const Mesh &mesh)
{
  using namespace bke::stable_id;
  const std::string name = uv_map_name(umd, mesh);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<float2> uvs = name.empty() ?
                                     VArraySpan<float2>() :
                                     *attributes.lookup<float2>(name, bke::AttrDomain::Corner);
  if (uvs.is_empty()) {
    return 0;
  }
  const Array<StableID> face_ids = read_or_make_root_ids(mesh, bke::AttrDomain::Face);
  const Array<StableID> vert_ids = read_or_make_root_ids(mesh, bke::AttrDomain::Point);
  const VArraySpan<int2> parents = *attributes.lookup_or_default<int2>(
      ATTR_PARENT_FACE, bke::AttrDomain::Face, int2(0, 0));
  const VArraySpan<float3> layout = *attributes.lookup<float3>(ATTR_SUBD_UV,
                                                               bke::AttrDomain::Corner);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();

  Vector<int> ints;
  Vector<float> floats;
  Set<uint64_t> seen;
  int count = 0;
  for (const int f : faces.index_range()) {
    if (!seen.add(face_ids[f].as_u64())) {
      continue; /* Duplicate IDs (topology edited in the session): keep the first. */
    }
    const IndexRange face = faces[f];
    const bool has_layout = !layout.is_empty();
    ints.extend({int(face_ids[f].lo),
                 int(face_ids[f].hi),
                 parents[f].x,
                 parents[f].y,
                 int(face.size()),
                 has_layout ? 1 : 0});
    for (const int corner : face) {
      const StableID v = vert_ids[corner_verts[corner]];
      ints.extend({int(v.lo), int(v.hi)});
      floats.extend({uvs[corner].x, uvs[corner].y});
    }
    if (has_layout) {
      for (const int corner : face) {
        floats.extend({layout[corner].x, layout[corner].y});
      }
    }
    count++;
  }
  clear(umd);
  umd.face_ints = dup_array(ints.data(), ints.size());
  umd.face_floats = dup_array(floats.data(), floats.size());
  umd.face_ints_num = umd.face_ints ? int(ints.size()) : 0;
  umd.face_floats_num = umd.face_floats ? int(floats.size()) : 0;
  umd.faces_num = umd.face_ints ? count : 0;
  return umd.faces_num;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Finding UVs for a face
 * \{ */

/** Mean value coordinates of `p` in polygon `poly` (works outside the polygon too). */
static void mean_value_weights(const Span<float2> poly, const float2 p, MutableSpan<float> r_w)
{
  const int n = int(poly.size());
  Array<float2, 16> s(n);
  Array<float, 16> r(n);
  r_w.fill(0.0f);
  for (int i = 0; i < n; i++) {
    s[i] = poly[i] - p;
    r[i] = math::length(s[i]);
    if (r[i] < 1e-8f) {
      r_w[i] = 1.0f;
      return;
    }
  }
  Array<float, 16> t(n);
  for (int i = 0; i < n; i++) {
    const int j = (i + 1) % n;
    const float area = s[i].x * s[j].y - s[i].y * s[j].x;
    const float dot = math::dot(s[i], s[j]);
    if (std::abs(area) < 1e-10f * r[i] * r[j]) {
      if (dot < 0.0f) {
        /* On the edge. */
        const float f = r[i] / (r[i] + r[j]);
        r_w[i] = 1.0f - f;
        r_w[j] = f;
        return;
      }
      t[i] = 0.0f;
      continue;
    }
    t[i] = (r[i] * r[j] - dot) / area;
  }
  float sum = 0.0f;
  for (int i = 0; i < n; i++) {
    r_w[i] = (t[(i + n - 1) % n] + t[i]) / r[i];
    sum += r_w[i];
  }
  if (std::abs(sum) < 1e-12f) {
    r_w.fill(1.0f / float(n));
    return;
  }
  for (float &w : r_w) {
    w /= sum;
  }
}

static bool inside(const Span<float2> poly, const float2 p)
{
  bool in = false;
  const int n = int(poly.size());
  for (int i = 0, j = n - 1; i < n; j = i++) {
    if ((poly[i].y > p.y) != (poly[j].y > p.y) &&
        p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)
    {
      in = !in;
    }
  }
  return in;
}

/** Layout position of each corner of a stored face that is a subdivision base face. */
static void base_face_layout(const Stored &stored, const StoredFace &f, MutableSpan<float2> r)
{
  using namespace bke::stable_id;
  Array<StableID, 16> ids(f.n);
  for (int k = 0; k < f.n; k++) {
    ids[k] = stored.vert(f, k);
  }
  const int start = canonical_start(ids.data(), f.n);
  const int dir = canonical_direction(ids.data(), f.n, start);
  for (int k = 0; k < f.n; k++) {
    const int corner = ((start + dir * k) % f.n + f.n) % f.n;
    const float angle = 2.0f * PI_F * float(k) / float(f.n);
    r[corner] = float2(std::cos(angle), std::sin(angle));
  }
}

struct MeshIDs {
  OffsetIndices<int> faces;
  Span<int> corner_verts;
  Span<StableID> face_ids;
  Span<StableID> vert_ids;
  Span<int2> face_parents;
  Span<int2> vert_parents;
  Span<float3> subd_layout;
};

/** UVs for face `f` from the stored data, or false if the face is new. */
static bool resolve_face(const Stored &stored,
                         const MeshIDs &m,
                         const int f,
                         MutableSpan<float2> r_uvs)
{
  const IndexRange face = m.faces[f];
  const int n = int(face.size());
  auto vert_id = [&](const int c) { return m.vert_ids[m.corner_verts[face[c]]]; };

  /* 1. Stored as it is. */
  if (const int *i = stored.by_id.lookup_ptr(m.face_ids[f].as_u64())) {
    const StoredFace &sf = stored.faces[*i];
    bool all = true;
    for (int c = 0; c < n && all; c++) {
      const int k = stored.find_vert(sf, vert_id(c));
      all = k != -1;
      if (all) {
        r_uvs[c] = stored.uv(sf, k);
      }
    }
    if (all) {
      return true;
    }
  }

  /* 2. A coarser face: its corners are corners of the stored faces split from it. */
  if (const Vector<int> *kids = stored.by_parent.lookup_ptr(m.face_ids[f].as_u64())) {
    bool all = true;
    for (int c = 0; c < n && all; c++) {
      all = false;
      for (const int kid : *kids) {
        const int k = stored.find_vert(stored.faces[kid], vert_id(c));
        if (k != -1) {
          r_uvs[c] = stored.uv(stored.faces[kid], k);
          all = true;
          break;
        }
      }
    }
    if (all) {
      return true;
    }
  }

  if (m.face_parents.is_empty()) {
    return false;
  }
  const StableID parent = id_from_pair(m.face_parents[f].x, m.face_parents[f].y);
  if (!parent.is_valid()) {
    return false;
  }

  /* 3. Split from a stored face (or from the face stored faces were split from): interpolate
   * in the subdivision layout. */
  if (!m.subd_layout.is_empty()) {
    struct Candidate {
      Vector<float2> layout;
      Vector<float2> uvs;
    };
    Vector<Candidate> candidates;
    if (const Vector<int> *kids = stored.by_parent.lookup_ptr(parent.as_u64())) {
      for (const int kid : *kids) {
        const StoredFace &sf = stored.faces[kid];
        if (!sf.has_layout) {
          continue;
        }
        Candidate cand;
        for (int k = 0; k < sf.n; k++) {
          cand.layout.append(stored.layout(sf, k));
          cand.uvs.append(stored.uv(sf, k));
        }
        candidates.append(std::move(cand));
      }
    }
    if (const int *i = stored.by_id.lookup_ptr(parent.as_u64())) {
      const StoredFace &sf = stored.faces[*i];
      Candidate cand;
      cand.layout.resize(sf.n);
      base_face_layout(stored, sf, cand.layout);
      for (int k = 0; k < sf.n; k++) {
        cand.uvs.append(stored.uv(sf, k));
      }
      candidates.append(std::move(cand));
    }
    if (!candidates.is_empty()) {
      Array<float2, 16> points(n);
      float2 center(0.0f);
      for (int c = 0; c < n; c++) {
        points[c] = float2(m.subd_layout[face[c]].x, m.subd_layout[face[c]].y);
        center += points[c];
      }
      center /= float(n);
      const Candidate *best = nullptr;
      float best_dist = FLT_MAX;
      for (const Candidate &cand : candidates) {
        if (inside(cand.layout, center)) {
          best = &cand;
          break;
        }
        float2 cc(0.0f);
        for (const float2 &p : cand.layout) {
          cc += p;
        }
        cc /= float(cand.layout.size());
        const float d = math::distance_squared(cc, center);
        if (d < best_dist) {
          best_dist = d;
          best = &cand;
        }
      }
      Array<float, 16> w(best->layout.size());
      for (int c = 0; c < n; c++) {
        mean_value_weights(best->layout, points[c], w);
        float2 uv(0.0f);
        for (const int k : w.index_range()) {
          uv += w[k] * best->uvs[k];
        }
        r_uvs[c] = uv;
      }
      return true;
    }
  }

  /* 4. A copy (Mirror, Array): the source face, by the source vertices. */
  if (const int *i = stored.by_id.lookup_ptr(parent.as_u64())) {
    const StoredFace &sf = stored.faces[*i];
    for (int c = 0; c < n; c++) {
      int k = stored.find_vert(sf, vert_id(c));
      if (k == -1 && !m.vert_parents.is_empty()) {
        const int2 p = m.vert_parents[m.corner_verts[face[c]]];
        k = stored.find_vert(sf, id_from_pair(p.x, p.y));
      }
      if (k == -1) {
        return false;
      }
      r_uvs[c] = stored.uv(sf, k);
    }
    return true;
  }
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name New faces
 * \{ */

static float uv_area(const Span<float2> uvs)
{
  float a = 0.0f;
  for (const int i : uvs.index_range()) {
    const float2 &p = uvs[i];
    const float2 &q = uvs[(i + 1) % uvs.size()];
    a += p.x * q.y - q.x * p.y;
  }
  return std::abs(a) * 0.5f;
}

/**
 * Unwrap the new faces. Their corners on vertices of faces with UVs are pinned to those UVs,
 * so the new faces attach to them. Islands with fewer than two such corners are scaled to the
 * density of the stored UVs and laid out to the right of the 0..1 tile.
 */
static void fill_new_faces(const Span<float3> positions,
                           const OffsetIndices<int> faces,
                           const Span<int> corner_verts,
                           const Span<bool> resolved,
                           const IndexMask &new_faces,
                           MutableSpan<float2> uvs)
{
  const int verts_num = int(positions.size());
  Array<float2> vert_uv(verts_num, float2(0.0f));
  Array<bool> pinned(verts_num, false);
  double known_uv = 0.0, known_3d = 0.0;
  for (const int f : faces.index_range()) {
    if (!resolved[f]) {
      continue;
    }
    for (const int corner : faces[f]) {
      const int v = corner_verts[corner];
      if (!pinned[v]) {
        pinned[v] = true;
        vert_uv[v] = uvs[corner];
      }
    }
    known_uv += uv_area(uvs.slice(faces[f]));
    known_3d += bke::mesh::face_area_calc(positions, corner_verts.slice(faces[f]));
  }

  geometry::ParamHandle *handle = new geometry::ParamHandle();
  new_faces.foreach_index([&](const int f) {
    const IndexRange face = faces[f];
    const int n = int(face.size());
    Array<geometry::ParamKey, 16> vkeys(n);
    Array<const float *, 16> co(n);
    Array<float *, 16> uv(n);
    Array<bool, 16> pin(n);
    Array<bool, 16> select(n);
    for (const int i : IndexRange(n)) {
      const int corner = face[i];
      const int v = corner_verts[corner];
      vkeys[i] = geometry::ParamKey(v);
      co[i] = positions[v];
      uvs[corner] = pinned[v] ? vert_uv[v] : float2(0.0f);
      uv[i] = &uvs[corner].x;
      pin[i] = pinned[v];
      select[i] = false;
    }
    geometry::uv_parametrizer_face_add(handle,
                                       geometry::ParamKey(f),
                                       n,
                                       vkeys.data(),
                                       co.data(),
                                       uv.data(),
                                       nullptr,
                                       pin.data(),
                                       select.data());
  });
  geometry::uv_parametrizer_construct_end(handle, false, false, nullptr);
  geometry::uv_parametrizer_lscm_begin(handle, false, true, false);
  geometry::uv_parametrizer_lscm_solve(handle, nullptr, nullptr);
  geometry::uv_parametrizer_lscm_end(handle);
  geometry::uv_parametrizer_flush(handle);
  delete handle;

  /* Islands of new faces (connected through shared vertices). */
  Array<int> first_face_of_vert(verts_num, -1);
  Vector<int> list;
  new_faces.foreach_index([&](const int f) { list.append(f); });
  Array<int> root(list.size());
  for (const int i : list.index_range()) {
    root[i] = i;
  }
  auto find = [&](int i) {
    while (root[i] != i) {
      root[i] = root[root[i]];
      i = root[i];
    }
    return i;
  };
  for (const int i : list.index_range()) {
    for (const int corner : faces[list[i]]) {
      const int v = corner_verts[corner];
      if (first_face_of_vert[v] == -1) {
        first_face_of_vert[v] = i;
      }
      else {
        root[find(i)] = find(first_face_of_vert[v]);
      }
    }
  }
  Map<int, Vector<int>> islands;
  for (const int i : list.index_range()) {
    islands.lookup_or_add_default(find(i)).append(list[i]);
  }

  double new_3d_total = 0.0;
  for (const int f : list) {
    new_3d_total += bke::mesh::face_area_calc(positions, corner_verts.slice(faces[f]));
  }
  const double density = (known_uv > 1e-12 && known_3d > 1e-12) ?
                             std::sqrt(known_uv / known_3d) :
                             (new_3d_total > 1e-12 ? 1.0 / std::sqrt(new_3d_total) : 1.0);

  float cursor = 1.05f;
  for (const Vector<int> &island : islands.values()) {
    Set<int> pins;
    double a3 = 0.0, auv = 0.0;
    float2 lo(FLT_MAX), hi(-FLT_MAX);
    for (const int f : island) {
      for (const int corner : faces[f]) {
        if (pinned[corner_verts[corner]]) {
          pins.add(corner_verts[corner]);
        }
        lo = math::min(lo, uvs[corner]);
        hi = math::max(hi, uvs[corner]);
      }
      a3 += bke::mesh::face_area_calc(positions, corner_verts.slice(faces[f]));
      auv += uv_area(uvs.slice(faces[f]));
    }
    if (pins.size() >= 2) {
      continue; /* Attached to existing UVs. */
    }
    const float scale = auv > 1e-12 ? float(density * std::sqrt(a3 / auv)) : 1.0f;
    for (const int f : island) {
      for (const int corner : faces[f]) {
        uvs[corner] = (uvs[corner] - lo) * scale + float2(cursor, 0.0f);
      }
    }
    cursor += (hi.x - lo.x) * scale + 0.05f;
  }
}

/** \} */

}  // namespace unwrap

/* -------------------------------------------------------------------- */
/** \name Modifier
 * \{ */

static void init_data(ModifierData *md)
{
  UnwrapModifierData *umd = reinterpret_cast<UnwrapModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(umd, modifier);
}

static void required_data_mask(ModifierData * /*md*/, CustomData_MeshMasks *r_cddata_masks)
{
  r_cddata_masks->lmask |= CD_MASK_PROP_FLOAT2;
}

static void copy_data(const ModifierData *md, ModifierData *target, const int flag)
{
  const UnwrapModifierData *umd = reinterpret_cast<const UnwrapModifierData *>(md);
  UnwrapModifierData *tumd = reinterpret_cast<UnwrapModifierData *>(target);
  BKE_modifier_copydata_generic(md, target, flag);
  tumd->face_ints = unwrap::dup_array(umd->face_ints, umd->face_ints_num);
  tumd->face_floats = unwrap::dup_array(umd->face_floats, umd->face_floats_num);
  if (!tumd->face_ints || !tumd->face_floats) {
    unwrap::clear(*tumd);
  }
}

static void free_data(ModifierData *md)
{
  unwrap::clear(*reinterpret_cast<UnwrapModifierData *>(md));
}

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  using namespace bke::stable_id;
  UnwrapModifierData *umd = reinterpret_cast<UnwrapModifierData *>(md);

  /* IDs on the result, so a UV session can store by them. */
  ensure_root_ids(*mesh);
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  attributes.remove(unwrap::ATTR_NEW_FACE);
  if (umd->faces_num == 0 || mesh->faces_num == 0) {
    return mesh;
  }

  unwrap::Stored stored;
  if (!unwrap::decode(*umd, stored)) {
    BKE_modifier_set_warning(ctx->object, md, "Stored UVs could not be read");
    return mesh;
  }

  std::string name = unwrap::uv_map_name(*umd, *mesh);
  const bool make_active = name.empty();
  if (name.empty()) {
    name = "UVMap";
  }
  bke::SpanAttributeWriter<float2> uv = attributes.lookup_or_add_for_write_span<float2>(
      name, bke::AttrDomain::Corner);
  if (!uv) {
    BKE_modifier_set_warning(
        ctx->object, md, "\"%s\" is not a UV map (face corner 2D vector)", name.c_str());
    return mesh;
  }

  const Array<StableID> face_ids = read_or_make_root_ids(*mesh, bke::AttrDomain::Face);
  const Array<StableID> vert_ids = read_or_make_root_ids(*mesh, bke::AttrDomain::Point);
  const VArraySpan<int2> face_parents = *attributes.lookup<int2>(ATTR_PARENT_FACE,
                                                                 bke::AttrDomain::Face);
  const VArraySpan<int2> vert_parents = *attributes.lookup<int2>(ATTR_PARENT_POINT,
                                                                 bke::AttrDomain::Point);
  const VArraySpan<float3> subd_layout = *attributes.lookup<float3>(ATTR_SUBD_UV,
                                                                    bke::AttrDomain::Corner);
  unwrap::MeshIDs m;
  m.faces = mesh->faces();
  m.corner_verts = mesh->corner_verts();
  m.face_ids = face_ids;
  m.vert_ids = vert_ids;
  m.face_parents = face_parents;
  m.vert_parents = vert_parents;
  m.subd_layout = subd_layout;

  Array<bool> resolved(mesh->faces_num, false);
  threading::parallel_for(m.faces.index_range(), 512, [&](const IndexRange range) {
    Array<float2, 16> face_uvs;
    for (const int f : range) {
      face_uvs.reinitialize(m.faces[f].size());
      if (unwrap::resolve_face(stored, m, f, face_uvs)) {
        uv.span.slice(m.faces[f]).copy_from(face_uvs);
        resolved[f] = true;
      }
    }
  });

  IndexMaskMemory memory;
  const IndexMask new_faces = IndexMask::from_predicate(
      m.faces.index_range(), memory, [&](const int f) { return !resolved[f]; });
  if (!new_faces.is_empty() && umd->new_faces == MOD_UNWRAP_NEW_FILL) {
    unwrap::fill_new_faces(
        mesh->vert_positions(), m.faces, m.corner_verts, resolved, new_faces, uv.span);
  }
  uv.finish();
  if (make_active && mesh->active_uv_map_name().is_empty()) {
    mesh->uv_maps_active_set(name);
    mesh->uv_maps_default_set(name);
  }

  if (!new_faces.is_empty()) {
    bke::SpanAttributeWriter<bool> flag = attributes.lookup_or_add_for_write_span<bool>(
        unwrap::ATTR_NEW_FACE, bke::AttrDomain::Face);
    new_faces.foreach_index([&](const int f) { flag.span[f] = true; });
    flag.finish();
    BKE_modifier_set_warning(ctx->object,
                             md,
                             umd->new_faces == MOD_UNWRAP_NEW_FILL ?
                                 "%d new face(s) unwrapped automatically (Tab to edit them)" :
                                 "%d new face(s) keep the UVs from below (Tab to edit them)",
                             int(new_faces.size()));
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

  ui::Layout *row = &layout.row(true);
  PointerRNA op_ptr = row->op("OBJECT_OT_unwrap_level_edit", IFACE_("Edit UVs"), ICON_UV);
  RNA_string_set(&op_ptr, "modifier", md->name);
  op_ptr = row->op("OBJECT_OT_unwrap_clear", IFACE_("Reset"), ICON_X);
  RNA_string_set(&op_ptr, "modifier", md->name);

  layout.use_property_split_set(true);
  layout.prop_search(ptr, "uv_layer", &obj_data_ptr, "uv_layers", std::nullopt, ICON_GROUP_UVS);
  layout.prop(ptr, "new_faces", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  layout.prop(ptr, "faces_num", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  modifier_error_message_draw(layout, ptr);
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Unwrap, panel_draw);
}

static void blend_write(BlendWriter *writer, const ID * /*id_owner*/, const ModifierData *md)
{
  const UnwrapModifierData *umd = reinterpret_cast<const UnwrapModifierData *>(md);
  writer->write_struct(umd);
  if (umd->face_ints_num > 0 && umd->face_ints) {
    writer->write_int32_array(umd->face_ints_num, umd->face_ints);
  }
  if (umd->face_floats_num > 0 && umd->face_floats) {
    writer->write_float_array(umd->face_floats_num, umd->face_floats);
  }
}

static void blend_read(BlendDataReader *reader, ModifierData *md)
{
  UnwrapModifierData *umd = reinterpret_cast<UnwrapModifierData *>(md);
  BLO_read_array_and_validate_size(reader, &umd->face_ints, &umd->face_ints_num);
  BLO_read_array_and_validate_size(reader, &umd->face_floats, &umd->face_floats_num);
  if (!umd->face_ints || !umd->face_floats) {
    MEM_SAFE_DELETE(umd->face_ints);
    MEM_SAFE_DELETE(umd->face_floats);
    umd->face_ints_num = 0;
    umd->face_floats_num = 0;
    umd->faces_num = 0;
  }
}

/** \} */

ModifierTypeInfo modifierType_Unwrap = {
    /*idname*/ "Unwrap",
    /*name*/ N_("Unwrap"),
    /*struct_name*/ "UnwrapModifierData",
    /*struct_size*/ sizeof(UnwrapModifierData),
    /*srna*/ &RNA_UnwrapModifier,
    /*type*/ ModifierTypeType::NonGeometrical,
    /*flags*/
    (eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_SupportsMapping |
     eModifierTypeFlag_SupportsEditmode | eModifierTypeFlag_EnableInEditmode),
    /*icon*/ ICON_UV,

    /*copy_data*/ copy_data,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ required_data_mask,
    /*free_data*/ free_data,
    /*is_disabled*/ nullptr,
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
