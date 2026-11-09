# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Item 2 check: Collapse to Here bakes the stack below into the mesh and keeps stable IDs,
# so the Edit Poly levels above give exactly the same result.
# ./blender.bin --background --factory-startup --python tests/python/bl_modifier_edit_poly_collapse.py
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


def evaluated(ob):
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    co = sorted(tuple(round(c, 4) for c in v.co) for v in me.vertices)
    n = (len(me.vertices), len(me.polygons))
    ev.to_mesh_clear()
    return n, co


def ids_of(ob):
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    a = me.attributes.get(".stable_id_point")
    out = {tuple(d.value): tuple(me.vertices[i].co) for i, d in enumerate(a.data)} if a else {}
    ev.to_mesh_clear()
    return out


bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_cube_add()
ob = bpy.context.active_object

print("Stack: Edit Poly (corner move) > Subdivision > Edit Poly (extrusion + detail move)")
low = ob.modifiers.new("Low", 'EDIT_POLY')
# Corner move on the base level through a session, as a user would.
ob.modifiers.active = low
bpy.ops.object.edit_poly_tab()
bm = bmesh.from_edit_mesh(ob.data)
v = max(bm.verts, key=lambda v: v.co.x + v.co.y + v.co.z)
v.co.x += 0.3
bm.normal_update()
bmesh.update_edit_mesh(ob.data)
bpy.ops.object.edit_poly_tab()
sub = ob.modifiers.new("Subdivision", 'SUBSURF')
sub.levels = 1
top = ob.modifiers.new("Top", 'EDIT_POLY')
ob.modifiers.active = top
bpy.ops.object.edit_poly_tab()
bpy.ops.mesh.select_mode(type='FACE')
bpy.ops.mesh.select_all(action='DESELECT')
bm = bmesh.from_edit_mesh(ob.data)
for f in bm.faces:
    if f.normal.z > 0.5 and f.calc_center_median().z > 0.5:
        f.select_set(True)
bm.select_flush_mode()
bmesh.update_edit_mesh(ob.data)
bpy.context.view_layer.update()
bpy.ops.mesh.extrude_region_move('EXEC_DEFAULT', True, TRANSFORM_OT_translate={"value": (0, 0, 0.4)})
bpy.context.view_layer.update()
bpy.ops.object.edit_poly_tab()
check(low.moves_num == 1 and top.patch_new_faces > 0, "stack built (low move, top extrusion)")
before = evaluated(ob)

print("Collapse to the Subdivision")
r = bpy.ops.object.edit_poly_collapse_to(modifier="Subdivision")
check(r == {'FINISHED'}, "collapse ran")
check([m.name for m in ob.modifiers] == ["Top"], "Low and Subdivision removed, Top kept")
check(len(ob.data.vertices) == 26, f"base mesh is now the subdivided mesh ({len(ob.data.vertices)} vertices)")
check(ob.data.attributes.get(".stable_id_point") is not None, "base mesh carries stable IDs")
check(ob.data.attributes.get(".stable_parent_face") is None and ob.data.attributes.get(".stable_subd_uv") is None,
      "history attributes removed")
after = evaluated(ob)
check(after == before, "result is exactly the same as before the collapse")

print("The level above can still be edited")
bpy.ops.object.edit_poly_tab()
check(ob.mode == 'EDIT' and ob.get("_edit_poly_modifier") == "Top", "Tab edits the Top level")
bpy.ops.object.edit_poly_tab()
check(evaluated(ob) == before, "leaving without changes keeps the result")

print("Collapse is not offered while editing a level")
bpy.ops.object.edit_poly_tab()
check(not bpy.ops.object.edit_poly_collapse_to.poll(), "refused inside a level")
bpy.ops.object.edit_poly_tab()

print("Save and reload")
path = os.path.join(tempfile.gettempdir(), "edit_poly_collapse_check.blend")
bpy.ops.wm.save_as_mainfile(filepath=path)
bpy.ops.wm.open_mainfile(filepath=path)
check(evaluated(bpy.data.objects["Cube"]) == before, "same result after reload")

print("Collapse everything")
ob = bpy.data.objects["Cube"]
bpy.ops.object.select_all(action='DESELECT')
ob.select_set(True)
bpy.context.view_layer.objects.active = ob
bpy.ops.object.edit_poly_collapse_to(modifier="Top")
check(len(ob.modifiers) == 0 and evaluated(ob) == before, "all collapsed, same result")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
# A non-zero exit code makes the test fail when run by CTest.
sys.exit(1 if failures else 0)
