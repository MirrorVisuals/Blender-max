# UVW Map check: projections, gizmo (values, gizmo object, Tab editing, Fit), and Selected Faces
# Only with the face selection passed up from an Edit Poly level.
# Run with CHECK_UVW_MAP.bat (Blender in background mode).
import math
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

def faces_uv(ob):
    """[(corner positions, corner uvs, face normal)] of the evaluated mesh, active UV map."""
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    layer = me.uv_layers.active
    out = []
    for f in me.polygons:
        co = [tuple(me.vertices[me.loops[c].vertex_index].co) for c in f.loop_indices]
        uv = [tuple(layer.data[c].uv) for c in f.loop_indices] if layer else []
        out.append((co, uv, tuple(f.normal)))
    ev.to_mesh_clear()
    return out

def close(a, b, eps=1e-4):
    return abs(a - b) < eps

def new_object(kind, **kw):
    bpy.ops.object.select_all(action='DESELECT')
    getattr(bpy.ops.mesh, "primitive_" + kind + "_add")(**kw)
    return bpy.context.active_object

bpy.ops.wm.read_factory_settings(use_empty=True)

print("Planar")
ob = new_object("cube")
uvw = ob.modifiers.new("UVW Map", 'UVW_MAP')
check(uvw.map_type == 'PLANAR' and uvw.axis == 'Z', "a new UVW Map is Planar, aligned to Z")
data = faces_uv(ob)
check(all(close(u, (c[0] + 1) / 2) and close(v, (c[1] + 1) / 2)
          for co, uv, _ in data for c, (u, v) in zip(co, uv)),
      "planar Z: u = (x+1)/2, v = (y+1)/2 for a gizmo of size 2")
uvw.axis = 'X'
data = faces_uv(ob)
check(all(close(u, (c[1] + 1) / 2) and close(v, (c[2] + 1) / 2)
          for co, uv, _ in data for c, (u, v) in zip(co, uv)),
      "planar X: u from Y, v from Z")
uvw.axis = 'Z'
uvw.tile = (2.0, 1.0, 1.0)
uvw.offset = (0.25, 0.0)
uvw.use_flip_u = True
data = faces_uv(ob)
check(all(close(u, (1 - (c[0] + 1) / 2) * 2 + 0.25) for co, uv, _ in data for c, (u, v) in zip(co, uv)),
      "flip U, tile 2 and offset 0.25")
uvw.tile = (1.0, 1.0, 1.0)
uvw.offset = (0.0, 0.0)
uvw.use_flip_u = False
uvw.gizmo_size = (4.0, 4.0, 4.0)
uvw.gizmo_location = (1.0, 0.0, 0.0)
data = faces_uv(ob)
check(all(close(u, ((c[0] - 1) / 2 + 1) / 2) for co, uv, _ in data for c, (u, v) in zip(co, uv)),
      "gizmo location and size move and scale the map")

print("Box")
uvw.gizmo_size = (2.0, 2.0, 2.0)
uvw.gizmo_location = (0.0, 0.0, 0.0)
uvw.map_type = 'BOX'
data = faces_uv(ob)
square = {(0, 0), (1, 0), (1, 1), (0, 1)}
check(all({(round(u, 4), round(v, 4)) for u, v in uv} == square for _, uv, _ in data),
      "every cube face gets the whole square")

print("Face")
uvw.map_type = 'FACE'
data = faces_uv(ob)
check(all({(round(u, 4), round(v, 4)) for u, v in uv} == square for _, uv, _ in data),
      "every quad gets the whole square")

print("Cylindrical")
cyl = new_object("cylinder", vertices=32, radius=1.0, depth=2.0)
uvw2 = cyl.modifiers.new("UVW Map", 'UVW_MAP')
uvw2.map_type = 'CYLINDRICAL'
data = faces_uv(cyl)
sides = [d for d in data if abs(d[2][2]) < 0.5]
check(len(sides) == 32 and all(max(u for u, _ in uv) - min(u for u, _ in uv) < 0.1 for _, uv, _ in sides),
      "no side face spans the seam")
check(all(close(v, (c[2] + 1) / 2) for co, uv, _ in sides for c, (u, v) in zip(co, uv)),
      "v follows the height")
uvw2.use_cap = True
data = faces_uv(cyl)
caps = [d for d in data if abs(d[2][2]) > 0.9]
check(len(caps) == 2 and all(close(u, (c[0] + 1) / 2) for co, uv, _ in caps for c, (u, v) in zip(co, uv)),
      "with Cap the top and bottom are planar")

print("Spherical / Shrink Wrap")
sph = new_object("uv_sphere", segments=16, ring_count=8, radius=1.0)
uvw3 = sph.modifiers.new("UVW Map", 'UVW_MAP')
uvw3.map_type = 'SPHERICAL'
data = faces_uv(sph)
check(all(math.isfinite(u) and math.isfinite(v) for _, uv, _ in data for u, v in uv) and
      all(max(u for u, _ in uv) - min(u for u, _ in uv) < 0.2 for _, uv, _ in data),
      "spherical: finite, no face spans the seam (poles included)")
uvw3.map_type = 'SHRINK_WRAP'
data = faces_uv(sph)
check(all(-1e-4 <= u <= 1 + 1e-4 and -1e-4 <= v <= 1 + 1e-4 for _, uv, _ in data for u, v in uv),
      "shrink wrap stays inside the square")

print("Gizmo object")
bpy.context.view_layer.objects.active = ob
uvw.map_type = 'PLANAR'
empty = bpy.data.objects.new("Gizmo", None)
bpy.context.collection.objects.link(empty)
empty.scale = (2.0, 2.0, 2.0)
uvw.gizmo_object = empty
data = faces_uv(ob)
check(all(close(u, (c[0] / 2 + 1) / 2) for co, uv, _ in data for c, (u, v) in zip(co, uv)),
      "the gizmo object's scale sizes the map")
empty.location.x = 1.0
data = faces_uv(ob)
check(all(close(u, ((c[0] - 1) / 2 + 1) / 2) for co, uv, _ in data for c, (u, v) in zip(co, uv)),
      "moving the gizmo object moves the map")
uvw.gizmo_object = None
bpy.data.objects.remove(empty)

print("Fit")
bpy.ops.object.select_all(action='DESELECT')
ob.select_set(True)
bpy.context.view_layer.objects.active = ob
for v in ob.data.vertices:
    v.co.x = v.co.x * 3.0 + 1.0
ob.data.update()
bpy.ops.object.uvw_map_fit(modifier=uvw.name)
check(all(close(a, b) for a, b in zip(uvw.gizmo_size, (6, 2, 2))) and
      all(close(a, b) for a, b in zip(uvw.gizmo_location, (1, 0, 0))),
      "Fit: gizmo size (6, 2, 2) at (1, 0, 0)")
data = faces_uv(ob)
us = [u for _, uv, _ in data for u, _ in uv]
check(close(min(us), 0) and close(max(us), 1), "after Fit the map spans the whole square")

print("Edit the gizmo with Tab")
from bl_operators import uvw_map as uvw_tools
ob.modifiers.active = uvw
tab()
giz = bpy.context.active_object
check(giz is not None and giz != ob and uvw_tools.is_gizmo(giz) and uvw.gizmo_object is None,
      "Tab on the UVW Map selects a gizmo helper (not linked, so snapping to the mesh works)")
giz.location.x += 0.5
giz.rotation_euler.z = 0.3
bpy.context.view_layer.update()
check(close(uvw.gizmo_location[0], 1.5) and close(uvw.gizmo_rotation[2], 0.3),
      "moving the helper updates the map right away")
tab()
check(bpy.context.active_object == ob and "Cube UVW Gizmo" not in bpy.data.objects,
      "Tab again: back on the object, helper removed")
check(close(uvw.gizmo_location[0], 1.5) and close(uvw.gizmo_rotation[2], 0.3) and close(uvw.gizmo_size[0], 6),
      "the gizmo move and rotation are stored in the modifier")

print("Alignment resets the rotation")
uvw.gizmo_rotation = (0.4, 0.2, 0.3)
uvw.axis = 'Y'
check(all(close(r, 0) for r in uvw.gizmo_rotation), "picking an alignment turns the gizmo back to that axis")
uvw.axis = 'Z'
uvw.gizmo_rotation = (0.0, 0.0, 0.3)

print("Helpers are put away")
ob.modifiers.active = uvw
tab()
extra = ob.modifiers.new("Temp Map", 'UVW_MAP')
ob.modifiers.active = extra
bpy.ops.object.select_all(action='DESELECT')
ob.select_set(True)
bpy.context.view_layer.objects.active = ob
tab()
temp_helper = bpy.context.active_object
ob.modifiers.remove(extra)
removed = uvw_tools.cleanup()
check(removed == 2 and not any(uvw_tools.is_gizmo(o) for o in bpy.data.objects),
      "helpers of a deleted UVW Map, or no longer selected, are removed")

print("Real-world size (pixel density)")
cube_rw = new_object("cube", location=(0, 8, 0))   # 2 m wide
rw = cube_rw.modifiers.new("UVW Map", 'UVW_MAP')
rw.gizmo_size = (10.0, 10.0, 10.0)                  # The gizmo size must not matter.
rw.use_real_world = True
rw.texture_size = 1024
rw.texel_density = 512.0                            # 1024 px / 512 px/m = 2 m per tile.
data = faces_uv(cube_rw)
us = [u for _, uv, _ in data for u, _ in uv]
check(close(max(us) - min(us), 1.0), "1024 px at 512 px/m: the 2 m cube is one tile wide")
rw.texel_density = 1024.0
data = faces_uv(cube_rw)
us = [u for _, uv, _ in data for u, _ in uv]
check(close(max(us) - min(us), 2.0), "at 1024 px/m it is two tiles wide")
cube_rw.scale = (2.0, 1.0, 1.0)
data = faces_uv(cube_rw)
us = [u for _, uv, _ in data for u, _ in uv]
check(close(max(us) - min(us), 4.0), "the density is in world space: scaling the object x2 gives 4 tiles")

print("Selected faces from an Edit Poly level")
cube = new_object("cube", location=(5, 0, 0))
base_map = cube.modifiers.new("Face Map", 'UVW_MAP')
base_map.map_type = 'FACE'
ep = cube.modifiers.new("Edit Poly", 'EDIT_POLY')
cube.modifiers.active = ep
tab()
check(cube.mode == 'EDIT', "Tab on the Edit Poly edits the level")
bm = bmesh.from_edit_mesh(cube.data)
for f in bm.faces:
    f.select_set(False)
top = max(bm.faces, key=lambda f: f.calc_center_median().z)
top.select_set(True)
bm.select_flush_mode()
bmesh.update_edit_mesh(cube.data)
tab()
check(cube.mode == 'OBJECT' and ep.selected_faces_num == 1, "leaving the level stores 1 selected face")
sel_map = cube.modifiers.new("Top Map", 'UVW_MAP')
sel_map.use_selection = True
sel_map.offset = (5.0, 0.0)
data = faces_uv(cube)
mapped = [d for d in data if min(u for u, _ in d[1]) > 4]
check(len(mapped) == 1 and mapped[0][2][2] > 0.9, "only the selected (top) face is mapped by the UVW Map above")
check(all({(round(u, 4), round(v, 4)) for u, v in d[1]} == square for d in data if d not in mapped),
      "the other faces keep the UVs from below")

cube.modifiers.active = ep
tab()
bm = bmesh.from_edit_mesh(cube.data)
selected = [f for f in bm.faces if f.select]
check(len(selected) == 1 and selected[0].normal.z > 0.9, "entering the level again shows its selection")
tab()

sub = cube.modifiers.new("Subdivision", 'SUBSURF')
sub.levels = 1
bpy.ops.object.modifier_move_to_index(modifier=sub.name, index=0)
data = faces_uv(cube)
mapped = [d for d in data if min(u for u, _ in d[1]) > 4]
check(len(mapped) == 4, "Subdivision added below: the 4 faces split from the selected face are mapped")

ep.use_selection = False
data = faces_uv(cube)
mapped = [d for d in data if min(u for u, _ in d[1]) > 4]
check(len(mapped) == len(data),
      "the level can stop passing its selection: the base mesh's (all faces) is used again")

print("Size modes")
cube_s = new_object("cube", location=(0, 12, 0))   # 2 m wide
sm = cube_s.modifiers.new("UVW Map", 'UVW_MAP')
sm.gizmo_size = (10.0, 10.0, 10.0)                   # The gizmo size must not matter.
def spans(ob):
    data = faces_uv(ob)
    top = [d for d in data if d[2][2] > 0.9][0]
    us = [u for u, _ in top[1]]
    vs = [v for _, v in top[1]]
    return max(us) - min(us), max(vs) - min(vs)
check(sm.size_mode == 'GIZMO', "the default size is the gizmo")
sm.size_mode = 'REAL_WORLD'
sm.real_size = (1.0, 1.0)
su, sv = spans(cube_s)
check(close(su, 2.0) and close(sv, 2.0), f"Real-World Size 1 x 1 m: a 2 m face is 2 x 2 tiles ({su:.3f} x {sv:.3f})")
sm.real_size = (2.0, 0.5)
su, sv = spans(cube_s)
check(close(su, 1.0) and close(sv, 4.0), f"2 m x 0.5 m tile: 1 x 4 tiles ({su:.3f} x {sv:.3f})")
sm.size_mode = 'PIXEL_DENSITY'
sm.texture_size_preset = '1K'
sm.texel_density_preset = '512'
check(sm.texture_size == 1024 and close(sm.texel_density, 512), "the presets set 1024 px and 512 px/m")
su, sv = spans(cube_s)
check(close(su, 1.0), f"1K at 512 px/m: a 2 m face is one tile ({su:.3f})")
sm.texel_density = 300.0
check(sm.texel_density_preset == 'CUSTOM', "a density that is not a preset shows as Custom")

print("Material")
cube_m = new_object("cube", location=(0, 16, 0))
for name in ("A", "B"):
    cube_m.data.materials.append(bpy.data.materials.new(name))
top = max(cube_m.data.polygons, key=lambda p: p.center.z)
top.material_index = 1
mm = cube_m.modifiers.new("UVW Map", 'UVW_MAP')
mm.offset = (5.0, 0.0)
mm.use_material = True
mm.material_id = 2
check(mm.material_index == 1, "material ID 2 is the second slot")
data = faces_uv(cube_m)
mapped = [d for d in data if min(u for u, _ in d[1]) > 4]
check(len(mapped) == 1 and mapped[0][2][2] > 0.9, "only the face with material B is mapped")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
