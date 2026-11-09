# Extrude Manifold (solid) check: on a closed mesh, pushing faces in cuts the volume they sweep
# (clean walls, a hole when it goes through) and pulling out adds it; an open mesh, or Solid
# turned off, extrudes as Blender's Extrude Manifold always did; other parts of the mesh are
# left alone.
# Run with CHECK_EXTRUDE_SOLID.bat (Blender in background mode).
import bmesh
import bpy
from mathutils import Vector

failures = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene


def make_object(name, build):
    me = bpy.data.meshes.new(name)
    bm = bmesh.new()
    build(bm)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    scene.collection.objects.link(ob)
    for o in scene.objects:
        o.select_set(False)
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    return ob


def grid_cube(bm):
    # A 2 x 2 x 2 cube cut into 3 x 3 x 3 (faces 2/3 wide), like a subdivided box.
    bmesh.ops.create_cube(bm, size=2.0)
    bmesh.ops.subdivide_edges(bm, edges=bm.edges[:], cuts=2, use_grid_fill=True)


def grid_plane(bm):
    bmesh.ops.create_grid(bm, x_segments=4, y_segments=4, size=1.0)


def run(name, build, pick, move, solid=True, stock=False):
    ob = make_object(name, build)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_mode(type='FACE')
    bm = bmesh.from_edit_mesh(ob.data)
    if pick == 'NEAREST_CENTER':
        nearest = min(bm.faces, key=lambda f: center(f).length)
        pick = lambda f: f == nearest
    for f in bm.faces:
        f.select_set(pick(f))
    bm.select_flush_mode()
    bmesh.update_edit_mesh(ob.data)
    picked = sum(1 for f in bm.faces if f.select)
    faces_before = len(bm.faces)
    if stock:
        # Blender's original Extrude Manifold steps.
        bpy.ops.mesh.extrude_region(use_dissolve_ortho_edges=True)
        bpy.ops.transform.translate(value=move, use_automerge_and_split=True)
    else:
        bpy.ops.mesh.extrude_manifold(MESH_OT_extrude_manifold_region={"use_solid": solid},
                                      TRANSFORM_OT_translate={"value": move})
    bm = bmesh.from_edit_mesh(ob.data)
    selected = [f for f in bm.faces if f.select]
    # Faces pointing the wrong way: compare with a copy whose directions are recalculated.
    probe = bm.copy()
    before_no = [f.normal.copy() for f in probe.faces]
    bmesh.ops.recalc_face_normals(probe, faces=probe.faces[:])
    probe.normal_update()
    wrong_way = sum(1 for f, n in zip(probe.faces, before_no) if f.normal.dot(n) < 0)
    probe.free()
    info = dict(
        picked=picked,
        faces_before=faces_before,
        faces=len(bm.faces),
        verts=len(bm.verts),
        edges=len(bm.edges),
        open_edges=sum(1 for e in bm.edges if not e.is_manifold),
        euler=len(bm.verts) - len(bm.edges) + len(bm.faces),
        volume=bm.calc_volume(signed=True),
        selected=len(selected),
        wrong_way=wrong_way,
        selected_center=(sum((f.calc_center_median() for f in selected), Vector()) / len(selected)
                         if selected else None),
        layers=[layer.name for layer in bm.verts.layers.float_vector],
    )
    bpy.ops.object.mode_set(mode='OBJECT')
    return info


def center(f):
    return f.calc_center_median()


corner = lambda f: f.normal.z > 0.9 and center(f).x > 0.5 and center(f).y > 0.5
edge_row = lambda f: f.normal.z > 0.9 and center(f).x > 0.5
side = lambda f: f.normal.x > 0.9 and abs(center(f).y) < 0.1 and abs(center(f).z) < 0.1
cell = 2.0 / 3.0


def solid_case(title, pick, move, volume, euler=2):
    r = run(title, grid_cube, pick, move)
    print(f"{title}: {r['picked']} face(s), moved {tuple(round(c, 3) for c in move)}")
    check(r['open_edges'] == 0, f"  the result is closed (open edges: {r['open_edges']})")
    check(abs(r['volume'] - volume) < 1e-3,
          f"  volume {r['volume']:.4f} (expected {volume:.4f})")
    check(r['euler'] == euler,
          f"  {'a hole goes through' if euler == 0 else 'no hole'} (V-E+F = {r['euler']})")
    check(r['wrong_way'] == 0, f"  every face points outward ({r['wrong_way']} inward)")
    check(not any(n.startswith(".extrude_solid") for n in r['layers']),
          "  the temporary position attribute is removed")
    return r


print("Pushing in")
r = solid_case("Corner face down one row", corner, (0, 0, -cell), 8 - cell ** 3)
check(r['faces'] == 54, f"  walls against the faces below disappear ({r['faces']} faces, expected 54)")
check(r['selected'] >= 1 and r['selected_center'] is not None and
      abs(r['selected_center'].z - (1 - cell)) < 1e-3,
      "  the pushed face stays selected at its new height")
solid_case("Corner face down part of a row", corner, (0, 0, -0.4), 8 - cell * cell * 0.4)
solid_case("Edge row down two rows", edge_row, (0, 0, -2 * cell), 8 - 2 * cell * 2 * cell)
solid_case("Side face through the whole cube", side, (-2.5, 0, 0), 8 - cell * cell * 2, euler=0)

print("Pulling out")
solid_case("Side face out", side, (1.0, 0, 0), 8 + cell * cell)
solid_case("Corner face up", corner, (0, 0, 1.0), 8 + cell * cell)

print("Solid turned off")
partial_off = None
for title, move in (("Corner face down part of a row", (0, 0, -0.4)),
                    ("Corner face down one row", (0, 0, -cell))):
    off = run(title + ", Solid off", grid_cube, corner, move, solid=False)
    partial_off = partial_off or off
    old = run(title + ", original steps", grid_cube, corner, move, stock=True)
    same = (off['verts'], off['edges'], off['faces']) == (old['verts'], old['edges'], old['faces'])
    check(same and abs(off['volume'] - old['volume']) < 1e-4,
          f"{title}: same as Blender's original Extrude Manifold "
          f"({off['verts']}/{off['edges']}/{off['faces']} against "
          f"{old['verts']}/{old['edges']}/{old['faces']} vertices/edges/faces)")
on = run("Corner face down part of a row, Solid on", grid_cube, corner, (0, 0, -0.4))
off = partial_off
check((on['verts'], on['faces']) != (off['verts'], off['faces']),
      f"Solid on gives a different result there ({on['verts']} vertices, {on['faces']} faces)")

print("Other parts of the mesh")
def cube_and_far_cube(bm):
    grid_cube(bm)
    far = bmesh.new()
    grid_cube(far)
    bmesh.ops.translate(far, verts=far.verts[:], vec=(10, 0, 0))
    me = bpy.data.meshes.new("far")
    far.to_mesh(me)
    far.free()
    bm.from_mesh(me)
    bpy.data.meshes.remove(me)
r = run("Corner pushed, with a second cube in the mesh", cube_and_far_cube,
        lambda f: corner(f) and center(f).x < 5, (0, 0, -cell))
check(r['open_edges'] == 0, f"the result is closed (open edges: {r['open_edges']})")
check(abs(r['volume'] - (8 - cell ** 3 + 8)) < 1e-3,
      f"volume {r['volume']:.4f} (expected {8 - cell ** 3 + 8:.4f}: the cut, and the other cube whole)")
check(r['faces'] == 54 + 54, f"{r['faces']} faces (expected 54 + 54: the other cube is unchanged)")
check(r['wrong_way'] == 0, f"every face points outward ({r['wrong_way']} inward)")

print("Open mesh")
r = run("Plane pushed down", grid_plane, 'NEAREST_CENTER', (0, 0, -0.5))
check(r['picked'] == 1 and r['faces'] == r['faces_before'] + 4,
      f"keeps the normal extrude ({r['faces_before']} faces + 4 walls = {r['faces']})")
check(not any(n.startswith(".extrude_solid") for n in r['layers']),
      "the temporary position attribute is removed")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
