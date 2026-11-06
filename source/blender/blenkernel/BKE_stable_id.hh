/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Stable element IDs for the Edit Poly modifier stack (phase 1).
 *
 * A stable ID names a mesh element (vertex, edge or face) independent of its index. Every
 * geometry-creating modifier must follow the ID rule below, so an operation stored against an ID
 * can be found again after the modifiers below it change or are reordered.
 *
 * The ID rule:
 *  1. An element that survives a modifier keeps its ID.
 *  2. A new element gets an ID derived from the IDs of the elements it was made from, a tag that
 *     names the kind of derivation, and a few small integers (e.g. a subdivision fraction).
 *     The result must not depend on element indices, thread count or iteration order.
 *  3. Derived elements record their primary parent ID, so an operation stored on a face can find
 *     the faces it was split into (see #ParentIndex). Surviving elements have no parent.
 *
 * IDs are stored as a pair of 32 bit integers (#blender::bke::AttrType::Int32_2D), because there is
 * no 64 bit attribute type. The value {0, 0} means "no ID".
 *
 * This header only depends on the standard library, so the rule can be unit tested on its own.
 */

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace blender::bke::stable_id {

/**
 * Attribute names, one per domain because attribute names are unique within a mesh. The leading
 * dot hides them from the UI; the spreadsheet shows them with "Show Internal Attributes".
 * The ID of an element's parent is in the matching `.stable_parent_*` attribute.
 */
constexpr const char *ATTR_ID_POINT = ".stable_id_point";
constexpr const char *ATTR_ID_EDGE = ".stable_id_edge";
constexpr const char *ATTR_ID_FACE = ".stable_id_face";
constexpr const char *ATTR_PARENT_POINT = ".stable_parent_point";
constexpr const char *ATTR_PARENT_EDGE = ".stable_parent_edge";
constexpr const char *ATTR_PARENT_FACE = ".stable_parent_face";
/**
 * Face corner attribute written by subdivision: the position of the corner's vertex inside the
 * base face it came from, in a 2D layout of that face that does not depend on its stored corner
 * order or winding (vertex k of the canonical order at angle 2*pi*k/n on the unit circle, the
 * center at the origin). Used to find which faces of a finer or coarser subdivision cover a face
 * of another level ("footprints").
 */
constexpr const char *ATTR_SUBD_UV = ".stable_subd_uv";

struct StableID {
  uint32_t lo = 0;
  uint32_t hi = 0;

  constexpr bool is_valid() const
  {
    return (lo | hi) != 0;
  }
  constexpr uint64_t as_u64() const
  {
    return (uint64_t(hi) << 32) | uint64_t(lo);
  }
  static constexpr StableID from_u64(const uint64_t v)
  {
    return {uint32_t(v & 0xffffffffu), uint32_t(v >> 32)};
  }
  friend constexpr bool operator==(const StableID &a, const StableID &b)
  {
    return a.lo == b.lo && a.hi == b.hi;
  }
  friend constexpr bool operator!=(const StableID &a, const StableID &b)
  {
    return !(a == b);
  }
  friend constexpr bool operator<(const StableID &a, const StableID &b)
  {
    return a.as_u64() < b.as_u64();
  }
};

/**
 * What kind of derivation created an element. Values are saved in files through the hash, so
 * never renumber or reuse them; only append.
 */
enum class Derivation : uint32_t {
  /** Element of a mesh that has no history (primitive, import, collapse). */
  Root = 1,
  /** Subdivision: point on an edge, at a fraction of its length. */
  SubdEdgePoint = 2,
  /** Subdivision: point inside a face, at a fraction in each direction. */
  SubdFacePoint = 3,
  /** Subdivision: the centre of an n-gon. */
  SubdFaceCenter = 4,
  /** Subdivision: an edge that is a piece of a parent edge. */
  SubdEdgePiece = 5,
  /** Subdivision: an edge inside a parent face. */
  SubdInnerEdge = 6,
  /** Subdivision: a face that is a piece of a parent face. */
  SubdFacePiece = 7,
  /** Mirror: the reflected copy of an element. */
  MirrorCopy = 8,
  /** Array: copy number `a` of an element. */
  ArrayCopy = 9,
  /** Bevel / extrude / solidify and other tools: `a` selects the role of the new element. */
  Generated = 10,
};

namespace detail {

/** SplitMix64 finaliser: a cheap, well mixed 64 bit hash. */
constexpr uint64_t mix(uint64_t x)
{
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return x ^ (x >> 31);
}

constexpr uint64_t combine(const uint64_t seed, const uint64_t value)
{
  return mix(seed ^ (mix(value) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2)));
}

constexpr StableID finish(const uint64_t h)
{
  /* Never return the invalid ID {0, 0}. */
  return StableID::from_u64(h == 0 ? 1 : h);
}

}  // namespace detail

/** ID of element `index` of a mesh with no history. `seed` separates unrelated meshes. */
constexpr StableID make_root(const uint64_t seed, const int64_t index)
{
  uint64_t h = detail::combine(uint64_t(Derivation::Root), seed);
  h = detail::combine(h, uint64_t(index));
  return detail::finish(h);
}

/** ID of an element derived from one parent. `a` and `b` are small tag parameters. */
constexpr StableID derive(const Derivation kind,
                          const StableID parent,
                          const uint64_t a = 0,
                          const uint64_t b = 0)
{
  uint64_t h = detail::combine(uint64_t(kind), parent.as_u64());
  h = detail::combine(h, a);
  h = detail::combine(h, b);
  return detail::finish(h);
}

/**
 * ID of an element derived from two parents where the order has no meaning, such as an edge
 * between two vertices. The result is the same for (p, q) and (q, p).
 */
constexpr StableID derive_unordered(const Derivation kind,
                                    const StableID p,
                                    const StableID q,
                                    const uint64_t a = 0,
                                    const uint64_t b = 0)
{
  const uint64_t lo = p < q ? p.as_u64() : q.as_u64();
  const uint64_t hi = p < q ? q.as_u64() : p.as_u64();
  uint64_t h = detail::combine(uint64_t(kind), lo);
  h = detail::combine(h, hi);
  h = detail::combine(h, a);
  h = detail::combine(h, b);
  return detail::finish(h);
}

/**
 * A fraction in lowest terms, used for positions inside a parent element. Using fractions makes
 * the midpoint of an edge the same element at every subdivision level: 1/2 at level 1, and 2/4
 * (which reduces to 1/2) at level 2.
 */
struct Fraction {
  uint32_t num = 0;
  uint32_t den = 1;

  static constexpr uint32_t gcd(uint32_t a, uint32_t b)
  {
    while (b != 0) {
      const uint32_t t = a % b;
      a = b;
      b = t;
    }
    return a;
  }

  static constexpr Fraction make(const uint32_t num, const uint32_t den)
  {
    const uint32_t g = gcd(num, den);
    return g == 0 ? Fraction{0, 1} : Fraction{num / g, den / g};
  }

  constexpr uint64_t packed() const
  {
    return (uint64_t(num) << 32) | uint64_t(den);
  }
};

/**
 * Subdivision: the point `step` out of `steps` along an edge.
 *
 * `from` and `to` are the edge's end vertices in any order. The fraction is measured from the end
 * with the smaller ID, so the same edge always gives the same result regardless of its stored
 * direction. Example: the midpoint of a level 1 subdivision is (step 1, steps 2); at level 2 the
 * same midpoint is (step 2, steps 4), which gives the same ID.
 */
constexpr StableID subd_edge_point(const StableID from,
                                   const StableID to,
                                   const uint32_t step,
                                   const uint32_t steps)
{
  const bool flipped = to < from;
  const StableID first = flipped ? to : from;
  const StableID second = flipped ? from : to;
  const uint32_t s = flipped ? steps - step : step;
  const Fraction t = Fraction::make(s, steps);
  return derive_unordered(Derivation::SubdEdgePoint, first, second, t.packed());
}

/**
 * Subdivision: the centre of a face that is not a quad. A quad's centre is
 * `subd_face_point(face, 1/2, 1/2)` instead, so that it matches the quad's other inner points at
 * every level. Do not use this for quads, or the same point would get two different IDs.
 */
constexpr StableID subd_face_center(const StableID face)
{
  return derive(Derivation::SubdFaceCenter, face);
}

/**
 * Subdivision: a point inside a quad face at (u, v), in a frame that does not depend on how the
 * face is stored:
 *  - The origin is the corner with the smallest vertex ID (#canonical_start).
 *  - `u` runs towards whichever neighbour of that corner has the smaller vertex ID, and `v`
 *    towards the other one (#canonical_direction). This makes the result independent of both the
 *    corner the face's loop starts at and the face's winding, so flipping normals keeps the IDs.
 */
constexpr StableID subd_face_point(const StableID face, const Fraction u, const Fraction v)
{
  return derive(Derivation::SubdFacePoint, face, u.packed(), v.packed());
}

/**
 * Subdivision of a face that is not a quad: a point on the line from the midpoint of one of the
 * face's edges (`edge_a`, `edge_b` are its vertices, in any order) to the face centre, at
 * fraction `t` from the midpoint. Such a point lies between two corner patches, so it is named
 * by the edge rather than by either corner.
 */
constexpr StableID subd_ngon_spoke_point(const StableID face,
                                         const StableID edge_a,
                                         const StableID edge_b,
                                         const Fraction t)
{
  const StableID spoke = derive_unordered(
      Derivation::SubdInnerEdge, edge_a, edge_b, face.as_u64());
  return derive(Derivation::SubdFacePoint, spoke, t.packed(), 1);
}

/**
 * Subdivision of a face that is not a quad: a point inside the patch of one corner.
 * `u` runs from the corner towards the midpoint of the edge to the neighbour with the smaller
 * vertex ID, `v` towards the other neighbour's edge midpoint, so the result does not depend on
 * the face's winding.
 */
constexpr StableID subd_ngon_patch_point(const StableID face,
                                         const StableID corner_vertex,
                                         const Fraction u,
                                         const Fraction v)
{
  const StableID patch = derive(Derivation::SubdFacePiece, face, corner_vertex.as_u64(), 2);
  return derive(Derivation::SubdFacePoint, patch, u.packed(), v.packed());
}

/**
 * ID of an element defined by a set of vertices, such as a face made by subdivision. The order of
 * `ids` does not matter; the array is sorted in place.
 */
inline StableID derive_from_set(const Derivation kind, StableID *ids, const int count)
{
  std::sort(ids, ids + count);
  uint64_t h = detail::combine(uint64_t(kind), uint64_t(count));
  for (int i = 0; i < count; i++) {
    h = detail::combine(h, ids[i].as_u64());
  }
  return detail::finish(h);
}

/**
 * Index of the corner to start counting from: the one whose vertex has the smallest ID.
 * Every vertex of a face has a different ID, so this is unique and independent of the face's
 * stored corner order.
 */
inline int canonical_start(const StableID *corner_vertex_ids, const int corner_count)
{
  int best = 0;
  for (int i = 1; i < corner_count; i++) {
    if (corner_vertex_ids[i] < corner_vertex_ids[best]) {
      best = i;
    }
  }
  return best;
}

/**
 * Direction to walk around a face from `start` (+1 = stored order, -1 = reverse): towards the
 * neighbouring corner whose vertex has the smaller ID. Together with #canonical_start this gives a
 * frame that is the same for a face and its flipped copy.
 */
inline int canonical_direction(const StableID *corner_vertex_ids,
                               const int corner_count,
                               const int start)
{
  const StableID next = corner_vertex_ids[(start + 1) % corner_count];
  const StableID prev = corner_vertex_ids[(start + corner_count - 1) % corner_count];
  return next < prev ? 1 : -1;
}

/**
 * Convert a point on an `n` x `n` grid over a quad into the quad's canonical frame
 * (#canonical_start, #canonical_direction). The input has (0, 0) at stored corner 0, `a` running
 * towards corner 1 and `b` towards corner 3, which is how subdivision reports quad points.
 * Returns the numerators of the canonical (u, v), both over `n`.
 */
inline void quad_to_canonical(
    const StableID corner_vertex_ids[4], const int a, const int b, const int n, int &r_u, int &r_v)
{
  const int pos[4][2] = {{0, 0}, {n, 0}, {n, n}, {0, n}};
  const int s = canonical_start(corner_vertex_ids, 4);
  const int d = canonical_direction(corner_vertex_ids, 4, s);
  const int *o = pos[s];
  const int *pu = pos[(s + d + 4) % 4];
  const int *pv = pos[(s - d + 4) % 4];
  const int ax = pu[0] - o[0], ay = pu[1] - o[1];
  const int bx = pv[0] - o[0], by = pv[1] - o[1];
  /* The axes are +-n along x or y, so the division is exact. */
  r_u = ((a - o[0]) * ax + (b - o[1]) * ay) / n;
  r_v = ((a - o[0]) * bx + (b - o[1]) * by) / n;
}

/**
 * Make IDs unique inside one domain. Hash collisions are astronomically rare with 64 bits, but
 * two elements with the same ID would silently merge their edits. Duplicates are re-hashed in
 * index order; this is deterministic as long as the element order is, which holds for every
 * modifier that does not depend on thread count.
 *
 * Returns the number of IDs that were changed.
 */
inline int resolve_collisions(StableID *ids, const int64_t count)
{
  std::unordered_map<uint64_t, int> seen;
  seen.reserve(size_t(count));
  int changed = 0;
  for (int64_t i = 0; i < count; i++) {
    /* Elements without an ID stay without one; they are not duplicates of each other. */
    if (!ids[i].is_valid()) {
      continue;
    }
    StableID id = ids[i];
    uint32_t attempt = 0;
    while (!seen.emplace(id.as_u64(), 1).second) {
      id = derive(Derivation::Generated, ids[i], 0xC011, ++attempt);
    }
    if (attempt > 0) {
      ids[i] = id;
      changed++;
    }
  }
  return changed;
}

/** Find the current index of an element from its stable ID. */
class IDIndex {
 public:
  IDIndex() = default;

  IDIndex(const StableID *ids, const int64_t count)
  {
    map_.reserve(size_t(count));
    for (int64_t i = 0; i < count; i++) {
      if (ids[i].is_valid()) {
        map_.emplace(ids[i].as_u64(), int(i));
      }
    }
  }

  /** Returns -1 if the element no longer exists, e.g. a SubD level was lowered. The edit that
   * refers to it is paused, not deleted. */
  int find(const StableID id) const
  {
    const auto it = map_.find(id.as_u64());
    return it == map_.end() ? -1 : it->second;
  }

 private:
  std::unordered_map<uint64_t, int> map_;
};

/**
 * Find the elements that were made from a parent element. Built from the per-element parent IDs
 * (the `.stable_parent_*` attributes). Used to run an operation stored on a face on the faces it was split into.
 */
class ParentIndex {
 public:
  ParentIndex() = default;

  ParentIndex(const StableID *parents, const int64_t count)
  {
    for (int64_t i = 0; i < count; i++) {
      if (parents[i].is_valid()) {
        children_[parents[i].as_u64()].push_back(int(i));
      }
    }
  }

  /** Direct children, in increasing index order. Empty if there are none. */
  const std::vector<int> &children(const StableID parent) const
  {
    static const std::vector<int> none;
    const auto it = children_.find(parent.as_u64());
    return it == children_.end() ? none : it->second;
  }

 private:
  std::unordered_map<uint64_t, std::vector<int>> children_;
};

}  // namespace blender::bke::stable_id
