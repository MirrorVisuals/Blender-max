/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * Reading and writing stable element IDs (see BKE_stable_id.hh) on a #Mesh.
 */

#include "BLI_array.hh"
#include "BLI_span.hh"

#include "BKE_attribute_enums.hh"
#include "BKE_stable_id.hh"

struct Mesh;

namespace blender::bke::stable_id {

/** Name of the ID attribute for a domain (point, edge or face). */
const char *id_attribute_name(AttrDomain domain);
/** Name of the parent ID attribute for a domain (point, edge or face). */
const char *parent_attribute_name(AttrDomain domain);

/** Number of elements of `mesh` in a domain (point, edge or face). */
int domain_size(const Mesh &mesh, AttrDomain domain);

/**
 * IDs of the elements of a domain. If the mesh has no IDs for the domain, root IDs are made from
 * element indices. That is the right answer for a base mesh with no history: in the Edit Poly
 * design the base mesh is never edited directly, so its indices do not change.
 */
Array<StableID> read_or_make_root_ids(const Mesh &mesh, AttrDomain domain);

/** True if the mesh stores IDs for the domain. */
bool has_ids(const Mesh &mesh, AttrDomain domain);

/**
 * Write IDs, and optionally parent IDs, for a domain. `parents` may be empty, in which case an
 * existing parent attribute is removed. Sizes must match the domain size.
 */
void write_ids(Mesh &mesh, AttrDomain domain, Span<StableID> ids, Span<StableID> parents);

/** Store root IDs for every domain that has none yet. */
void ensure_root_ids(Mesh &mesh);

/**
 * A block of a mesh that a modifier built by copying another mesh element by element (same
 * order), e.g. the mirrored half of Mirror or one copy of Array.
 */
struct IDPart {
  const Mesh *source = nullptr;
  int vert_start = 0;
  int edge_start = 0;
  int face_start = 0;
  /** The block keeps the source IDs (the original half / first copy). */
  bool keep = false;
  /** Otherwise: IDs are `derive(kind, source ID, a, b)` and the source ID is the parent. */
  Derivation kind = Derivation::Generated;
  uint64_t a = 0;
  uint64_t b = 0;
};

/**
 * Write IDs for vertices, edges and faces of `result` from the parts it was built from. Elements
 * outside every part get IDs from their index; duplicates are resolved (the first keeps its ID).
 */
void write_part_ids(Mesh &result, Span<IDPart> parts);

}  // namespace blender::bke::stable_id
