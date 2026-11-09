/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "testing/testing.h"

#include <set>

#include "BKE_stable_id.hh"

namespace blender::bke::stable_id::tests {

/* The rule must be usable at compile time, and the level-independence must hold there too. */
static_assert(make_root(1, 0).is_valid());
static_assert(subd_edge_point(make_root(1, 0), make_root(1, 1), 1, 2) ==
              subd_edge_point(make_root(1, 1), make_root(1, 0), 4, 8));
static_assert(Fraction::make(6, 8).num == 3 && Fraction::make(6, 8).den == 4);
static_assert(Fraction::make(0, 4).num == 0 && Fraction::make(0, 4).den == 1);

TEST(stable_id, root_ids_are_valid_and_unique)
{
  std::set<uint64_t> seen;
  for (int i = 0; i < 100000; i++) {
    const StableID id = make_root(1234, i);
    EXPECT_TRUE(id.is_valid());
    EXPECT_TRUE(seen.insert(id.as_u64()).second);
  }
}

TEST(stable_id, root_ids_depend_on_seed)
{
  EXPECT_NE(make_root(1, 0), make_root(2, 0));
}

TEST(stable_id, derive_is_deterministic_and_tag_sensitive)
{
  const StableID p = make_root(7, 3);
  EXPECT_EQ(derive(Derivation::ArrayCopy, p, 2), derive(Derivation::ArrayCopy, p, 2));
  EXPECT_NE(derive(Derivation::ArrayCopy, p, 2), derive(Derivation::ArrayCopy, p, 3));
  EXPECT_NE(derive(Derivation::ArrayCopy, p, 2), derive(Derivation::MirrorCopy, p, 2));
}

TEST(stable_id, unordered_derivation_ignores_parent_order)
{
  const StableID a = make_root(7, 1);
  const StableID b = make_root(7, 2);
  EXPECT_EQ(derive_unordered(Derivation::Generated, a, b),
            derive_unordered(Derivation::Generated, b, a));
}

TEST(stable_id, subd_midpoint_is_the_same_at_every_level)
{
  const StableID v0 = make_root(9, 0);
  const StableID v1 = make_root(9, 1);
  /* Level 1 has the midpoint at 1/2. Level 2 has points at 1/4, 2/4, 3/4. Level 3 has 4/8. */
  const StableID level1 = subd_edge_point(v0, v1, 1, 2);
  const StableID level2 = subd_edge_point(v0, v1, 2, 4);
  const StableID level3 = subd_edge_point(v0, v1, 4, 8);
  EXPECT_EQ(level1, level2);
  EXPECT_EQ(level1, level3);
  /* The quarter points are new elements. */
  EXPECT_NE(subd_edge_point(v0, v1, 1, 4), level1);
  EXPECT_NE(subd_edge_point(v0, v1, 1, 4), subd_edge_point(v0, v1, 3, 4));
}

TEST(stable_id, subd_edge_point_ignores_edge_direction)
{
  const StableID v0 = make_root(9, 0);
  const StableID v1 = make_root(9, 1);
  for (uint32_t steps = 2; steps <= 8; steps++) {
    for (uint32_t step = 1; step < steps; step++) {
      EXPECT_EQ(subd_edge_point(v0, v1, step, steps),
                subd_edge_point(v1, v0, steps - step, steps));
    }
  }
}

TEST(stable_id, subd_face_point_levels_agree)
{
  const StableID face = make_root(5, 12);
  const Fraction half = Fraction::make(1, 2);
  EXPECT_EQ(subd_face_point(face, half, half),
            subd_face_point(face, Fraction::make(2, 4), Fraction::make(4, 8)));
}

TEST(stable_id, canonical_start_ignores_corner_rotation)
{
  const StableID ids[4] = {make_root(1, 10), make_root(1, 11), make_root(1, 12), make_root(1, 13)};
  const StableID smallest = *std::min_element(ids, ids + 4);
  for (int rotation = 0; rotation < 4; rotation++) {
    StableID rotated[4];
    for (int i = 0; i < 4; i++) {
      rotated[i] = ids[(i + rotation) % 4];
    }
    EXPECT_EQ(rotated[canonical_start(rotated, 4)], smallest);
  }
}

TEST(stable_id, resolve_collisions_makes_ids_unique_and_keeps_first)
{
  const StableID a = make_root(1, 0);
  const StableID b = make_root(1, 1);
  StableID ids[4] = {a, b, a, a};
  EXPECT_EQ(resolve_collisions(ids, 4), 2);
  EXPECT_EQ(ids[0], a);
  EXPECT_EQ(ids[1], b);
  std::set<uint64_t> unique;
  for (const StableID id : ids) {
    EXPECT_TRUE(unique.insert(id.as_u64()).second);
  }
  /* Same input gives the same output. */
  StableID again[4] = {a, b, a, a};
  resolve_collisions(again, 4);
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(ids[i], again[i]);
  }
}

TEST(stable_id, resolve_collisions_leaves_missing_ids_alone)
{
  const StableID a = make_root(1, 0);
  StableID ids[4] = {StableID{}, a, StableID{}, StableID{}};
  EXPECT_EQ(resolve_collisions(ids, 4), 0);
  EXPECT_FALSE(ids[0].is_valid());
  EXPECT_FALSE(ids[2].is_valid());
  EXPECT_FALSE(ids[3].is_valid());
  EXPECT_EQ(ids[1], a);
}

TEST(stable_id, canonical_frame_ignores_winding)
{
  /* A quad and its flipped copy (same corners, reversed order). */
  const StableID quad[4] = {make_root(3, 0), make_root(3, 1), make_root(3, 2), make_root(3, 3)};
  const StableID flipped[4] = {quad[0], quad[3], quad[2], quad[1]};

  auto frame = [](const StableID *c) {
    const int s = canonical_start(c, 4);
    const int d = canonical_direction(c, 4, s);
    /* Origin, u-neighbour, v-neighbour. */
    return std::vector<uint64_t>{
        c[s].as_u64(), c[(s + d + 4) % 4].as_u64(), c[(s - d + 4) % 4].as_u64()};
  };
  EXPECT_EQ(frame(quad), frame(flipped));
}

/* Stored corner order of a quad after rotating by `rotation` and optionally flipping winding. */
static void reorder_quad(const StableID in[4],
                         const int rotation,
                         const bool flip,
                         StableID out[4])
{
  for (int i = 0; i < 4; i++) {
    const int j = flip ? (rotation - i + 8) % 4 : (i + rotation) % 4;
    out[i] = in[j];
  }
}

/* Grid position (a, b) of a corner `k` of `in` inside the reordered quad's stored frame. */
static void corner_pos(const StableID quad[4], const StableID v, const int n, int &a, int &b)
{
  const int pos[4][2] = {{0, 0}, {n, 0}, {n, n}, {0, n}};
  for (int i = 0; i < 4; i++) {
    if (quad[i] == v) {
      a = pos[i][0];
      b = pos[i][1];
    }
  }
}

TEST(stable_id, quad_points_ignore_corner_order_and_winding)
{
  const StableID base[4] = {make_root(4, 7), make_root(4, 2), make_root(4, 9), make_root(4, 5)};
  const int n = 4;
  /* Reference: a point at weights (wa toward base[1], wb toward base[3]) from base[0]. */
  for (int wa = 0; wa <= n; wa++) {
    for (int wb = 0; wb <= n; wb++) {
      int ref_u = -1, ref_v = -1;
      for (int rotation = 0; rotation < 4; rotation++) {
        for (const bool flip : {false, true}) {
          StableID q[4];
          reorder_quad(base, rotation, flip, q);
          /* Express the same physical point in q's stored frame via bilinear corner positions. */
          int a0, b0, a1, b1, a3, b3;
          corner_pos(q, base[0], n, a0, b0);
          corner_pos(q, base[1], n, a1, b1);
          corner_pos(q, base[3], n, a3, b3);
          const int a = a0 + (a1 - a0) * wa / n + (a3 - a0) * wb / n;
          const int b = b0 + (b1 - b0) * wa / n + (b3 - b0) * wb / n;
          int u, v;
          quad_to_canonical(q, a, b, n, u, v);
          if (ref_u < 0) {
            ref_u = u;
            ref_v = v;
          }
          EXPECT_EQ(u, ref_u);
          EXPECT_EQ(v, ref_v);
        }
      }
    }
  }
}

TEST(stable_id, quad_center_matches_across_levels)
{
  const StableID q[4] = {make_root(4, 0), make_root(4, 1), make_root(4, 2), make_root(4, 3)};
  const StableID face = make_root(6, 0);
  int u1, v1, u2, v2;
  quad_to_canonical(q, 1, 1, 2, u1, v1); /* Level 1 center. */
  quad_to_canonical(q, 2, 2, 4, u2, v2); /* Level 2 center. */
  EXPECT_EQ(subd_face_point(face, Fraction::make(u1, 2), Fraction::make(v1, 2)),
            subd_face_point(face, Fraction::make(u2, 4), Fraction::make(v2, 4)));
}

TEST(stable_id, ngon_points_are_level_independent_and_distinct)
{
  const StableID face = make_root(6, 1);
  const StableID a = make_root(1, 0), b = make_root(1, 1), c = make_root(1, 2);
  /* Spoke point halfway between edge midpoint and center, at levels 2 and 3. */
  EXPECT_EQ(subd_ngon_spoke_point(face, a, b, Fraction::make(1, 2)),
            subd_ngon_spoke_point(face, b, a, Fraction::make(2, 4)));
  EXPECT_NE(subd_ngon_spoke_point(face, a, b, Fraction::make(1, 2)),
            subd_ngon_spoke_point(face, b, c, Fraction::make(1, 2)));
  EXPECT_EQ(subd_ngon_patch_point(face, a, Fraction::make(1, 2), Fraction::make(1, 2)),
            subd_ngon_patch_point(face, a, Fraction::make(2, 4), Fraction::make(2, 4)));
  EXPECT_NE(subd_ngon_patch_point(face, a, Fraction::make(1, 4), Fraction::make(1, 2)),
            subd_ngon_patch_point(face, a, Fraction::make(1, 2), Fraction::make(1, 4)));
  EXPECT_NE(subd_ngon_patch_point(face, a, Fraction::make(1, 2), Fraction::make(1, 2)),
            subd_face_center(face));
}

TEST(stable_id, derive_from_set_ignores_order)
{
  StableID x[4] = {make_root(1, 3), make_root(1, 1), make_root(1, 4), make_root(1, 2)};
  StableID y[4] = {make_root(1, 2), make_root(1, 4), make_root(1, 1), make_root(1, 3)};
  EXPECT_EQ(derive_from_set(Derivation::SubdFacePiece, x, 4),
            derive_from_set(Derivation::SubdFacePiece, y, 4));
  StableID z[3] = {make_root(1, 3), make_root(1, 1), make_root(1, 4)};
  EXPECT_NE(derive_from_set(Derivation::SubdFacePiece, x, 4),
            derive_from_set(Derivation::SubdFacePiece, z, 3));
}

TEST(stable_id, id_index_finds_elements_after_reordering)
{
  StableID ids[3] = {make_root(1, 0), make_root(1, 1), make_root(1, 2)};
  IDIndex index(ids, 3);
  EXPECT_EQ(index.find(ids[1]), 1);
  EXPECT_EQ(index.find(make_root(1, 99)), -1);

  /* Reordered: the same IDs live at other indices. */
  StableID shuffled[3] = {ids[2], ids[0], ids[1]};
  IDIndex shuffled_index(shuffled, 3);
  EXPECT_EQ(shuffled_index.find(ids[1]), 2);
}

TEST(stable_id, parent_index_returns_children_in_order)
{
  const StableID f0 = make_root(1, 0);
  const StableID f1 = make_root(1, 1);
  /* After subdivision face f0 became elements 0, 1, 3 and face f1 became element 2. */
  StableID parents[4] = {f0, f0, f1, f0};
  ParentIndex index(parents, 4);
  EXPECT_EQ(index.children(f0), (std::vector<int>{0, 1, 3}));
  EXPECT_EQ(index.children(f1), (std::vector<int>{2}));
  EXPECT_TRUE(index.children(make_root(1, 5)).empty());
}

}  // namespace blender::bke::stable_id::tests
