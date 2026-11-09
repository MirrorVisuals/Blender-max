# Edit Poly stack benchmark: builds big scenes in background Blender and times our features.
# Run with BENCHMARK.bat. Results are printed and appended to benchmark_results.txt.
#
# For each feature it measures how long the scene takes to evaluate WITH the modifier and
# WITHOUT it (same scene, modifier hidden), so the difference is the modifier's own cost.
# Note: developer builds have extra checks and are slower than a normal build.
import bpy, bmesh, os, subprocess, sys, time, datetime, statistics

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = os.path.join(HERE, "benchmark_results.txt")
QUICK = "--quick" in sys.argv
SCALE = 0.5 if QUICK else 1.0
rows = []

def ms(t0):
    return (time.perf_counter() - t0) * 1000.0

def update():
    bpy.context.view_layer.update()

def grid(cuts, name):
    """A flat grid with about cuts x cuts faces."""
    cuts = max(2, int(cuts * SCALE))
    bpy.ops.object.select_all(action='DESELECT')
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=cuts, y_subdivisions=cuts, size=10)
    ob = bpy.context.active_object
    ob.name = name
    return ob

def driver(ob):
    """A cheap Displace at the bottom of the stack: changing it forces the stack to re-evaluate."""
    d = ob.modifiers.new("Driver", 'DISPLACE')
    bpy.ops.object.modifier_move_to_index(modifier=d.name, index=0)
    return d

def evaluate(objects):
    """Evaluate and read the result: Blender delays some work (e.g. a Subdivision at the end
    of the stack) until the mesh is read, so reading it is part of the time."""
    update()
    dg = bpy.context.evaluated_depsgraph_get()
    return sum(len(o.evaluated_get(dg).data.polygons) for o in objects)

def eval_median(ob, drv, repeats=5):
    times = []
    for i in range(repeats):
        drv.strength = 0.001 * (i + 1)
        t0 = time.perf_counter()
        evaluate([ob])
        times.append(ms(t0))
    return statistics.median(times)

def faces_of(ob):
    dg = bpy.context.evaluated_depsgraph_get()
    return len(ob.evaluated_get(dg).data.polygons)

def overhead(label, ob, drv, mods):
    """Evaluation time with and without `mods` (hidden), and the difference."""
    with_ms = eval_median(ob, drv)
    n = faces_of(ob)
    for m in mods:
        m.show_viewport = False
    without_ms = eval_median(ob, drv)
    for m in mods:
        m.show_viewport = True
    update()
    extra = max(with_ms - without_ms, 0.0)
    rows.append((label, n, with_ms, extra))
    print(f"  {label:<46} {n:>9,} faces  total {with_ms:9.1f} ms   cost {extra:9.1f} ms")

def single(label, n, t):
    rows.append((label, n, t, None))
    print(f"  {label:<46} {n:>9,} faces  time  {t:9.1f} ms")

def tab():
    t0 = time.perf_counter()
    bpy.ops.object.edit_poly_tab()
    return ms(t0)

def fresh():
    bpy.ops.wm.read_factory_settings(use_empty=True)

print("\nEdit Poly stack benchmark" + (" (quick)" if QUICK else ""))
print("-" * 100)

# 1. Subdivision with stable IDs ------------------------------------------------------------
fresh()
ob = grid(160, "Subdiv")
drv = driver(ob)
sub = ob.modifiers.new("Subdivision", 'SUBSURF')
sub.levels = 2
update()
overhead("Subdivision level 2 (incl. stable IDs)", ob, drv, [sub])

# 2. Edit Poly: Tab sessions, moves and a topology patch on a big mesh ------------------------
fresh()
ob = grid(500, "EditPoly")
n = len(ob.data.polygons)
t_in = tab()
bm = bmesh.from_edit_mesh(ob.data)
bm.verts.ensure_lookup_table()
step = max(1, len(bm.verts) // 1000)
for v in list(bm.verts)[::step]:
    v.co.z += 0.01
bm.normal_update()
bmesh.update_edit_mesh(ob.data)
t_out = tab()
single("Tab into an Edit Poly level", n, t_in)
single("Tab out, storing ~1000 moves", n, t_out)
ep = ob.modifiers["Edit Poly"]
tab()
bm = bmesh.from_edit_mesh(ob.data)
bm.faces.ensure_lookup_table()
bmesh.ops.delete(bm, geom=[bm.faces[i] for i in range(0, len(bm.faces), max(1, len(bm.faces) // 200))], context='FACES')
bmesh.update_edit_mesh(ob.data)
t_out2 = tab()
single("Tab out, storing a patch (~200 deleted faces)", n, t_out2)
drv = driver(ob)
update()
overhead("Edit Poly replay (moves + patch)", ob, drv, [ep])

# 3. Edit Poly element data and selection --------------------------------------------------
tab()
bm = bmesh.from_edit_mesh(ob.data)
bm.edges.ensure_lookup_table()
bm.faces.ensure_lookup_table()
bw = bm.edges.layers.float.get("bevel_weight_edge") or bm.edges.layers.float.new("bevel_weight_edge")
for e in list(bm.edges)[::20]:
    e[bw] = 1.0
for f in list(bm.faces)[::10]:
    f.select_set(True)
bmesh.update_edit_mesh(ob.data)
t_out3 = tab()
single("Tab out, storing bevel weights + selection", n, t_out3)
overhead("Edit Poly replay (+ weights, selection)", ob, drv, [ep])

# 4. UVW Map on a very big mesh ---------------------------------------------------------------
fresh()
ob = grid(1000, "UVW")
drv = driver(ob)
uvw = ob.modifiers.new("UVW Map", 'UVW_MAP')
uvw.map_type = 'BOX'
update()
overhead("UVW Map, Box", ob, drv, [uvw])
uvw.map_type = 'CYLINDRICAL'
overhead("UVW Map, Cylindrical", ob, drv, [uvw])
uvw.size_mode = 'REAL_WORLD'
overhead("UVW Map, Cylindrical, Real-World Size", ob, drv, [uvw])

# 5. Unwrap: replaying stored UVs, and following a Subdivision ----------------------------------
fresh()
ob = grid(400, "Unwrap")
n = len(ob.data.polygons)
unw = ob.modifiers.new("Unwrap", 'UNWRAP')
ob.modifiers.active = unw
t_in = tab()
t_out = tab()
single("Unwrap: Tab in + out (stores every face)", n, t_in + t_out)
drv = driver(ob)
update()
overhead("Unwrap replay (stored UVs by ID)", ob, drv, [unw])
sub = ob.modifiers.new("Subdivision", 'SUBSURF')
sub.subdivision_type = 'SIMPLE'
sub.levels = 1
bpy.ops.object.modifier_move_to_index(modifier=sub.name, index=1)
update()
overhead("Unwrap after Subdivision (interpolated)", ob, drv, [unw])

# 6. Mirror and Array with stable IDs -------------------------------------------------------------
fresh()
ob = grid(120, "Array")
drv = driver(ob)
arr = ob.modifiers.new("Array", 'ARRAY')
arr.count = 40
mir = ob.modifiers.new("Mirror", 'MIRROR')
update()
overhead("Array x40 + Mirror (incl. stable IDs)", ob, drv, [arr, mir])

# 7. Many objects with a full stack ----------------------------------------------------------------
fresh()
count = int(400 * SCALE)
objects = []
t0 = time.perf_counter()
for i in range(count):
    bpy.ops.mesh.primitive_cube_add(location=((i % 20) * 3, (i // 20) * 3, 0))
    o = bpy.context.active_object
    s = o.modifiers.new("Subdivision", 'SUBSURF')
    s.levels = 2
    o.modifiers.new("Edit Poly", 'EDIT_POLY')
    o.modifiers.new("UVW Map", 'UVW_MAP')
    o.modifiers.new("Unwrap", 'UNWRAP')
    # Blender computes a Subdivision that is the last modifier in a slower, delayed way; a
    # do-nothing modifier at the end keeps both measurements doing the same Subdivision work.
    tail = o.modifiers.new("Tail", 'DISPLACE')
    tail.strength = 1e-6   # Not 0: Blender skips a Displace that does nothing.
    objects.append(o)
build_ms = ms(t0)
evaluate(objects)
def reevaluate_all():
    """Make every object evaluate its whole stack again, and time it."""
    for o in objects:
        o.update_tag(refresh={'DATA'})
    t0 = time.perf_counter()
    n = evaluate(objects)
    return ms(t0), n
runs = [reevaluate_all() for _ in range(3)]
first = statistics.median(t for t, _ in runs)
faces = runs[0][1]
single(f"{count} objects: evaluation (full stacks)", faces, first)
for o in objects:
    for m in o.modifiers[1:4]:   # Edit Poly, UVW Map, Unwrap (Subdivision and Tail stay)
        m.show_viewport = False
# Hiding modifiers makes Blender rebuild its update structure for every object: do that untimed.
evaluate(objects)
plain = statistics.median(reevaluate_all()[0] for _ in range(3))
single(f"{count} objects: Subdivision only", faces, plain)
rows.append((f"{count} objects: cost of our modifiers", faces, first, max(first - plain, 0.0)))
print(f"  {count} objects: cost of our modifiers             {max(first - plain, 0.0):9.1f} ms")

# 8. Extrude Solid (edit mode), against Blender's normal extrude ----------------------------------
# Each case is built fresh, the extrude is timed (both run the same extrude and move; Extrude
# Solid adds the Boolean on top), the median of 3 runs is reported.
def solid_box(bm, cuts):
    """A 2 x 2 x 2 box cut into (cuts + 1)^3 cells: 6 (cuts + 1)^2 faces."""
    geom = bmesh.ops.create_cube(bm, size=2.0)
    edges = list({e for v in geom['verts'] for e in v.link_edges})
    bmesh.ops.subdivide_edges(bm, edges=edges, cuts=cuts, use_grid_fill=True)

def extrude_case(build, pick, move, solid, repeats=3):
    times, faces = [], 0
    for _ in range(repeats):
        fresh()
        me = bpy.data.meshes.new("Extrude")
        bm = bmesh.new()
        build(bm)
        bm.to_mesh(me)
        bm.free()
        ob = bpy.data.objects.new("Extrude", me)
        bpy.context.scene.collection.objects.link(ob)
        ob.select_set(True)
        bpy.context.view_layer.objects.active = ob
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.select_mode(type='FACE')
        bm = bmesh.from_edit_mesh(ob.data)
        for f in bm.faces:
            f.select_set(pick(f))
        bm.select_flush_mode()
        bmesh.update_edit_mesh(ob.data)
        faces = len(bm.faces)
        t0 = time.perf_counter()
        if solid:
            bpy.ops.mesh.extrude_solid(TRANSFORM_OT_translate={"value": move})
        else:
            bpy.ops.mesh.extrude_region_move(TRANSFORM_OT_translate={"value": move})
        times.append(ms(t0))
        bpy.ops.object.mode_set(mode='OBJECT')
    return faces, statistics.median(times)

def extrude_pair(label, build, pick, move):
    n, t_normal = extrude_case(build, pick, move, False)
    _n, t_solid = extrude_case(build, pick, move, True)
    rows.append((f"{label} (normal extrude)", n, t_normal, None))
    rows.append((f"{label} (Extrude Solid)", n, t_solid, max(t_solid - t_normal, 0.0)))
    print(f"  {label:<46} {n:>9,} faces  normal {t_normal:8.1f} ms   solid {t_solid:8.1f} ms"
          f"   cost {max(t_solid - t_normal, 0.0):8.1f} ms")

def cuts_for(target_faces):
    """Cuts that give about `target_faces` faces on a box (scaled down in quick mode)."""
    return max(2, int(((target_faces * SCALE) / 6.0) ** 0.5) - 1)

def top_corner(cuts):
    cell = 2.0 / (cuts + 1)
    return lambda f: f.normal.z > 0.9 and min(f.calc_center_median().x, f.calc_center_median().y) > 1 - cell

def top_block(cuts, size):
    half = size * (2.0 / (cuts + 1)) / 2.0
    return lambda f: (f.normal.z > 0.9 and abs(f.calc_center_median().x) < half and
                      abs(f.calc_center_median().y) < half)

def side_center(cuts):
    cell = 2.0 / (cuts + 1)
    return lambda f: (f.normal.x > 0.9 and abs(f.calc_center_median().y) < cell / 2 and
                      abs(f.calc_center_median().z) < cell / 2)

print()
print("  Extrude Solid (edit mode): time of the whole extrude; cost = time added to a normal extrude")
for target in (1500, 10000, 50000, 200000):
    c = cuts_for(target)
    cell = 2.0 / (c + 1)
    extrude_pair(f"Corner face pushed in one cell", lambda bm, c=c: solid_box(bm, c),
                 top_corner(c), (0, 0, -cell))
c = cuts_for(50000)
cell = 2.0 / (c + 1)
extrude_pair("10 x 10 faces pushed in halfway", lambda bm: solid_box(bm, c),
             top_block(c, 10), (0, 0, -1.0))
extrude_pair("Side face pushed through (hole)", lambda bm: solid_box(bm, c),
             side_center(c), (-2.5, 0, 0))
extrude_pair("10 x 10 faces pulled out", lambda bm: solid_box(bm, c),
             top_block(c, 10), (0, 0, 1.0))
extrude_pair("Same, pulled out at an angle", lambda bm: solid_box(bm, c),
             top_block(c, 10), (0.7, 0.3, 1.0))

def two_parts(bm):
    """A small box to edit, and a separate dense box far away in the same mesh."""
    solid_box(bm, cuts_for(200000))
    bmesh.ops.translate(bm, verts=bm.verts[:], vec=(10.0, 0.0, 0.0))
    solid_box(bm, 5)

extrude_pair("Small part of a mesh with a 200k-face part", two_parts,
             lambda f: (f.normal.z > 0.9 and f.calc_center_median().x < 2 and
                        min(f.calc_center_median().x, f.calc_center_median().y) > 1 - 2.0 / 6),
             (0, 0, -2.0 / 6))

# Report -----------------------------------------------------------------------------------------
try:
    commit = subprocess.run(["git", "-C", os.path.join(HERE, "blender"), "rev-parse", "--short", "HEAD"],
                            capture_output=True, text=True, timeout=10).stdout.strip()
except Exception:
    commit = "?"
stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
with open(RESULTS, "a", encoding="utf-8") as f:
    f.write(f"\n=== {stamp}  commit {commit}  {'quick' if QUICK else 'full'}  Blender {bpy.app.version_string} ===\n")
    f.write(f"{'test':<48}{'faces':>11}{'total ms':>12}{'cost ms':>12}\n")
    for label, n, total, extra in rows:
        f.write(f"{label:<48}{n:>11,}{total:>12.1f}{'' if extra is None else f'{extra:12.1f}'}\n")
print("-" * 100)
print("total = whole evaluation; cost = time added by our modifier (with minus without).")
print(f"Results appended to {RESULTS}")
print("\nRESULT: DONE")
