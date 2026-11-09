# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Unwrap check: UVs edited in a Tab session are stored by stable ID and follow the mesh below
# (Subdivision added / raised / lowered, Mirror copies); new faces are unwrapped and flagged.
# ./blender.bin --background --factory-startup --python tests/python/bl_modifier_unwrap.py
import sys
import math
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


def f(co):
    """The UVs painted in the sessions: a linear function of the position."""
    return (0.1 * co[0] + 0.05 * co[2] + 0.5, 0.1 * co[1] - 0.03 * co[2] + 0.5)


def evaluated(ob):
    """[(face index, [(position, uv)], is_new)] of the evaluated mesh."""
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    layer = me.uv_layers.active
    flag = me.attributes.get(".unwrap_new")
    out = []
    for p in me.polygons:
        corners = [(tuple(me.vertices[me.loops[c].vertex_index].co), tuple(layer.data[c].uv))
                   for c in p.loop_indices]
        out.append((p.index, corners, bool(flag.data[p.index].value) if flag else False))
    ev.to_mesh_clear()
    return out


def all_match(data, fn=f, faces=None, eps=1e-4):
    for i, corners, _new in data:
        if faces is not None and i not in faces:
            continue
        for co, uv in corners:
            e = fn(co)
            if abs(uv[0] - e[0]) > eps or abs(uv[1] - e[1]) > eps:
                return False
    return True


def paint_session(ob):
    """In an Unwrap session: set every UV to f(position)."""
    bm = bmesh.from_edit_mesh(ob.data)
    uv = bm.loops.layers.uv.active
    for face in bm.faces:
        for loop in face.loops:
            loop[uv].uv = f(loop.vert.co)
    bmesh.update_edit_mesh(ob.data)


def new_object(location=(0, 0, 0)):
    bpy.ops.object.select_all(action='DESELECT')
    bpy.ops.mesh.primitive_cube_add(location=location)
    return bpy.context.active_object


def unwrap_and_paint(ob):
    mod = ob.modifiers.new("Unwrap", 'UNWRAP')
    ob.modifiers.active = mod
    tab()
    check(ob.mode == 'EDIT' and ob.data.name.endswith("(Unwrap)"), "Tab on the Unwrap starts a UV session")
    paint_session(ob)
    tab()
    check(ob.mode == 'OBJECT' and not ob.data.name.endswith("(Unwrap)"), "Tab again: back to the original mesh")
    return mod


bpy.ops.wm.read_factory_settings(use_empty=True)

print("Store and replay")
a = new_object()
mod = unwrap_and_paint(a)
check(mod.faces_num == 6, "6 faces stored")
data = evaluated(a)
check(all_match(data) and not any(n for _, _, n in data), "the stored UVs are on the mesh")

print("Subdivision added below, then raised")
sub = a.modifiers.new("Subdivision", 'SUBSURF')
sub.subdivision_type = 'SIMPLE'
sub.levels = 1
bpy.ops.object.modifier_move_to_index(modifier=sub.name, index=0)
data = evaluated(a)
check(len(data) == 24 and all_match(data) and not any(n for _, _, n in data),
      "level 1: the 24 split faces get UVs interpolated from their stored face")
sub.levels = 2
data = evaluated(a)
check(len(data) == 96 and all_match(data), "level 2: still exact")

print("Unwrapped on the subdivided mesh, then the level changes")
b = new_object(location=(4, 0, 0))
sub_b = b.modifiers.new("Subdivision", 'SUBSURF')
sub_b.subdivision_type = 'SIMPLE'
sub_b.levels = 1
mod_b = unwrap_and_paint(b)
check(mod_b.faces_num == 24, "24 faces stored")
sub_b.levels = 2
data = evaluated(b)
check(len(data) == 96 and all_match(data) and not any(n for _, _, n in data),
      "level raised to 2: UVs interpolated inside the stored level-1 faces")
sub_b.levels = 0
data = evaluated(b)
check(len(data) == 6 and all_match(data) and not any(n for _, _, n in data),
      "level lowered to 0: the corners take the UVs of the stored faces split from them")

print("Unwrapped at level 2, then lowered one level at a time")
e = new_object(location=(8, 0, 0))
sub_e = e.modifiers.new("Subdivision", 'SUBSURF')
sub_e.subdivision_type = 'SIMPLE'
sub_e.levels = 2
mod_e = unwrap_and_paint(e)
check(mod_e.faces_num == 96, "96 faces stored")
sub_e.levels = 1
data = evaluated(e)
check(len(data) == 24 and all_match(data) and not any(n for _, _, n in data), "level 2 -> 1: exact")
sub_e.levels = 0
data = evaluated(e)
check(len(data) == 6 and all_match(data), "level 1 -> 0: exact")
sub_e.levels = 3
data = evaluated(e)
check(len(data) == 384 and all_match(data), "and up to 3: exact")

print("Mirror added below")
c = new_object(location=(0, 6, 0))
for v in c.data.vertices:
    v.co.x += 2.0
mod_c = unwrap_and_paint(c)
mir = c.modifiers.new("Mirror", 'MIRROR')
bpy.ops.object.modifier_move_to_index(modifier=mir.name, index=0)
data = evaluated(c)
copies = {i for i, corners, _ in data if all(co[0] < 0 for co, _ in corners)}
check(len(data) == 12 and len(copies) == 6, "12 faces, 6 of them mirror copies")
check(all_match(data, fn=lambda co: f((-co[0], co[1], co[2])), faces=copies) and
      not any(n for _, _, n in data),
      "mirror copies take the UVs of their source faces")

print("New faces")
d = new_object(location=(0, -6, 0))
mod_d = unwrap_and_paint(d)
bm = bmesh.new()
bm.from_mesh(d.data)
bm.verts.ensure_lookup_table()
top = [v for v in bm.verts if v.co.z > 0.9 and v.co.y > 0.9]          # Top back edge.
v1 = bm.verts.new((top[0].co.x, 2.0, 1.0))
v2 = bm.verts.new((top[1].co.x, 2.0, 1.0))
bm.faces.new((top[0], top[1], v2, v1))                                  # Attached to the cube.
loose = [bm.verts.new(p) for p in ((5, 0, 0), (6, 0, 0), (6, 1, 0), (5, 1, 0))]
bm.faces.new(loose)                                                     # Not attached.
bm.to_mesh(d.data)
bm.free()
d.data.update()
data = evaluated(d)
new = [(i, corners) for i, corners, n in data if n]
check(len(new) == 2, "2 faces without stored UVs are flagged as new")
check(all_match(data, faces={i for i, _, n in data if not n}), "the stored faces keep their UVs")
attached = [corners for i, corners in new if any(abs(co[1] - 2.0) < 1e-4 for co, _ in corners)]
shared = [(co, uv) for co, uv in attached[0] if abs(co[1] - 1.0) < 1e-4] if len(attached) == 1 else []
check(len(shared) == 2 and all(abs(uv[0] - f(co)[0]) < 1e-4 and abs(uv[1] - f(co)[1]) < 1e-4
                               for co, uv in shared),
      "the attached new face shares the UVs of the cube's edge it hangs on")
loose_uv = [uv for i, corners in new if all(co[0] >= 5 for co, _ in corners) for _, uv in corners]
check(len(loose_uv) == 4 and all(math.isfinite(u) and u >= 1.05 - 1e-4 for u, _ in loose_uv),
      "the loose new face is unwrapped beside the 0..1 tile")
mod_d.new_faces = 'KEEP'
data = evaluated(d)
check(all(uv == (0.0, 0.0) for i, corners, n in data if n for _, uv in corners),
      "Keep: new faces keep the UVs from below")
mod_d.new_faces = 'FILL'

print("Editing the new faces")
sync_before = bpy.context.scene.tool_settings.use_uv_select_sync
d.modifiers.active = mod_d
tab()
bm = bmesh.from_edit_mesh(d.data)
selected = [face for face in bm.faces if face.select]
check(len(selected) == 2 and bpy.context.scene.tool_settings.use_uv_select_sync,
      "the session selects the 2 new faces, with UV Sync Selection on")
tab()
check(mod_d.faces_num == 8, "after the session 8 faces are stored")
check(bpy.context.scene.tool_settings.use_uv_select_sync == sync_before, "UV Sync Selection is put back")
data = evaluated(d)
check(not any(n for _, _, n in data), "no new faces any more")

print("Quick buttons")
q = new_object(location=(0, -12, 0))
mod_q = q.modifiers.new("Unwrap", 'UNWRAP')
for method in ('SMART', 'UNWRAP', 'PACK'):
    try:
        result = bpy.ops.object.unwrap_quick(modifier=mod_q.name, method=method)
    except RuntimeError as ex:
        result = {str(ex)}
    check(result == {'FINISHED'} and q.mode == 'OBJECT' and mod_q.faces_num == 6 and
          not q.data.name.endswith("(Unwrap)"), f"{method}: stored without a session left open")
data = evaluated(q)
us = [uv for _, corners, _ in data for _, uv in corners]
check(all(-1e-4 <= u <= 1 + 1e-4 and -1e-4 <= v <= 1 + 1e-4 for u, v in us) and
      len({(round(u, 3), round(v, 3)) for u, v in us}) > 4, "after Pack the UVs are inside the 0..1 tile")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
# A non-zero exit code makes the test fail when run by CTest.
sys.exit(1 if failures else 0)
