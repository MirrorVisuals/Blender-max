# Phase 4b check: extrude / inset / bevel inside an Edit Poly level are re-applied when the
# Subdivision level below changes, instead of pausing.
# Run with CHECK_EDIT_POLY_OPS.bat (Blender in background mode).
# Tools are called with undo=True ('EXEC_DEFAULT', True), as the UI does: only then does Blender
# record them in its operator history, which is what the Edit Poly session watches.
import bpy, bmesh

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

def update():
    bpy.context.view_layer.update()

def evaluated(ob):
    update()
    dg = bpy.context.evaluated_depsgraph_get()
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    info = {"verts": len(me.vertices), "faces": len(me.polygons),
            "max_z": max(v.co.z for v in me.vertices),
            "co": sorted(tuple(round(c, 4) for c in v.co) for v in me.vertices)}
    ev.to_mesh_clear()
    return info

def plain(ob, levels):
    ep = ob.modifiers["Edit Poly"]
    sub = ob.modifiers["Subdivision"]
    ep.show_viewport = False
    sub.levels = levels
    info = evaluated(ob)
    ep.show_viewport = True
    return info

def select_faces(ob, pred):
    bpy.ops.mesh.select_mode(type='FACE')
    bpy.ops.mesh.select_all(action='DESELECT')
    bm = bmesh.from_edit_mesh(ob.data)
    n = 0
    for f in bm.faces:
        if pred(f):
            f.select_set(True)
            n += 1
    bm.select_flush_mode()
    bmesh.update_edit_mesh(ob.data)
    update()  # Lets the session see the selection, as a click in the viewport would.
    return n

def new_cube():
    bpy.ops.object.select_all(action='DESELECT') if bpy.context.object else None
    bpy.ops.mesh.primitive_cube_add()
    ob = bpy.context.active_object
    sub = ob.modifiers.new("Subdivision", 'SUBSURF')
    sub.levels = 1
    return ob, sub

top4 = lambda f: f.normal.z > 0.5 and f.calc_center_median().z > 0.5

bpy.ops.wm.read_factory_settings(use_empty=True)

print("Extrude, then change the Subdivision level below")
ob, sub = new_cube()
tab()
ep = ob.modifiers["Edit Poly"]
check(select_faces(ob, top4) == 4, "selected the 4 top faces")
bpy.ops.mesh.extrude_region_move('EXEC_DEFAULT', True, TRANSFORM_OT_translate={"value": (0.0, 0.0, 0.4)})
update()
tab()
check(ep.operations_num == 1, f"one operation logged (operations: {ep.operations_num})")
print("        log:", ep.operations_text.strip()[:120])
lv1 = evaluated(ob)
for level in (2, 3):
    base = plain(ob, level)
    sub.levels = level
    res = evaluated(ob)
    check(res["verts"] > base["verts"] and abs(res["max_z"] - (base["max_z"] + 0.4)) < 0.02,
          f"level {level}: extrusion re-applied ({base['verts']} -> {res['verts']} vertices, "
          f"top {base['max_z']:.3f} -> {res['max_z']:.3f})")
sub.levels = 1
check(evaluated(ob)["co"] == lv1["co"], "back to level 1: exactly the edited result again")

print("Inset")
ob, sub = new_cube()
tab()
ep = ob.modifiers["Edit Poly"]
select_faces(ob, top4)
bpy.ops.mesh.inset('EXEC_DEFAULT', True, thickness=0.1, depth=0.0)
update()
tab()
check(ep.operations_num == 1, "inset logged")
base = plain(ob, 2)
sub.levels = 2
res = evaluated(ob)
check(res["faces"] > base["faces"], f"level 2: inset re-applied ({base['faces']} -> {res['faces']} faces)")

print("Bevel an edge")
ob, sub = new_cube()
tab()
ep = ob.modifiers["Edit Poly"]
bpy.ops.mesh.select_mode(type='EDGE')
bpy.ops.mesh.select_all(action='DESELECT')
bm = bmesh.from_edit_mesh(ob.data)
edge = max(bm.edges, key=lambda e: min(e.verts[0].co.z, e.verts[1].co.z)
           - abs(e.verts[0].co.y) - abs(e.verts[1].co.y))
edge.select_set(True)
bmesh.update_edit_mesh(ob.data)
update()
bpy.ops.mesh.bevel('EXEC_DEFAULT', True, offset=0.05, segments=2)
update()
tab()
check(ep.operations_num == 1, "bevel logged")
base = plain(ob, 2)
sub.levels = 2
res = evaluated(ob)
check(res["faces"] > base["faces"], f"level 2: bevel re-applied ({base['faces']} -> {res['faces']} faces)")

print("A tool that cannot be replayed: the log is dropped, the level pauses as before")
ob, sub = new_cube()
tab()
ep = ob.modifiers["Edit Poly"]
select_faces(ob, top4)
bpy.ops.mesh.extrude_region_move('EXEC_DEFAULT', True, TRANSFORM_OT_translate={"value": (0.0, 0.0, 0.3)})
update()
bpy.ops.mesh.select_all(action='SELECT')
bpy.ops.mesh.subdivide('EXEC_DEFAULT', True)
update()
tab()
check(ep.operations_num == 0 and ep.patch_new_faces > 0, "log dropped, patch kept")
base = plain(ob, 2)
sub.levels = 2
check(evaluated(ob)["verts"] == base["verts"], "level 2: paused (plain result), no crash")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
