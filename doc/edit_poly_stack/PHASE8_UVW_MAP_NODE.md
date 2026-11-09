# UVW Map node (Geometry Nodes)

`GeometryNodeUVWMap`, `node_geo_uvw_map.cc`; settings and evaluation in `MOD_nodes.cc`; the
projection in `MOD_uvw_map.cc` (`uvw_map::apply`, shared with the UVW Map modifier); gizmo tools
in `scripts/startup/bl_operators/uvw_map.py`.

## Settings

All the settings of the UVW Map modifier (projection, alignment, map channel / UV map, material,
size mode, tile, offset, flip, gizmo or gizmo object), except Selected Faces Only: the node has a
Selection input instead (the setting is ignored by nodes). They are stored per
object in the Geometry Nodes modifier (`NodesModifierData::uvw_maps`, one `UVWMapModifierData`
per nested node ID, created when the node tree changes), like the edits of Edit Poly nodes, and
shown in a UVW Map panel of the modifier, drawn by the same code as the modifier's panel.
`NodesModifier.uvw_map_settings(id)` gives them to Python as a `UVWMapModifier`, so the gizmo
tools work on either.

The node also has a Selection input (faces to map), e.g. Selected Faces of an Edit Poly node.

## Gizmo

The gizmo of the UVW Map node closest to the output (or the active node, if it is a UVW Map node)
is drawn while the Geometry Nodes modifier is active. Edit Gizmo, Fit, Center and View Align work
as for the modifier; Fit and Center use the mesh arriving at the node (captured during
evaluation). Tab edits the gizmo when the active node of the tree is a UVW Map node, or when the
tree has UVW Map nodes and no Edit Poly nodes; Tab again returns to the object.

The projection uses positions only, not stable IDs, so it keeps working whatever the nodes before
it do.

Check: `tests/python/bl_node_uvw_map.py`.
