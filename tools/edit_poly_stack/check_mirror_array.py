# Item 3 check: Mirror and Array give stable IDs, so Edit Poly levels above them keep their
# edits when the mirror axes or the array count change.
# Run with CHECK_MIRROR_ARRAY.bat (Blender in background mode).
import bpy, bmesh

failures = []
def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)

def points(ob):
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    a = me.attributes.get(".stable_id_point")
    ids = [tuple(d.value) for d in a.data] if a else []
    out = {ids[i]: tuple(v.co) for i, v in enumerate(me.vertices)} if a else {}
    n = len(me.vertices)
    ev.to_mesh_clear()
    return out, len(ids), n

def unique_ok(ob, label):
    pts, nids, n = points(ob)
    check(nids == n and len(pts) == n, f"{label}: every vertex has a unique ID ({n} vertices)")
    return pts

def moved_ok(ob, edit, sid, expected, label):
    edit.show_viewport = False
    before, _, _ = points(ob)
    edit.show_viewport = True
    after, _, _ = points(ob)
    ok = sid in before and sid in after and all(
        abs(after[sid][k] - before[sid][k] - expected[k]) < 1e-5 for k in range(3))
    check(ok, label)

bpy.ops.wm.read_factory_settings(use_empty=True)

print("Mirror")
bpy.ops.mesh.primitive_cube_add()
ob = bpy.context.active_object
for v in ob.data.vertices:
    v.co.x += 2.0          # Cube away from the mirror plane: the mirror makes a second cube.
mir = ob.modifiers.new("Mirror", 'MIRROR')
pts = unique_ok(ob, "mirror X")
ep = ob.modifiers.new("Edit Poly", 'EDIT_POLY')
# A corner of the mirrored cube (x < 0).
ep.show_viewport = False
pts, _, _ = points(ob)
ep.show_viewport = True
corner = max((k for k in pts if pts[k][0] < 0), key=lambda k: -pts[k][0] + pts[k][1] + pts[k][2])
ep.move_set(stable_id=corner, offset=(0.0, 0.0, 0.5))
moved_ok(ob, ep, corner, (0, 0, 0.5), "a vertex of the mirrored half is moved")
mir.use_axis[1] = True
unique_ok(ob, "mirror X + Y")
moved_ok(ob, ep, corner, (0, 0, 0.5), "adding the Y axis keeps the move on the same vertex")
mir.use_axis[2] = True
moved_ok(ob, ep, corner, (0, 0, 0.5), "adding the Z axis keeps it too")
mir.use_axis[1] = False
mir.use_axis[2] = False
moved_ok(ob, ep, corner, (0, 0, 0.5), "back to X only: still the same vertex")

print("Mirror with merging on the mirror plane")
bpy.ops.object.select_all(action='DESELECT')
bpy.ops.mesh.primitive_cube_add(location=(0, 6, 0))
ob2 = bpy.context.active_object
ob2.modifiers.new("Mirror", 'MIRROR')          # Cube centred on the plane: halves merge.
unique_ok(ob2, "merged mirror")

print("Array")
bpy.ops.object.select_all(action='DESELECT')
bpy.ops.mesh.primitive_cube_add(location=(0, -6, 0))
ob3 = bpy.context.active_object
arr = ob3.modifiers.new("Array", 'ARRAY')
arr.count = 3
arr.relative_offset_displace = (1.5, 0, 0)  # Gaps between copies, nothing merges.
unique_ok(ob3, "array of 3")
ep3 = ob3.modifiers.new("Edit Poly", 'EDIT_POLY')
ep3.show_viewport = False
pts, _, _ = points(ob3)
ep3.show_viewport = True
third = max(pts, key=lambda k: pts[k][0] + pts[k][2])      # Top corner of the third copy.
ep3.move_set(stable_id=third, offset=(0.0, 0.0, 0.7))
moved_ok(ob3, ep3, third, (0, 0, 0.7), "a vertex of the third copy is moved")
arr.count = 6
unique_ok(ob3, "array of 6")
moved_ok(ob3, ep3, third, (0, 0, 0.7), "count 6: the move stays on the third copy")
arr.count = 2
pts2, _, _ = points(ob3)
check(third not in pts2, "count 2: the third copy is gone, the move is paused (no crash)")
arr.count = 3
moved_ok(ob3, ep3, third, (0, 0, 0.7), "count 3 again: the move is back")

print("Array with merging and caps")
arr.relative_offset_displace = (1.0, 0, 0)
arr.use_merge_vertices = True
unique_ok(ob3, "merged array")
bpy.ops.mesh.primitive_uv_sphere_add(location=(0, -12, 0))
cap = bpy.context.active_object
arr.start_cap = cap
arr.end_cap = cap
unique_ok(ob3, "array with start and end caps")

print("Topology edit above a Mirror survives a new axis")
bpy.ops.object.select_all(action='DESELECT')
ob.select_set(True)
bpy.context.view_layer.objects.active = ob
ep.moves_clear()
ob.modifiers.active = ep
bpy.ops.object.edit_poly_tab()
bpy.ops.mesh.select_mode(type='FACE'); bpy.ops.mesh.select_all(action='DESELECT')
bm = bmesh.from_edit_mesh(ob.data)
f = max(bm.faces, key=lambda f: -f.calc_center_median().x + 3 * f.normal.z)  # top of mirrored cube
f.select_set(True)
bm.select_flush_mode(); bmesh.update_edit_mesh(ob.data); bpy.context.view_layer.update()
bpy.ops.mesh.extrude_region_move('EXEC_DEFAULT', True, TRANSFORM_OT_translate={"value": (0, 0, 0.6)})
bpy.context.view_layer.update()
bpy.ops.object.edit_poly_tab()
_, _, n_x = points(ob)
mir.use_axis[1] = True
_, _, n_xy = points(ob)
ep.show_viewport = False
_, _, plain_xy = points(ob)
ep.show_viewport = True
check(n_xy == plain_xy + (n_x - 16), f"the extrusion is still there with X + Y ({plain_xy} + {n_x - 16} vertices)")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
