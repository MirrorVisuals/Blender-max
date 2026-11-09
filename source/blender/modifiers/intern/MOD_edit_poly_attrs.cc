/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include <atomic>
#include <cmath>
#include <optional>
#include <utility>

#include "BLI_map.hh"
#include "BLI_set.hh"
#include "BLI_task.hh"

#include "DNA_mesh_types.h"

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_mapping.hh"
#include "BKE_stable_id_mesh.hh"

#include "MOD_edit_poly_attrs.hh"

namespace blender::edit_poly {

using bke::AttrDomain;

enum class ValueType { Float, Bool, Int };

struct AttrInfo {
  const char *name;
  AttrDomain domain;
  ValueType type;
};

/* The order is part of the stored format: only append. */
static const AttrInfo ATTRS[] = {
    {"bevel_weight_vert", AttrDomain::Point, ValueType::Float},
    {"crease_vert", AttrDomain::Point, ValueType::Float},
    {"bevel_weight_edge", AttrDomain::Edge, ValueType::Float},
    {"crease_edge", AttrDomain::Edge, ValueType::Float},
    {"sharp_edge", AttrDomain::Edge, ValueType::Bool},
    {"uv_seam", AttrDomain::Edge, ValueType::Bool},
    {"material_index", AttrDomain::Face, ValueType::Int},
    {"sharp_face", AttrDomain::Face, ValueType::Bool},
};
static constexpr int ATTRS_NUM = int(sizeof(ATTRS) / sizeof(ATTRS[0]));
static constexpr int FORMAT_VERSION = 1;

using EdgeKey = std::pair<uint64_t, uint64_t>;

static EdgeKey edge_key(const StableID a, const StableID b)
{
  const uint64_t x = a.as_u64(), y = b.as_u64();
  return x < y ? EdgeKey(x, y) : EdgeKey(y, x);
}

struct Item {
  StableID a;
  StableID b; /* Second vertex, for edges. */
  float value = 0.0f;
};

static StableID id_from(const int lo, const int hi)
{
  return StableID{uint32_t(lo), uint32_t(hi)};
}

static void decode(const Span<int> ints, const Span<float> floats, Array<Vector<Item>> &r_items)
{
  r_items.reinitialize(ATTRS_NUM);
  if (ints.size() < 2 || ints[0] != FORMAT_VERSION) {
    return;
  }
  const int records = ints[1];
  int64_t p = 2, f = 0;
  for (int r = 0; r < records; r++) {
    if (p + 2 > ints.size()) {
      return;
    }
    const int code = ints[p];
    const int count = ints[p + 1];
    p += 2;
    if (code < 0 || code >= ATTRS_NUM || count < 0) {
      return;
    }
    const bool is_edge = ATTRS[code].domain == AttrDomain::Edge;
    const int key_size = is_edge ? 4 : 2;
    for (int i = 0; i < count; i++) {
      if (p + key_size > ints.size() || f >= floats.size()) {
        return;
      }
      Item item;
      item.a = id_from(ints[p], ints[p + 1]);
      if (is_edge) {
        item.b = id_from(ints[p + 2], ints[p + 3]);
      }
      item.value = floats[f++];
      p += key_size;
      r_items[code].append(item);
    }
  }
}

static void encode(const Span<Vector<Item>> items, Vector<int> &r_ints, Vector<float> &r_floats)
{
  r_ints.clear();
  r_floats.clear();
  int records = 0;
  for (const Vector<Item> &list : items) {
    records += list.is_empty() ? 0 : 1;
  }
  if (records == 0) {
    return;
  }
  r_ints.extend({FORMAT_VERSION, records});
  for (const int code : items.index_range()) {
    if (items[code].is_empty()) {
      continue;
    }
    const bool is_edge = ATTRS[code].domain == AttrDomain::Edge;
    r_ints.extend({code, int(items[code].size())});
    for (const Item &item : items[code]) {
      r_ints.extend({int(item.a.lo), int(item.a.hi)});
      if (is_edge) {
        r_ints.extend({int(item.b.lo), int(item.b.hi)});
      }
      r_floats.append(item.value);
    }
  }
}

int attrs_count(const Span<int> ints)
{
  if (ints.size() < 2 || ints[0] != FORMAT_VERSION) {
    return 0;
  }
  int count = 0;
  int64_t p = 2;
  for (int r = 0; r < ints[1] && p + 2 <= ints.size(); r++) {
    const int code = ints[p];
    const int n = ints[p + 1];
    if (code < 0 || code >= ATTRS_NUM || n < 0) {
      break;
    }
    count += n;
    p += 2 + int64_t(n) * (ATTRS[code].domain == AttrDomain::Edge ? 4 : 2);
  }
  return count;
}

static Map<uint64_t, int> index_of(const Span<StableID> ids)
{
  Map<uint64_t, int> map;
  for (const int i : ids.index_range()) {
    map.add(ids[i].as_u64(), i);
  }
  return map;
}

static Map<EdgeKey, int> edge_index_of(const Mesh &mesh, const Span<StableID> vert_ids)
{
  Map<EdgeKey, int> map;
  const Span<int2> edges = mesh.edges();
  for (const int e : edges.index_range()) {
    const StableID a = vert_ids[edges[e][0]];
    const StableID b = vert_ids[edges[e][1]];
    if (a.is_valid() && b.is_valid()) {
      map.add(edge_key(a, b), e);
    }
  }
  return map;
}

/**
 * Input index of an ID: tries the index the element is expected at first (a session keeps the
 * order of the input until its topology changes), and builds a table of all IDs only the first
 * time that guess is wrong.
 */
class HintedIndex {
 public:
  explicit HintedIndex(const Span<StableID> ids) : ids_(ids) {}

  int find(const StableID id, const int64_t hint)
  {
    if (hint >= 0 && hint < ids_.size() && ids_[hint] == id) {
      return int(hint);
    }
    if (!map_) {
      map_.emplace(index_of(ids_));
    }
    return map_->lookup_default(id.as_u64(), -1);
  }

 private:
  Span<StableID> ids_;
  std::optional<Map<uint64_t, int>> map_;
};

void attrs_compute(const Mesh &input,
                   const Mesh &edited,
                   const Span<StableID> edited_vert_ids,
                   const Span<StableID> edited_face_ids,
                   const Span<int> old_ints,
                   const Span<float> old_floats,
                   Vector<int> &r_ints,
                   Vector<float> &r_floats)
{
  using namespace bke::stable_id;
  Array<Vector<Item>> old;
  decode(old_ints, old_floats, old);
  Array<Vector<Item>> items(ATTRS_NUM);

  if (edited_vert_ids.size() != edited.verts_num || edited_face_ids.size() != edited.faces_num) {
    encode(old, r_ints, r_floats);
    return;
  }

  const bke::AttributeAccessor cur_attrs = edited.attributes();
  const bke::AttributeAccessor in_attrs = input.attributes();
  const Array<StableID> in_vert_ids = read_or_make_root_ids(input, AttrDomain::Point);
  const Array<StableID> in_face_ids = read_or_make_root_ids(input, AttrDomain::Face);
  HintedIndex in_vert_of(in_vert_ids);
  HintedIndex in_face_of(in_face_ids);
  const Span<int2> edges = edited.edges();
  const Span<int2> in_edges = input.edges();

  /* Input vertex of every session vertex (-1 for new ones), shared by the attributes below. */
  std::optional<Array<int>> vert_input;
  auto input_of_verts = [&]() -> const Array<int> & {
    if (!vert_input) {
      vert_input.emplace(edited_vert_ids.size());
      for (const int64_t v : edited_vert_ids.index_range()) {
        (*vert_input)[v] = edited_vert_ids[v].is_valid() ? in_vert_of.find(edited_vert_ids[v], v) :
                                                           -1;
      }
    }
    return *vert_input;
  };
  /* Input edges around each input vertex, to find the input edge of a session edge. */
  Array<int> vert_edge_offsets;
  Array<int> vert_edge_indices;
  std::optional<GroupedSpan<int>> in_vert_edges;

  for (const int code : IndexRange(ATTRS_NUM)) {
    const AttrInfo &info = ATTRS[code];
    const bool compare = bool(cur_attrs.lookup_meta_data(info.name)) ||
                         bool(in_attrs.lookup_meta_data(info.name));
    if (!compare && old[code].is_empty()) {
      continue; /* Neither mesh has it and nothing is stored: nothing to do. */
    }
    if (compare) {
      const VArraySpan<float> cur = *cur_attrs.lookup_or_default<float>(
          info.name, info.domain, 0.0f);
      const VArraySpan<float> base = *in_attrs.lookup_or_default<float>(
          info.name, info.domain, 0.0f);
      auto add_if_changed = [&](const float value, const int in_index, Item item) {
        const float baseline = in_index >= 0 ? base[in_index] : 0.0f;
        if (std::abs(value - baseline) > 1e-6f) {
          item.value = value;
          items[code].append(item);
        }
      };
      switch (info.domain) {
        case AttrDomain::Point: {
          const Array<int> &vin = input_of_verts();
          for (const int v : edited_vert_ids.index_range()) {
            if (edited_vert_ids[v].is_valid()) {
              add_if_changed(cur[v], vin[v], Item{edited_vert_ids[v], {}});
            }
          }
          break;
        }
        case AttrDomain::Face:
          for (const int f : edited_face_ids.index_range()) {
            const StableID id = edited_face_ids[f];
            if (id.is_valid()) {
              add_if_changed(cur[f], in_face_of.find(id, f), Item{id, {}});
            }
          }
          break;
        case AttrDomain::Edge: {
          const Array<int> &vin = input_of_verts();
          if (!in_vert_edges) {
            in_vert_edges.emplace(bke::mesh::build_vert_to_edge_map(
                in_edges, input.verts_num, vert_edge_offsets, vert_edge_indices));
          }
          for (const int e : edges.index_range()) {
            const int v0 = edges[e][0], v1 = edges[e][1];
            const StableID a = edited_vert_ids[v0];
            const StableID b = edited_vert_ids[v1];
            if (!a.is_valid() || !b.is_valid()) {
              continue;
            }
            int in_edge = -1;
            const int ia = vin[v0], ib = vin[v1];
            if (ia != -1 && ib != -1) {
              for (const int candidate : (*in_vert_edges)[ia]) {
                const int2 ie = in_edges[candidate];
                if ((ie[0] == ia && ie[1] == ib) || (ie[0] == ib && ie[1] == ia)) {
                  in_edge = candidate;
                  break;
                }
              }
            }
            add_if_changed(cur[e], in_edge, Item{a, b});
          }
          break;
        }
        default:
          break;
      }
    }
    if (old[code].is_empty()) {
      continue;
    }
    /* Keep the stored items of elements that are not in this session (paused): a table of the
     * stored items, checked against every element of the session. */
    const bool is_edge = info.domain == AttrDomain::Edge;
    Map<uint64_t, int> old_of;
    Map<EdgeKey, int> old_edge_of;
    for (const int i : old[code].index_range()) {
      const Item &item = old[code][i];
      if (is_edge) {
        old_edge_of.add(edge_key(item.a, item.b), i);
      }
      else {
        old_of.add(item.a.as_u64(), i);
      }
    }
    Array<bool> here(old[code].size(), false);
    if (info.domain == AttrDomain::Point) {
      for (const StableID id : edited_vert_ids) {
        const int i = old_of.lookup_default(id.as_u64(), -1);
        if (i != -1) {
          here[i] = true;
        }
      }
    }
    else if (info.domain == AttrDomain::Face) {
      for (const StableID id : edited_face_ids) {
        const int i = old_of.lookup_default(id.as_u64(), -1);
        if (i != -1) {
          here[i] = true;
        }
      }
    }
    else if (is_edge) {
      for (const int2 &e : edges) {
        const int i = old_edge_of.lookup_default(
            edge_key(edited_vert_ids[e[0]], edited_vert_ids[e[1]]), -1);
        if (i != -1) {
          here[i] = true;
        }
      }
    }
    for (const int i : old[code].index_range()) {
      if (!here[i]) {
        items[code].append(old[code][i]);
      }
    }
  }
  encode(items, r_ints, r_floats);
}

int attrs_apply(const Span<int> ints,
                const Span<float> floats,
                Mesh &mesh,
                const Span<StableID> vert_ids,
                const Span<StableID> face_ids)
{
  Array<Vector<Item>> items;
  decode(ints, floats, items);
  bke::MutableAttributeAccessor attributes = mesh.attributes_for_write();
  const Span<int2> edges = mesh.edges();

  int paused = 0;
  for (const int code : IndexRange(ATTRS_NUM)) {
    if (items[code].is_empty()) {
      continue;
    }
    const AttrInfo &info = ATTRS[code];
    /* A small table of the stored items, checked against every element in parallel (much faster
     * than a table of every element when only some of them were edited). */
    Map<uint64_t, int> item_of;
    Map<EdgeKey, int> edge_item_of;
    for (const int i : items[code].index_range()) {
      const Item &item = items[code][i];
      if (info.domain == AttrDomain::Edge) {
        edge_item_of.add(edge_key(item.a, item.b), i);
      }
      else {
        item_of.add(item.a.as_u64(), i);
      }
    }
    const int64_t size = info.domain == AttrDomain::Point ? vert_ids.size() :
                         info.domain == AttrDomain::Face  ? face_ids.size() :
                                                            edges.size();
    Array<int> target(size, -1);
    std::atomic<int> found = 0;
    threading::parallel_for(IndexRange(size), 4096, [&](const IndexRange range) {
      int local = 0;
      for (const int64_t e : range) {
        int i = -1;
        if (info.domain == AttrDomain::Point) {
          i = item_of.lookup_default(vert_ids[e].as_u64(), -1);
        }
        else if (info.domain == AttrDomain::Face) {
          i = item_of.lookup_default(face_ids[e].as_u64(), -1);
        }
        else {
          i = edge_item_of.lookup_default(
              edge_key(vert_ids[edges[e][0]], vert_ids[edges[e][1]]), -1);
        }
        target[e] = i;
        local += (i != -1);
      }
      found += local;
    });
    paused += int(items[code].size()) - found.load();
    if (found.load() == 0) {
      continue;
    }
    auto write = [&](auto &&set) {
      threading::parallel_for(IndexRange(size), 4096, [&](const IndexRange range) {
        for (const int64_t e : range) {
          if (target[e] != -1) {
            set(e, items[code][target[e]].value);
          }
        }
      });
    };
    switch (info.type) {
      case ValueType::Float: {
        bke::SpanAttributeWriter<float> w = attributes.lookup_or_add_for_write_span<float>(
            info.name, info.domain);
        if (w) {
          write([&](const int64_t e, const float value) { w.span[e] = value; });
          w.finish();
        }
        break;
      }
      case ValueType::Bool: {
        bke::SpanAttributeWriter<bool> w = attributes.lookup_or_add_for_write_span<bool>(
            info.name, info.domain);
        if (w) {
          write([&](const int64_t e, const float value) { w.span[e] = value > 0.5f; });
          w.finish();
        }
        break;
      }
      case ValueType::Int: {
        bke::SpanAttributeWriter<int> w = attributes.lookup_or_add_for_write_span<int>(
            info.name, info.domain);
        if (w) {
          write([&](const int64_t e, const float value) { w.span[e] = int(std::lround(value)); });
          w.finish();
        }
        break;
      }
    }
  }
  return paused;
}

}  // namespace blender::edit_poly
