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
