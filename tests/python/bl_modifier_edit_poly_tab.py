# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Phase 3 check: Tab edits an Edit Poly level; moves are recorded in the modifier, never in the base mesh.
# ./blender.bin --background --factory-startup --python tests/python/bl_modifier_edit_poly_tab.py
import sys
import bpy
import bmesh

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


def points(ob):
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    attr = me.attributes.get(".stable_id_point")
    ids = [tuple(d.value) for d in attr.data] if attr else [(i, -i) for i in range(len(me.vertices))]
    out = {ids[i]: tuple(v.co) for i, v in enumerate(me.vertices)}
    ev.to_mesh_clear()
    return out


def move_top_vertex(ob, dz):
    """In Edit Mode: move the vertex nearest the top centre by dz."""
    bm = bmesh.from_edit_mesh(ob.data)
    v = max(bm.verts, key=lambda v: v.co.z - abs(v.co.x) - abs(v.co.y))
    v.co.z += dz
    bm.normal_update()
    bmesh.update_edit_mesh(ob.data)
    return v.index


bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_cube_add()
ob = bpy.context.active_object
base = ob.data
base_co = [tuple(v.co) for v in base.vertices]
sub = ob.modifiers.new("Subdivision", 'SUBSURF')
sub.levels = 1

before = points(ob)  # Object Mode, Subdivision level 1, no Edit Poly yet
print("Tab on a cube with only a Subdivision")
tab()
ep = ob.modifiers.get("Edit Poly")
check(ep is not None and ob.modifiers[:].index(ep) == 1, "Tab added an Edit Poly at the top of the stack")
check(ob.mode == 'EDIT', "now in Edit Mode")
check(ob.data != base and len(ob.data.vertices) == 26, "editing the subdivided mesh (26 vertices), not the base")
check(not sub.show_viewport, "Subdivision hidden while editing (its result is in the edited mesh)")

move_top_vertex(ob, 0.5)
tab()
check(ob.mode == 'OBJECT' and ob.data == base, "Tab again: back in Object Mode with the original mesh")
check(sub.show_viewport, "Subdivision visible again")
check(ep.moves_num == 1, "one move recorded in the Edit Poly")
check([tuple(v.co) for v in base.vertices] == base_co, "base mesh is untouched")
check("Cube (Edit Poly)" not in bpy.data.meshes, "temporary mesh removed")
after = points(ob)
moved = [k for k in after if k in before and abs(after[k][2] - before[k][2] - 0.5) < 1e-5]
check(len(moved) == 1, "evaluated result: exactly one point moved up 0.5")
top_id = moved[0] if moved else None

sub.levels = 2
check(top_id in points(ob), "Subdivision level 2: the moved point still exists with the same ID")

print("Edit the level again: existing moves show up and can be changed")
sub.levels = 1
tab()
bm = bmesh.from_edit_mesh(ob.data)
top = max(bm.verts, key=lambda v: v.co.z - abs(v.co.x) - abs(v.co.y))
check(abs(top.co.z - (after[top_id][2])) < 1e-5, "Edit Mode shows the vertex at its moved position")
top.co.z -= 0.5
bm.normal_update()
bmesh.update_edit_mesh(ob.data)
tab()
check(ep.moves_num == 0, "moving it back to the start removes the move")

print("Discard leaves a level without recording")
tab()
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.mesh.subdivide()
bpy.ops.object.edit_poly_session_discard()
check(ob.mode == 'OBJECT' and ob.data == base and ep.moves_num == 0 and ep.patch_new_faces == 0,
      "Discard leaves without recording")

print("Editing a lower level, with the stack above showing the end result")
low = ob.modifiers.new("Edit Poly Low", 'EDIT_POLY')
bpy.ops.object.modifier_move_to_index(modifier=low.name, index=0)
ep.move_set(stable_id=top_id, offset=(0.0, 0.0, 0.5))   # a detail move on the top level again
ob.modifiers.active = low
tab()
check(len(bmesh.from_edit_mesh(ob.data).verts) == 8, "Tab with the low Edit Poly active edits the 8-vertex base level")
check(sub.show_viewport and ep.show_viewport, "Subdivision and the top Edit Poly stay visible above")
bm = bmesh.from_edit_mesh(ob.data)
corner = max(bm.verts, key=lambda v: v.co.x + v.co.y + v.co.z)
corner.co.x += 0.3
bm.normal_update()
bmesh.update_edit_mesh(ob.data)
tab()
check(low.moves_num == 1 and ep.moves_num == 1, "corner move recorded in the low level; top level untouched")
final = points(ob)
check(top_id in final, "the top level's detail move still finds its point after the low edit")
check([tuple(v.co) for v in base.vertices] == base_co, "base mesh is still untouched")

print("Tab rules")
ob.modifiers.active = sub
tab()
check(ob["_edit_poly_modifier"] == low.name, "active = Subdivision: Tab edits the nearest Edit Poly below it")
tab()

print("Switching levels while editing")
tab()                                   # editing the low level (active = low)
check(ob["_edit_poly_modifier"] == low.name, "editing the low level")
ob.modifiers.active = ep                # click the top Edit Poly in the stack
tab()
check(ob.mode == 'EDIT' and ob["_edit_poly_modifier"] == ep.name, "Tab switches straight to the clicked level")
check(len(bmesh.from_edit_mesh(ob.data).verts) == 26, "now editing the subdivided mesh of the top level")
bpy.ops.object.edit_poly_level_edit(modifier=low.name)
check(ob.mode == 'EDIT' and ob["_edit_poly_modifier"] == low.name, "Edit Level button switches back to the low level")
check(low.moves_num == 1 and ep.moves_num == 1, "switching keeps both levels' moves")
tab()
check(ob.mode == 'OBJECT' and ob.data == base, "Tab leaves; original mesh back")

print("Other objects are not affected")
bpy.ops.object.empty_add()
emp = bpy.context.active_object
tab()
check(emp.mode == 'OBJECT', "Tab on an empty does nothing special")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
# A non-zero exit code makes the test fail when run by CTest.
sys.exit(1 if failures else 0)
