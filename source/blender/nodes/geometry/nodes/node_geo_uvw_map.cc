/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 *
 * UVW Map node: the projections of the UVW Map modifier (as in 3ds Max) at a point in a node
 * tree. The settings and the gizmo are stored per object by the Geometry Nodes modifier (see
 * #GeoNodesEditPolyParams) and shown in its panel; Tab edits the gizmo.
 */

#include "BKE_mesh.hh"

#include "NOD_geometry_nodes_lazy_function.hh"

#include "node_geometry_util.hh"

namespace blender::nodes::node_geo_uvw_map_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Geometry>("Geometry"_ustr)
      .supported_type(GeometryComponent::Type::Mesh)
      .description("Mesh to map. Other geometry passes through");
  b.add_output<decl::Geometry>("Geometry"_ustr).propagate_all_geometry().align_with_previous();
  b.add_input<decl::Bool>("Selection"_ustr)
      .default_value(true)
      .hide_value()
      .evaluated_geometry_field()
      .description("Faces to map (e.g. Selected Faces of an Edit Poly node)");
}

static void node_geo_exec(GeoNodeExecParams params)
{
  GeometrySet geometry = params.extract_input<GeometrySet>("Geometry"_ustr);
  const Field<bool> selection = params.extract_input<Field<bool>>("Selection"_ustr);
  const GeoNodesUserData *user_data = params.user_data();
  if (user_data && user_data->call_data->edit_poly_params) {
    if (const std::optional<FoundNestedNodeID> found_id = find_nested_node_id(
            *user_data, params.node().identifier))
    {
      if (Mesh *mesh = geometry.get_mesh_for_write()) {
        const bke::MeshFieldContext context{*mesh, AttrDomain::Face};
        fn::FieldEvaluator evaluator{context, mesh->faces_num};
        evaluator.set_selection(selection);
        evaluator.evaluate();
        const IndexMask faces = evaluator.get_evaluated_selection_as_mask();
        std::string warning;
        user_data->call_data->edit_poly_params->uvw_map(found_id->id, *mesh, faces, warning);
        if (!warning.empty()) {
          params.error_message_add(NodeWarningType::Warning, warning);
        }
      }
    }
  }
  params.set_output("Geometry"_ustr, std::move(geometry));
}

static void node_register()
{
  static bke::bNodeType ntype;

  geo_node_type_base(&ntype, "GeometryNodeUVWMap"_ustr);
  ntype.ui_name = "UVW Map";
  ntype.ui_description =
      "Project UVs from a gizmo, as the UVW Map modifier: the settings are stored per object in "
      "the Geometry Nodes modifier panel, and Tab edits the gizmo";
  ntype.nclass = NODE_CLASS_GEOMETRY;
  ntype.declare = node_declare;
  ntype.geometry_node_execute = node_geo_exec;
  bke::node_register_type(ntype);
}
NOD_REGISTER_NODE(node_register)

}  // namespace blender::nodes::node_geo_uvw_map_cc
