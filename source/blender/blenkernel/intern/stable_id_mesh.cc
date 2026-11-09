/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "DNA_mesh_types.h"

#include "BLI_math_vector_types.hh"
#include "BLI_task.hh"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BLI_sort.hh"
#include "BLI_array.hh"
#include "BKE_stable_id_mesh.hh"

namespace blender::bke::stable_id {

static_assert(sizeof(StableID) == sizeof(int2), "IDs are stored in Int32_2D attributes");

/** Seeds that separate the root IDs of different domains. Never change them. */
static uint64_t root_seed(const AttrDomain domain)
{
  switch (domain) {
    case AttrDomain::Point:
      return 0x5EED0001;
    case AttrDomain::Edge:
      return 0x5EED0002;
    case AttrDomain::Face:
      return 0x5EED0003;
    default:
      BLI_assert_unreachable();
      return 0;
  }
}

const char *id_attribute_name(const AttrDomain domain)
{
  switch (domain) {
    case AttrDomain::Point:
      return ATTR_ID_POINT;
    case AttrDomain::Edge:
      return ATTR_ID_EDGE;
    case AttrDomain::Face:
      return ATTR_ID_FACE;
    default:
      BLI_assert_unreachable();
      return "";
  }
}

const char *parent_attribute_name(const AttrDomain domain)
{
  switch (domain) {
    case AttrDomain::Point:
      return ATTR_PARENT_POINT;
    case AttrDomain::Edge:
      return ATTR_PARENT_EDGE;
    case AttrDomain::Face:
      return ATTR_PARENT_FACE;
    default:
      BLI_assert_unreachable();
      return "";
  }
}

int domain_size(const Mesh &mesh, const AttrDomain domain)
{
  switch (domain) {
    case AttrDomain::Point:
      return mesh.verts_num;
    case AttrDomain::Edge:
      return mesh.edges_num;
    case AttrDomain::Face:
      return mesh.faces_num;
    default:
      BLI_assert_unreachable();
      return 0;
  }
}

static AttributeReader<int2> lookup_exact(const Mesh &mesh,
                                          const char *name,
                                          const AttrDomain domain)
{
  AttributeReader<int2> reader = mesh.attributes().lookup<int2>(name);
  /* Only accept the attribute on its own domain; never an interpolated copy. */
  if (!reader || reader.domain != domain) {
    return {};
  }
  return reader;
}

bool has_ids(const Mesh &mesh, const AttrDomain domain)
{
  return bool(lookup_exact(mesh, id_attribute_name(domain), domain));
}

Array<StableID> read_or_make_root_ids(const Mesh &mesh, const AttrDomain domain)
{
  const int size = domain_size(mesh, domain);
  Array<StableID> ids(size);
  if (const AttributeReader<int2> reader = lookup_exact(mesh, id_attribute_name(domain), domain)) {
    reader.varray.materialize(MutableSpan<int2>(reinterpret_cast<int2 *>(ids.data()), size));
    return ids;
  }
  const uint64_t seed = root_seed(domain);
  threading::parallel_for(IndexRange(size), 4096, [&](const IndexRange range) {
    for (const int i : range) {
      ids[i] = make_root(seed, i);
    }
  });
  return ids;
}

static void write_attribute(Mesh &mesh,
                            const char *name,
                            const AttrDomain domain,
                            const Span<StableID> values)
{
  MutableAttributeAccessor attributes = mesh.attributes_for_write();
  if (const std::optional<AttributeMetaData> meta = attributes.lookup_meta_data(name)) {
    if (meta->domain != domain || meta->data_type != AttrType::Int32_2D) {
      attributes.remove(name);
    }
  }
  SpanAttributeWriter<int2> writer = attributes.lookup_or_add_for_write_only_span<int2>(name,
                                                                                         domain);
  if (!writer) {
    return;
  }
  writer.span.copy_from(Span<int2>(reinterpret_cast<const int2 *>(values.data()), values.size()));
  writer.finish();
}

void write_ids(Mesh &mesh,
               const AttrDomain domain,
               const Span<StableID> ids,
               const Span<StableID> parents)
{
  BLI_assert(ids.size() == domain_size(mesh, domain));
  write_attribute(mesh, id_attribute_name(domain), domain, ids);
  if (parents.is_empty()) {
    mesh.attributes_for_write().remove(parent_attribute_name(domain));
  }
  else {
    BLI_assert(parents.size() == ids.size());
    write_attribute(mesh, parent_attribute_name(domain), domain, parents);
  }
}

/**
 * #resolve_collisions, with a fast check first: duplicates are rare, and sorting a copy of the
 * IDs in parallel finds out much faster than hashing millions of them one by one.
 */
static void resolve_collisions_fast(MutableSpan<StableID> ids)
{
  Array<uint64_t> keys(ids.size());
  threading::parallel_for(ids.index_range(), 8192, [&](const IndexRange range) {
    for (const int64_t i : range) {
      keys[i] = ids[i].as_u64(); /* Invalid IDs are 0: they sort first and are skipped. */
    }
  });
  parallel_sort(keys.begin(), keys.end());
  bool duplicate = false;
  for (int64_t i = 1; i < keys.size(); i++) {
    if (keys[i] != 0 && keys[i] == keys[i - 1]) {
      duplicate = true;
      break;
    }
  }
  if (duplicate) {
    resolve_collisions(ids.data(), ids.size());
  }
}

void write_part_ids(Mesh &result, const Span<IDPart> parts)
{
  for (const AttrDomain domain : {AttrDomain::Point, AttrDomain::Edge, AttrDomain::Face}) {
    const int size = domain_size(result, domain);
    Array<StableID> ids(size, StableID{});
    Array<StableID> parents(size, StableID{});
    /* Read every distinct source once (Array uses the same source for all copies). */
    Vector<std::pair<const Mesh *, Array<StableID>>> sources;
    auto source_ids = [&](const Mesh *source) -> const Array<StableID> & {
      for (const auto &item : sources) {
        if (item.first == source) {
          return item.second;
        }
      }
      sources.append({source, read_or_make_root_ids(*source, domain)});
      return sources.last().second;
    };
    for (const IDPart &part : parts) {
      if (part.source == nullptr) {
        continue;
      }
      const Array<StableID> &src = source_ids(part.source);
      const int start = domain == AttrDomain::Point ? part.vert_start :
                        domain == AttrDomain::Edge  ? part.edge_start :
                                                      part.face_start;
      threading::parallel_for(src.index_range(), 4096, [&](const IndexRange range) {
        for (const int i : range) {
          const int dst = start + i;
          if (dst < 0 || dst >= size) {
            continue;
          }
          if (part.keep) {
            ids[dst] = src[i];
          }
          else {
            ids[dst] = derive(part.kind, src[i], part.a, part.b);
            parents[dst] = src[i];
          }
        }
      });
    }
    const uint64_t seed = root_seed(domain) ^ 0xA11D;
    threading::parallel_for(IndexRange(size), 4096, [&](const IndexRange range) {
      for (const int i : range) {
        if (!ids[i].is_valid()) {
          ids[i] = make_root(seed, i);
        }
      }
    });
    resolve_collisions_fast(ids);
    write_ids(result, domain, ids, parents);
  }
}

void ensure_root_ids(Mesh &mesh)
{
  for (const AttrDomain domain : {AttrDomain::Point, AttrDomain::Edge, AttrDomain::Face}) {
    if (!has_ids(mesh, domain)) {
      const Array<StableID> ids = read_or_make_root_ids(mesh, domain);
      write_ids(mesh, domain, ids, {});
    }
  }
}

}  // namespace blender::bke::stable_id
