# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

# Edit Poly node in typical Geometry Nodes setups: what happens to an edit when something
# earlier in the node tree changes, and how long evaluation and Tab take on a large mesh.
# This is a report, not a pass / fail test.
# ./blender.bin --background --factory-startup --python tools/edit_poly_stack/benchmark_node_edit_poly.py
import functools
import time
import bpy
import bmesh

# Print each line at once, so the report is complete up to the point where anything stops.
print = functools.partial(print, flush=True)

RESULTS = []

EXPLAIN = {
    "FOLLOWS": "the edit stays on the same point / face",
    "PAUSED": "the edit is switched off (kept, comes back when the change is undone)",
    "WRONG PLACE": "the edit lands on a different point / face",
    "APPLIED": "the edit is applied",
    "NOT APPLIED": "the edit is not applied",
}


# -----------------------------------------------------------------------------
# Helpers


def sock(sockets, name):
    s = sockets.get(name)
    if s is None:
        raise RuntimeError("no socket '{}' (has: {})".format(name, ", ".join(x.name for x in sockets)))
    return s


def new_tree(name):
    tree = bpy.data.node_groups.new(name, 'GeometryNodeTree')
    tree.interface.new_socket("Geometry", in_out='INPUT', socket_type='NodeSocketGeometry')
    tree.interface.new_socket("Geometry", in_out='OUTPUT', socket_type='NodeSocketGeometry')
    gi = tree.nodes.new('NodeGroupInput')
    go = tree.nodes.new('NodeGroupOutput')
    return tree, gi, go


def chain(tree, sockets):
    """Link output, input, output, input... pairs."""
    for a, b in zip(sockets[0::2], sockets[1::2]):
        tree.links.new(a, b)


def new_object(name, tree, location):
    bpy.ops.mesh.primitive_cube_add(location=location)
    ob = bpy.context.active_object
    ob.name = name
    mod = ob.modifiers.new("GeometryNodes", 'NODES')
    mod.node_group = tree
    ob.modifiers.active = mod
    return ob, mod


def evaluated(ob):
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    co = [tuple(v.co) for v in me.vertices]
    faces = len(me.polygons)
    ev.to_mesh_clear()
    return co, faces


def tab():
    try:
        bpy.ops.object.edit_poly_tab()
        return None
    except RuntimeError as ex:
        return str(ex).strip().splitlines()[-1]


def activate(ob):
    for o in bpy.context.view_layer.objects:
        o.select_set(False)
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob


def corner_index(co):
    return max(range(len(co)), key=lambda i: co[i][0] + co[i][1] * 1.001 + co[i][2] * 1.002)


def edit_move_corner(ob):
    """Session on the Edit Poly node: raise the corner point (largest x + y + z) by 1."""
    activate(ob)
    err = tab()
    if err or ob.mode != 'EDIT':
        raise RuntimeError("Tab failed: {}".format(err))
    bm = bmesh.from_edit_mesh(ob.data)
    v = max(bm.verts, key=lambda v: v.co.x + v.co.y * 1.001 + v.co.z * 1.002)
    v.co.z += 1.0
    bm.normal_update()
    bmesh.update_edit_mesh(ob.data)
    err = tab()
    if err or ob.mode != 'OBJECT':
        raise RuntimeError("Tab out failed: {}".format(err))


def edit_delete_top_face(ob):
    """Session on the Edit Poly node: delete the highest face."""
    activate(ob)
    err = tab()
    if err or ob.mode != 'EDIT':
        raise RuntimeError("Tab failed: {}".format(err))
    bm = bmesh.from_edit_mesh(ob.data)
    f = max(bm.faces, key=lambda f: f.calc_center_median().z + f.calc_center_median().x * 0.001)
    bmesh.ops.delete(bm, geom=[f], context='FACES_ONLY')
    bm.normal_update()
    bmesh.update_edit_mesh(ob.data)
    err = tab()
    if err or ob.mode != 'OBJECT':
        raise RuntimeError("Tab out failed: {}".format(err))


def classify_move(ob, edit_node):
    """Compare the result with the node muted (no edits) and unmuted."""
    edit_node.mute = True
    base, _ = evaluated(ob)
    edit_node.mute = False
    edited, _ = evaluated(ob)
    if len(base) != len(edited):
        return "WRONG PLACE", "point count changed ({} -> {})".format(len(base), len(edited))
    changed = [i for i in range(len(base))
               if max(abs(base[i][k] - edited[i][k]) for k in range(3)) > 1e-4]
    if not changed:
        return "PAUSED", ""
    corner = corner_index(base)
    if changed == [corner]:
        dz = edited[corner][2] - base[corner][2]
        if abs(dz - 1.0) < 1e-3:
            return "FOLLOWS", ""
        return "FOLLOWS", "raised by {:.2f} instead of 1".format(dz)
    if corner in changed:
        return "WRONG PLACE", "the corner moved, and {} other point(s)".format(len(changed) - 1)
    return "WRONG PLACE", "{} other point(s) moved, not the corner".format(len(changed))


def classify_delete(ob, edit_node):
    edit_node.mute = True
    base_co, base_faces = evaluated(ob)
    edit_node.mute = False
    _co, faces = evaluated(ob)
    if faces == base_faces - 1:
        return "APPLIED", "one face fewer"
    if faces == base_faces:
        return "NOT APPLIED", "paused"
    return "WRONG PLACE", "{} faces instead of {}".format(faces, base_faces - 1)


def run(name, build, change, edit="move", note=""):
    """build() -> (tree, edit_node, extra); change(extra) changes something before the node."""
    print("\n" + name)
    try:
        tree, edit_node, extra = build()
        ob, mod = new_object(name, tree, (len(RESULTS) * 4.0, 0.0, 0.0))
        if edit == "move":
            edit_move_corner(ob)
            classify = classify_move
        else:
            edit_delete_top_face(ob)
            classify = classify_delete
        before, before_note = classify(ob, edit_node)
        change(extra)
        after, after_note = classify(ob, edit_node)
    except Exception as ex:
        print("  SETUP ERROR:", ex)
        RESULTS.append((name, "ERROR", str(ex)))
        return
    print("  right after editing: {} {}".format(before, before_note))
    print("  after the change:    {} {}".format(after, after_note))
    if note:
        print("  ({})".format(note))
    RESULTS.append((name, after, after_note))


# -----------------------------------------------------------------------------
# Scenarios


def build_transform():
    tree, gi, go = new_tree("Transform before")
    t = tree.nodes.new('GeometryNodeTransform')
    e = tree.nodes.new('GeometryNodeEditPoly')
    chain(tree, [gi.outputs[0], sock(t.inputs, "Geometry"), sock(t.outputs, "Geometry"), e.inputs[0],
                 e.outputs[0], go.inputs[0]])
    return tree, e, t


def change_transform(t):
    sock(t.inputs, "Translation").default_value = (0.0, 0.0, 2.0)
    sock(t.inputs, "Scale").default_value = (1.5, 1.5, 1.5)


def build_set_position():
    tree, gi, go = new_tree("Set Position before")
    sp = tree.nodes.new('GeometryNodeSetPosition')
    e = tree.nodes.new('GeometryNodeEditPoly')
    chain(tree, [gi.outputs[0], sock(sp.inputs, "Geometry"), sock(sp.outputs, "Geometry"), e.inputs[0],
                 e.outputs[0], go.inputs[0]])
    return tree, e, sp


def change_set_position(sp):
    sock(sp.inputs, "Offset").default_value = (0.3, 0.0, 0.2)


def build_subdivide(idname, level_socket="Level"):
    def build():
        tree, gi, go = new_tree(idname)
        s = tree.nodes.new(idname)
        sock(s.inputs, level_socket).default_value = 1
        e = tree.nodes.new('GeometryNodeEditPoly')
        chain(tree, [gi.outputs[0], sock(s.inputs, "Mesh"), sock(s.outputs, "Mesh"), e.inputs[0],
                     e.outputs[0], go.inputs[0]])
        return tree, e, s
    return build


def change_level(s):
    sock(s.inputs, "Level").default_value = 2


def build_boolean():
    tree, gi, go = new_tree("Boolean before")
    cube = tree.nodes.new('GeometryNodeMeshCube')
    sock(cube.inputs, "Size").default_value = (0.8, 0.8, 0.8)
    t = tree.nodes.new('GeometryNodeTransform')
    sock(t.inputs, "Translation").default_value = (-0.8, -0.8, 0.0)
    b = tree.nodes.new('GeometryNodeMeshBoolean')
    b.operation = 'DIFFERENCE'
    e = tree.nodes.new('GeometryNodeEditPoly')
    chain(tree, [sock(cube.outputs, "Mesh"), sock(t.inputs, "Geometry"),
                 gi.outputs[0], sock(b.inputs, "Mesh 1"),
                 sock(t.outputs, "Geometry"), sock(b.inputs, "Mesh 2"),
                 sock(b.outputs, "Mesh"), e.inputs[0], e.outputs[0], go.inputs[0]])
    return tree, e, t


def change_boolean(t):
    sock(t.inputs, "Translation").default_value = (-0.8, 0.8, 0.0)


def build_grid():
    tree, gi, go = new_tree("Grid primitive")
    g = tree.nodes.new('GeometryNodeMeshGrid')
    sock(g.inputs, "Vertices X").default_value = 10
    sock(g.inputs, "Vertices Y").default_value = 10
    e = tree.nodes.new('GeometryNodeEditPoly')
    chain(tree, [sock(g.outputs, "Mesh"), e.inputs[0], e.outputs[0], go.inputs[0]])
    return tree, e, g


def change_grid_size(g):
    sock(g.inputs, "Size X").default_value = 3.0


def change_grid_resolution(g):
    sock(g.inputs, "Vertices X").default_value = 12
    sock(g.inputs, "Vertices Y").default_value = 12


def build_repeat():
    tree, gi, go = new_tree("Repeat zone")
    ri = tree.nodes.new('GeometryNodeRepeatInput')
    ro = tree.nodes.new('GeometryNodeRepeatOutput')
    ri.pair_with_output(ro)
    sock(ri.inputs, "Iterations").default_value = 3
    t = tree.nodes.new('GeometryNodeTransform')
    sock(t.inputs, "Translation").default_value = (0.0, 0.0, 0.5)
    e = tree.nodes.new('GeometryNodeEditPoly')
    chain(tree, [gi.outputs[0], sock(ri.inputs, "Geometry"),
                 sock(ri.outputs, "Geometry"), sock(t.inputs, "Geometry"),
                 sock(t.outputs, "Geometry"), e.inputs[0],
                 e.outputs[0], sock(ro.inputs, "Geometry"),
                 sock(ro.outputs, "Geometry"), go.inputs[0]])
    return tree, e, ri


def change_iterations(ri):
    sock(ri.inputs, "Iterations").default_value = 4


def build_instances(edit_before):
    def build():
        tree, gi, go = new_tree("Instances, Edit Poly " + ("before" if edit_before else "after"))
        line = tree.nodes.new('GeometryNodeMeshLine')
        sock(line.inputs, "Count").default_value = 3
        iop = tree.nodes.new('GeometryNodeInstanceOnPoints')
        real = tree.nodes.new('GeometryNodeRealizeInstances')
        e = tree.nodes.new('GeometryNodeEditPoly')
        if edit_before:
            chain(tree, [gi.outputs[0], e.inputs[0],
                         e.outputs[0], sock(iop.inputs, "Instance"),
                         sock(line.outputs, "Mesh"), sock(iop.inputs, "Points"),
                         sock(iop.outputs, "Instances"), sock(real.inputs, "Geometry"),
                         sock(real.outputs, "Geometry"), go.inputs[0]])
        else:
            chain(tree, [gi.outputs[0], sock(iop.inputs, "Instance"),
                         sock(line.outputs, "Mesh"), sock(iop.inputs, "Points"),
                         sock(iop.outputs, "Instances"), sock(real.inputs, "Geometry"),
                         sock(real.outputs, "Geometry"), e.inputs[0],
                         e.outputs[0], go.inputs[0]])
        return tree, e, line
    return build


def change_count(line):
    sock(line.inputs, "Count").default_value = 5


# -----------------------------------------------------------------------------
# Large mesh timing


def benchmark(resolution):
    print("\nTiming: grid of {0} x {0} points ({1:,} points), every point moved".format(
        resolution, resolution * resolution))
    tree, gi, go = new_tree("Benchmark")
    g = tree.nodes.new('GeometryNodeMeshGrid')
    sock(g.inputs, "Vertices X").default_value = resolution
    sock(g.inputs, "Vertices Y").default_value = resolution
    e = tree.nodes.new('GeometryNodeEditPoly')
    chain(tree, [sock(g.outputs, "Mesh"), e.inputs[0], e.outputs[0], go.inputs[0]])
    ob, mod = new_object("Benchmark", tree, (0.0, -6.0, 0.0))
    activate(ob)

    t0 = time.perf_counter()
    tab()
    t1 = time.perf_counter()
    bm = bmesh.from_edit_mesh(ob.data)
    for i, v in enumerate(bm.verts):
        v.co.z += 0.001 * (i % 97)
    bm.normal_update()
    bmesh.update_edit_mesh(ob.data)
    t2 = time.perf_counter()
    tab()
    t3 = time.perf_counter()

    def eval_ms(muted):
        e.mute = muted
        bpy.context.view_layer.update()
        times = []
        for k in range(3):
            sock(g.inputs, "Size X").default_value = 1.0 + 0.01 * (k + 1)
            t = time.perf_counter()
            bpy.context.view_layer.update()
            times.append(time.perf_counter() - t)
        return min(times) * 1000.0

    base = eval_ms(True)
    edited = eval_ms(False)
    print("  Tab in:  {:.0f} ms".format((t1 - t0) * 1000.0))
    print("  Tab out (record {:,} moves): {:.0f} ms".format(resolution * resolution, (t3 - t2) * 1000.0))
    print("  evaluation without the edits: {:.1f} ms".format(base))
    print("  evaluation with the edits:    {:.1f} ms (the node adds {:.1f} ms)".format(edited, edited - base))
    bpy.data.objects.remove(ob)


# -----------------------------------------------------------------------------

bpy.ops.wm.read_factory_settings(use_empty=True)

run("1. Transform node before Edit Poly, then move / scale it",
    build_transform, change_transform)
run("2. Set Position node before Edit Poly, then change its offset",
    build_set_position, change_set_position)
run("3. Subdivide Mesh node (level 1) before Edit Poly, then level 2",
    build_subdivide('GeometryNodeSubdivideMesh'), change_level)
run("4. Subdivision Surface node (level 1) before Edit Poly, then level 2",
    build_subdivide('GeometryNodeSubdivisionSurface'), change_level)
run("5. Subdivision Surface node, delete a face, then level 2",
    build_subdivide('GeometryNodeSubdivisionSurface'), change_level, edit="delete")
run("6. Boolean (cube minus a small cube) before Edit Poly, then move the small cube",
    build_boolean, change_boolean)
run("7. Grid node (10 x 10) before Edit Poly, then make it wider",
    build_grid, change_grid_size)
run("8. Grid node (10 x 10) before Edit Poly, then 12 x 12 points",
    build_grid, change_grid_resolution)
run("9. Edit Poly inside a repeat zone (3 times), then 4 times",
    build_repeat, change_iterations,
    note="the edit is applied once per repetition")
run("10. Edit Poly before instancing (3 copies, realized), then 5 copies",
    build_instances(True), change_count,
    note="correct here: the edit is made before copying, so every copy has it (one point per copy)")
run("11. Edit Poly after instancing (3 copies, realized), then 5 copies",
    build_instances(False), change_count)

for res in (100, 300):
    try:
        benchmark(res)
    except Exception as ex:
        print("  SETUP ERROR:", ex)

print("\n" + "=" * 78)
print("SUMMARY (what happens to the edit after the change)")
print("=" * 78)
for name, result, note in RESULTS:
    print("{:<74} {}".format(name, result))
    if note:
        print("    " + note)
print("\nMeaning:")
for key, text in EXPLAIN.items():
    print("  {:<12} {}".format(key, text))
