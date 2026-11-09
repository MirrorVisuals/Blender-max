# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Edit Poly node: Tab edits the node's result, the edits are stored per object in the Geometry
# Nodes modifier, and Tab never adds an Edit Poly modifier when the node is there.
# ./blender.bin --background --factory-startup --python tests/python/bl_node_edit_poly.py
import sys
import bpy
import bmesh
import os
import tempfile

failures = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


def tab():
    try:
        return bpy.ops.object.edit_poly_tab()
    except RuntimeError as ex:
        print("        (reported:", str(ex).strip().splitlines()[-1], ")")
        return {'CANCELLED'}


def evaluated(ob):
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    info = {"verts": len(me.vertices), "faces": len(me.polygons),
            "max_z": round(max(v.co.z for v in me.vertices), 4)}
    ev.to_mesh_clear()
    return info


def make_tree():
    tree = bpy.data.node_groups.new("Edit Poly Tree", 'GeometryNodeTree')
    tree.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
    tree.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
    nodes = tree.nodes
    group_in = nodes.new('NodeGroupInput')
    group_out = nodes.new('NodeGroupOutput')
    edit = nodes.new('GeometryNodeEditPoly')
    transform = nodes.new('GeometryNodeTransform')
    transform.inputs["Translation"].default_value = (0.0, 0.0, 1.0)
    tree.links.new(group_in.outputs[0], edit.inputs[0])
    tree.links.new(edit.outputs[0], transform.inputs[0])
    tree.links.new(transform.outputs[0], group_out.inputs[0])
    return tree


def add_nodes_modifier(ob, tree):
    mod = ob.modifiers.new("GeometryNodes", 'NODES')
    mod.node_group = tree
    ob.modifiers.active = mod
    return mod


def select_only(ob, pred):
    bm = bmesh.from_edit_mesh(ob.data)
    for v in bm.verts:
        v.select_set(False)
    n = 0
    for f in bm.faces:
        f.select_set(pred(f))
        n += pred(f)
    bm.select_flush_mode()
    bmesh.update_edit_mesh(ob.data)
    return n


bpy.ops.wm.read_factory_settings(use_empty=True)
tree = make_tree()

bpy.ops.mesh.primitive_cube_add()
ob = bpy.context.active_object
base = ob.data
base_co = [tuple(v.co) for v in base.vertices]
mod = add_nodes_modifier(ob, tree)

bpy.ops.mesh.primitive_cube_add(location=(4.0, 0.0, 0.0))
other = bpy.context.active_object
other_mod = add_nodes_modifier(other, tree)

plain = evaluated(ob)
check(plain["verts"] == 8 and plain["max_z"] == 2.0, "the node passes the cube through (no edits yet)")
node_id = mod.edit_poly_pick()
check(node_id != -1, "Tab finds the Edit Poly node in the active Geometry Nodes modifier")

print("Move the top of the cube in the node's session")
bpy.context.view_layer.objects.active = ob
tab()
check(ob.mode == 'EDIT' and ob.data != base, "Tab enters Edit Mode on a session mesh")
check(not any(m.type == 'EDIT_POLY' for m in ob.modifiers), "no Edit Poly modifier was added")
check(mod.edit_poly_session_get() == node_id, "the node is the one being edited")
bm = bmesh.from_edit_mesh(ob.data)
check(len(bm.verts) == 8, "the session edits the mesh arriving at the node (8 vertices)")
for v in bm.verts:
    if v.co.z > 0.5:
        v.co.z += 0.5
bmesh.update_edit_mesh(ob.data)
tab()
check(ob.mode == 'OBJECT' and ob.data == base, "Tab leaves: original mesh back")
check([tuple(v.co) for v in base.vertices] == base_co, "base mesh untouched")
check(mod.edit_poly_session_get() == -1, "session ended")
check(mod.edit_poly_has_edits(id=node_id), "edits stored in the Geometry Nodes modifier")
moved = evaluated(ob)
check(moved["max_z"] == 2.5, "the move applies before the nodes after it (top at 1.5 + 1 = 2.5)")
check(evaluated(other)["max_z"] == 2.0 and not other_mod.edit_poly_has_edits(id=node_id),
      "another object using the same node group keeps its own (no) edits")

print("Extrude the top face")
tab()
select_only(ob, lambda f: f.normal.z > 0.5)
bpy.ops.mesh.extrude_region_move(TRANSFORM_OT_translate={"value": (0.0, 0.0, 0.5)})
tab()
ext = evaluated(ob)
check(ext["verts"] == 12 and ext["faces"] == 10 and ext["max_z"] == 3.0,
      f"topology edit stored ({ext['verts']} vertices, {ext['faces']} faces, top at {ext['max_z']})")

print("Save and reload")
path = os.path.join(tempfile.gettempdir(), "edit_poly_node_check.blend")
bpy.ops.wm.save_as_mainfile(filepath=path)
bpy.ops.wm.open_mainfile(filepath=path)
ob = bpy.data.objects["Cube"]
mod = ob.modifiers["GeometryNodes"]
check(evaluated(ob) == ext, "edits survive save and reload")
other = bpy.data.objects["Cube.001"]
check(evaluated(other)["max_z"] == 2.0, "the other object still has no edits after reload")

print("Which node Tab edits")
tree = mod.node_group
first = next(n for n in tree.nodes if n.bl_idname == 'GeometryNodeEditPoly')
transform = next(n for n in tree.nodes if n.bl_idname == 'GeometryNodeTransform')
group_out = next(n for n in tree.nodes if n.bl_idname == 'NodeGroupOutput')
second = tree.nodes.new('GeometryNodeEditPoly')
tree.links.new(transform.outputs[0], second.inputs[0])
tree.links.new(second.outputs[0], group_out.inputs[0])
tree.nodes.active = first
first_id = mod.edit_poly_pick()
tree.nodes.active = second
second_id = mod.edit_poly_pick()
tree.nodes.active = transform
check(first_id != second_id and first_id == node_id, "the active Edit Poly node is picked")
check(mod.edit_poly_pick() == second_id, "otherwise the one closest to the output")
second.mute = True
check(mod.edit_poly_pick() == first_id, "a muted Edit Poly node is skipped")
second.mute = False

print("Edit and Reset buttons")
bpy.context.view_layer.objects.active = ob
bpy.ops.object.edit_poly_node_edit(modifier=mod.name, node_id=second_id)
check(ob.mode == 'EDIT' and mod.edit_poly_session_get() == second_id, "Edit starts a session on that node")
tab()
check(ob.mode == 'OBJECT', "Tab leaves it")
bpy.ops.object.edit_poly_node_reset(modifier=mod.name, node_id=node_id)
check(not mod.edit_poly_has_edits(id=node_id), "Reset removes the stored edits")
check(evaluated(ob)["verts"] == 8 and evaluated(ob)["max_z"] == 2.0, "the node passes the cube through again")

print("Without Edit Poly nodes Tab behaves as before")
bpy.ops.mesh.primitive_cube_add(location=(-4.0, 0.0, 0.0))
plain_ob = bpy.context.active_object
plain_tree = bpy.data.node_groups.new("Plain Tree", 'GeometryNodeTree')
plain_tree.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
plain_tree.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
gi = plain_tree.nodes.new('NodeGroupInput')
go = plain_tree.nodes.new('NodeGroupOutput')
plain_tree.links.new(gi.outputs[0], go.inputs[0])
add_nodes_modifier(plain_ob, plain_tree)
tab()
check(any(m.type == 'EDIT_POLY' for m in plain_ob.modifiers), "an Edit Poly modifier is placed, as before")
tab()
check(plain_ob.mode == 'OBJECT', "Tab leaves that level")

print("Removing the node")
bpy.context.view_layer.objects.active = ob
tab()
bm = bmesh.from_edit_mesh(ob.data)
for v in bm.verts:
    v.co.z += 0.25
bmesh.update_edit_mesh(ob.data)
tab()
check(evaluated(ob)["max_z"] == 2.25, "Tab edits the node closest to the output (moved up 0.25)")
group_in = next(n for n in tree.nodes if n.bl_idname == 'NodeGroupInput')
tree.nodes.remove(first)
tree.links.new(group_in.outputs[0], transform.inputs[0])
info = evaluated(ob)
check(info["verts"] == 8 and info["max_z"] == 2.25,
      "removing the first node: the other node's edits still apply (no crash)")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
for f in failures:
    print("  -", f)
# A non-zero exit code makes the test fail when run by CTest.
sys.exit(1 if failures else 0)
