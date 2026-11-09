# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# UVW Map node: the UVW Map modifier's projections in a node tree, with its settings stored per
# object in the Geometry Nodes modifier, the gizmo tools (Edit Gizmo, Fit) and Tab.
# ./blender.bin --background --factory-startup --python tests/python/bl_node_uvw_map.py
import sys
import bpy
import os
import tempfile

failures = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


def uvs(ob, name="UVMap"):
    """Rounded UVs of the evaluated mesh, per face corner (None without that UV map)."""
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    attr = me.attributes.get(name)
    out = None
    if attr is not None and attr.domain == 'CORNER':
        out = [tuple(round(c, 4) for c in d.vector) for d in attr.data]
    ev.to_mesh_clear()
    return out


def new_tree(name, translate_z=0.0, selection_top_only=False):
    tree = bpy.data.node_groups.new(name, 'GeometryNodeTree')
    tree.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
    tree.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
    gi = tree.nodes.new('NodeGroupInput')
    go = tree.nodes.new('NodeGroupOutput')
    uvw = tree.nodes.new('GeometryNodeUVWMap')
    last = gi.outputs[0]
    if translate_z:
        t = tree.nodes.new('GeometryNodeTransform')
        t.inputs["Translation"].default_value = (0.0, 0.0, translate_z)
        tree.links.new(last, t.inputs["Geometry"])
        last = t.outputs["Geometry"]
    tree.links.new(last, uvw.inputs["Geometry"])
    tree.links.new(uvw.outputs["Geometry"], go.inputs[0])
    if selection_top_only:
        normal = tree.nodes.new('GeometryNodeInputNormal')
        sep = tree.nodes.new('ShaderNodeSeparateXYZ')
        cmp = tree.nodes.new('FunctionNodeCompare')
        cmp.data_type = 'FLOAT'
        cmp.operation = 'GREATER_THAN'
        cmp.inputs["B"].default_value = 0.5
        tree.links.new(normal.outputs[0], sep.inputs[0])
        tree.links.new(sep.outputs["Z"], cmp.inputs["A"])
        tree.links.new(cmp.outputs[0], uvw.inputs["Selection"])
    tree.nodes.active = uvw
    return tree


def add_object(tree, location):
    bpy.ops.mesh.primitive_cube_add(location=location)
    ob = bpy.context.active_object
    ob.data.uv_layers.remove(ob.data.uv_layers[0])
    mod = ob.modifiers.new("GeometryNodes", 'NODES')
    mod.node_group = tree
    ob.modifiers.active = mod
    return ob, mod


SETTINGS = {
    "map_type": 'BOX', "axis": 'Y', "size_mode": 'REAL_WORLD', "real_size": (0.5, 0.75),
    "tile": (2.0, 1.0, 1.0), "offset": (0.1, 0.2), "use_flip_u": True,
    "gizmo_location": (0.2, 0.0, 0.1), "gizmo_rotation": (0.0, 0.0, 0.3), "gizmo_size": (2.5, 2.0, 1.5),
}

bpy.ops.wm.read_factory_settings(use_empty=True)

print("The node maps like the modifier, with the same settings")
tree = new_tree("UVW Tree")
ob, mod = add_object(tree, (0.0, 0.0, 0.0))
node_id = mod.uvw_map_pick()
settings = mod.uvw_map_settings(id=node_id) if node_id != -1 else None
check(settings is not None, "the Geometry Nodes modifier has the node's settings")
check(settings is not None and settings.map_type == 'PLANAR' and settings.axis == 'Z',
      "new settings are the modifier's defaults (Planar, Z)")
check(uvs(ob) is not None, "the node writes the UV map \"UVMap\"")

bpy.ops.mesh.primitive_cube_add(location=(0.0, 4.0, 0.0))
ref = bpy.context.active_object
ref.data.uv_layers.remove(ref.data.uv_layers[0])
ref_mod = ref.modifiers.new("UVW Map", 'UVW_MAP')
check(uvs(ob) == uvs(ref), "default settings: same UVs as the modifier")
for key, value in SETTINGS.items():
    setattr(settings, key, value)
    setattr(ref_mod, key, value)
check(uvs(ob) == uvs(ref), "box, real world size, tile, offset, flip, moved gizmo: same UVs as the modifier")

print("Selection input")
sel_tree = new_tree("UVW Top Only", selection_top_only=True)
sel_ob, sel_mod = add_object(sel_tree, (4.0, 0.0, 0.0))
values = uvs(sel_ob)
mapped = sum(1 for uv in values if uv != (0.0, 0.0))
check(values is not None and 0 < mapped <= 4,
      f"only the top face is mapped ({mapped} of {len(values)} corners have UVs)")

sel_settings = sel_mod.uvw_map_settings(id=sel_mod.uvw_map_pick())
sel_settings.use_selection = True
check(uvs(sel_ob) == values, "Selected Faces Only is ignored by the node (its Selection input decides)")

print("Settings are per object")
other, other_mod = add_object(tree, (-4.0, 0.0, 0.0))
other_settings = other_mod.uvw_map_settings(id=other_mod.uvw_map_pick())
check(other_settings is not None and other_settings.map_type == 'PLANAR',
      "another object using the same node group has its own settings")
other_settings.map_type = 'CYLINDRICAL'
check(settings.map_type == 'BOX', "changing them does not change the first object's")

print("Edit Gizmo, Fit")
fit_tree = new_tree("UVW Fit", translate_z=3.0)
fit_ob, fit_mod = add_object(fit_tree, (8.0, 0.0, 0.0))
fit_id = fit_mod.uvw_map_pick()
bpy.ops.object.uvw_map_fit(modifier=fit_mod.name, node_id=fit_id)
fit_settings = fit_mod.uvw_map_settings(id=fit_id)
check(abs(fit_settings.gizmo_location[2] - 3.0) < 1e-4,
      "Fit uses the mesh arriving at the node (moved up 3 by a node before it)")
bpy.ops.object.uvw_map_gizmo_edit(modifier=fit_mod.name, node_id=fit_id)
helper = bpy.context.active_object
check(helper is not None and helper != fit_ob, "Edit Gizmo selects a helper for the node's gizmo")
helper.location.x += 0.5
bpy.context.view_layer.update()
bpy.ops.object.uvw_map_gizmo_edit()
check(bpy.context.active_object == fit_ob and abs(fit_settings.gizmo_location[0] - 0.5) < 1e-4,
      "moving the helper moves the node's gizmo; the object is selected again")

print("Tab")
for o in bpy.context.view_layer.objects:
    o.select_set(False)
fit_ob.select_set(True)
bpy.context.view_layer.objects.active = fit_ob
bpy.ops.object.edit_poly_tab()
check(bpy.context.active_object != fit_ob, "Tab with the UVW Map node active edits its gizmo")
bpy.ops.object.edit_poly_tab()
check(bpy.context.active_object == fit_ob and fit_ob.mode == 'OBJECT', "Tab again returns to the object")

print("Save and reload")
expected = uvs(ob)
ob_name = ob.name
path = os.path.join(tempfile.gettempdir(), "uvw_map_node_check.blend")
bpy.ops.wm.save_as_mainfile(filepath=path)
bpy.ops.wm.open_mainfile(filepath=path)
ob = bpy.data.objects[ob_name]
mod = ob.modifiers["GeometryNodes"]
settings = mod.uvw_map_settings(id=mod.uvw_map_pick())
check(settings is not None and settings.map_type == 'BOX' and settings.size_mode == 'REAL_WORLD',
      "settings survive save and reload")
check(uvs(ob) == expected, "same UVs after reload")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
for f in failures:
    print("  -", f)
# A non-zero exit code makes the test fail when run by CTest.
sys.exit(1 if failures else 0)
