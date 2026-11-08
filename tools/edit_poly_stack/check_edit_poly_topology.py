# Phase 4a check: topology edits (any tool) are stored as a patch in the Edit Poly level.
# Run with CHECK_EDIT_POLY_TOPOLOGY.bat (Blender in background mode).
import bpy, bmesh, os, tempfile

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
            "max_z": max(v.co.z for v in me.vertices), "max_x": max(v.co.x for v in me.vertices),
            "co": sorted(tuple(round(c, 4) for c in v.co) for v in me.vertices)}
    ev.to_mesh_clear()
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
    bm.normal_update()
    bmesh.update_edit_mesh(ob.data)
    return n

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_cube_add()
ob = bpy.context.active_object
base = ob.data
base_co = [tuple(v.co) for v in base.vertices]
sub = ob.modifiers.new("Subdivision", 'SUBSURF')
sub.levels = 1
plain = evaluated(ob)

print("Extrude the top of the subdivided cube")
tab()
ep = ob.modifiers["Edit Poly"]
n = select_faces(ob, lambda f: f.normal.z > 0.5 and f.calc_center_median().z > 0.5)
check(n == 4, "selected the 4 top faces of the level 1 cube")
bpy.ops.mesh.extrude_region_move(TRANSFORM_OT_translate={"value": (0.0, 0.0, 0.5)})
bm = bmesh.from_edit_mesh(ob.data)
edited_verts = len(bm.verts)
edited_faces = len(bm.faces)
tab()
check(ob.mode == 'OBJECT' and ob.data == base, "Tab leaves: original mesh back")
check([tuple(v.co) for v in base.vertices] == base_co, "base mesh untouched")
check(ep.patch_new_faces > 0 and ep.patch_new_vertices > 0,
      f"patch stored ({ep.patch_new_vertices} new vertices, {ep.patch_new_faces} new faces)")
ext = evaluated(ob)
check(ext["verts"] == edited_verts and ext["faces"] == edited_faces,
      f"result has the edited topology ({ext['verts']} vertices, {ext['faces']} faces)")
check(abs(ext["max_z"] - (plain["max_z"] + 0.5)) < 1e-4, "top is 0.5 higher")
for m in bpy.data.meshes:
    check(not m.name.endswith("(Edit Poly)") and not m.name.endswith("(Edit Poly input)"),
          f"no session mesh left behind ({m.name})")

print("Edit the level again: the extrusion is there and can be changed")
tab()
bm = bmesh.from_edit_mesh(ob.data)
check(len(bm.verts) == edited_verts, "session shows the extruded mesh")
top = max(bm.verts, key=lambda v: v.co.z - abs(v.co.x) - abs(v.co.y))
top.co.z += 0.25
bm.normal_update()
bmesh.update_edit_mesh(ob.data)
tab()
again = evaluated(ob)
check(again["verts"] == edited_verts and abs(again["max_z"] - (ext["max_z"] + 0.25)) < 1e-4,
      "moving a new vertex updates the patch, topology unchanged")

print("New geometry follows when the mesh below is deformed")
for v in base.vertices:
    v.co.x += 1.0
base.update()
moved = evaluated(ob)
check(abs(moved["max_x"] - (again["max_x"] + 1.0)) < 1e-4 and moved["verts"] == edited_verts,
      "everything, including the extrusion, shifted +1 in X")
for v in base.vertices:
    v.co.x -= 1.0
base.update()

print("Changing the Subdivision level below pauses the patch")
sub.levels = 2
lv2 = evaluated(ob)
ob.modifiers["Edit Poly"].show_viewport = False
lv2_plain = evaluated(ob)
ob.modifiers["Edit Poly"].show_viewport = True
check(lv2["verts"] == lv2_plain["verts"], "level 2: patch paused (plain level 2 result, no crash)")
result = tab()
check(result == {'CANCELLED'} and ob.mode == 'OBJECT' and ob.data == base,
      "Tab refuses to edit a paused level and leaves everything as it was")
check(all(m.show_viewport for m in ob.modifiers), "all modifiers visible after the refusal")
sub.levels = 1
check(evaluated(ob)["verts"] == edited_verts, "back to level 1: the extrusion is back")

print("Delete a face")
tab()
before_faces = len(bmesh.from_edit_mesh(ob.data).faces)
n = select_faces(ob, lambda f: f.normal.x > 0.5 and abs(f.calc_center_median().z) < 0.3
                 and f.calc_center_median().y > 0)
check(n >= 1, f"selected {n} side face(s)")
bpy.ops.mesh.delete(type='ONLY_FACE')
deleted = before_faces - len(bmesh.from_edit_mesh(ob.data).faces)
tab()
check(evaluated(ob)["faces"] == before_faces - deleted, f"result has {deleted} face(s) fewer")

print("Several tools in one session: inset and bevel")
tab()
select_faces(ob, lambda f: f.normal.y < -0.5 and f.calc_center_median().z > 0.2)
bpy.ops.mesh.inset(thickness=0.05)
bpy.ops.mesh.select_mode(type='EDGE')
bpy.ops.mesh.select_all(action='DESELECT')
bm = bmesh.from_edit_mesh(ob.data)
e = max(bm.edges, key=lambda e: (e.verts[0].co.z + e.verts[1].co.z) - abs(e.verts[0].co.x - e.verts[1].co.x))
e.select_set(True)
bm.normal_update()
bmesh.update_edit_mesh(ob.data)
bpy.ops.mesh.bevel(offset=0.03, segments=2)
bm = bmesh.from_edit_mesh(ob.data)
multi_verts = len(bm.verts)
multi = sorted(tuple(round(c, 4) for c in v.co) for v in bm.verts)
tab()
res = evaluated(ob)
check(res["verts"] == multi_verts, "inset + bevel recorded")
check(res["co"] == multi, "result matches the edited mesh exactly")

print("Save and reload")
path = os.path.join(tempfile.gettempdir(), "edit_poly_topology_check.blend")
bpy.ops.wm.save_as_mainfile(filepath=path)
bpy.ops.wm.open_mainfile(filepath=path)
ob = bpy.data.objects["Cube"]
check(evaluated(ob)["co"] == multi, "after reload the result is identical")

print("Clearing")
ob.modifiers["Edit Poly"].patch_clear()
ob.modifiers["Edit Poly"].moves_clear()
check(evaluated(ob)["co"] == plain["co"], "patch_clear + moves_clear give the plain subdivided cube")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
