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

#include "NOD_geometry_nodes_lazy_function.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_edit_poly_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry"_ustr)
      .supported_type(GeometryComponent::Type::Mesh)
      .description("Mesh to edit. Other geometry passes through");
  b.add_output<decl::Geometry>("Geometry"_ustr).propagate_all_geometry().align_with_previous();
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Geometry"_ustr);
  const GeoNodesUserData *user_data = params.user_data();
  if (user_data && user_data->call_data->edit_poly_params) {
    if (const std::optional<FoundNestedNodeID> found_id = find_nested_node_id(
            *user_data, params.node().identifier))
    {
      std::string warning;
      user_data->call_data->edit_poly_params->execute(found_id->id, geometry, warning);
      if (!warning.empty()) {
        params.error_message_add(NodeWarningType::Warning, warning);
      }
    }
  }
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
