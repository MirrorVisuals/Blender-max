/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * Edit Poly node: the edits of an Edit Poly modifier level, made at a point in a node tree. The
 * edits are stored per object by the Geometry Nodes modifier (see #GeoNodesEditPolyParams), and
 * made in Edit Mode with Tab.
 */

#include "BKE_attribute.hh"
#include "BKE_mesh.hh"

#include "NOD_geometry_nodes_lazy_function.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_edit_poly_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry"_ustr)
      .supported_type(GeometryComponent::Type::Mesh)
      .description("Mesh to edit. Other geometry passes through");
  b.add_output<decl::Geometry>("Geometry"_ustr).propagate_all_geometry().align_with_previous();
  b.add_output<decl::Bool>("Selected Points"_ustr)
      .anonymous_attribute_output()
      .description("Points selected when the node was last edited (live while editing)");
  b.add_output<decl::Bool>("Selected Edges"_ustr)
      .anonymous_attribute_output()
      .description("Edges selected when the node was last edited, e.g. for a Bevel node");
  b.add_output<decl::Bool>("Selected Faces"_ustr)
      .anonymous_attribute_output()
      .description("Faces selected when the node was last edited");
}

/** Copy the node's selection (in `.select_*` attributes) to a selection output. */
static void output_selection(GeoNodeExecParams &params,
                             Mesh *mesh,
                             const bool has_selection,
                             const UString output,
                             const StringRef selection_name,
                             const AttrDomain domain)
{
  const std::optional<std::string> id = params.get_output_anonymous_attribute_id_if_needed(output);
  if (!id || mesh == nullptr) {
    return;
  }
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  bke::SpanAttributeWriter<bool> selection = attributes.lookup_or_add_for_write_only_span<bool>(
      *id, domain);
  if (!selection) {
    return;
  }
  if (has_selection) {
    const VArraySpan<bool> source = *attributes.lookup_or_default<bool>(
        selection_name, domain, false);
    selection.span.copy_from(source);
  }
  else {
    selection.span.fill(false);
  }
  selection.finish();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Geometry"_ustr);
  bool has_selection = false;
  const GeoNodesUserData *user_data = params.user_data();
  if (user_data && user_data->call_data->edit_poly_params) {
    if (const std::optional<FoundNestedNodeID> found_id = find_nested_node_id(
            *user_data, params.node().identifier))
    {
      std::string warning;
      user_data->call_data->edit_poly_params->execute(
          found_id->id, geometry, warning, has_selection);
      if (!warning.empty()) {
        params.error_message_add(NodeWarningType::Warning, warning);
      }
    }
  }
  const bool selection_used =
      params.anonymous_attribute_output_is_required("Selected Points"_ustr) ||
      params.anonymous_attribute_output_is_required("Selected Edges"_ustr) ||
      params.anonymous_attribute_output_is_required("Selected Faces"_ustr);
  Mesh *mesh = selection_used ? geometry.get_mesh_for_write() : nullptr;
  output_selection(
      params, mesh, has_selection, "Selected Points"_ustr, ".select_vert", AttrDomain::Point);
  output_selection(
      params, mesh, has_selection, "Selected Edges"_ustr, ".select_edge", AttrDomain::Edge);
  output_selection(
      params, mesh, has_selection, "Selected Faces"_ustr, ".select_poly", AttrDomain::Face);
  params.set_output("Geometry"_ustr, std::move(geometry));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeEditPoly"_ustr);
  ntype.ui_name = "Edit Poly";
  ntype.ui_description =
      "Edits made in Edit Mode, stored per object: press Tab with the Geometry Nodes modifier "
      "active to edit the mesh at this point of the node tree";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_edit_poly_cc
