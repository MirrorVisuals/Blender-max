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
