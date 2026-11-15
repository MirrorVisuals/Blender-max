# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Slice Plane: Plane empties cut the selected geometry of the objects in Edit Mode
# (Mesh > Slice), with the modes Slice, Split, Remove Above and Remove Below.
# ./blender.bin --background --factory-startup --python tests/python/bl_slice_plane.py
import sys
import math
import bpy
import bmesh

failures = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


def reset():
    if bpy.context.object and bpy.context.object.mode != 'OBJECT':
        bpy.ops.object.mode_set(mode='OBJECT')
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob)


def add_cube(location=(0.0, 0.0, 0.0), name="Cube"):
    bpy.ops.mesh.primitive_cube_add(size=2.0, location=location)
    ob = bpy.context.object
    ob.name = name
    return ob


def add_plane(location=(0.0, 0.0, 0.0), rotation=(0.0, 0.0, 0.0), select=True):
    bpy.ops.object.empty_add(type='PLANE', location=location, rotation=rotation)
    ob = bpy.context.object
    ob.select_set(select)
    return ob


def edit(*meshes, select_all=True):
    bpy.ops.object.select_all(action='DESELECT')
    for ob in meshes:
        ob.select_set(True)
    bpy.context.view_layer.objects.active = meshes[0]
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT' if select_all else 'DESELECT')


def counts(ob):
    if ob.mode == 'EDIT':
        bm = bmesh.from_edit_mesh(ob.data)
        return len(bm.verts), len(bm.edges), len(bm.faces)
    me = ob.data
    return len(me.vertices), len(me.edges), len(me.polygons)


def z_range(ob):
    bm = bmesh.from_edit_mesh(ob.data)
    zs = [(ob.matrix_world @ v.co).z for v in bm.verts]
    return round(min(zs), 4), round(max(zs), 4)


def islands(ob):
    bm = bmesh.from_edit_mesh(ob.data)
    seen = set()
    count = 0
    for v in bm.verts:
        if v.index in seen:
            continue
        count += 1
        stack = [v]
        while stack:
            x = stack.pop()
            if x.index in seen:
                continue
            seen.add(x.index)
            stack += [e.other_vert(x) for e in x.link_edges]
    bm.verts.index_update()
    return count


def test_empty_type():
    print("Plane empty")
    reset()
    plane = add_plane()
    check(plane.empty_display_type == 'PLANE', "Add > Empty > Plane adds a Plane empty")
    plane.empty_display_type = 'CUBE'
    plane.empty_display_type = 'PLANE'
    check(plane.empty_display_type == 'PLANE', "the display type can be set back to Plane")


def test_slice():
    print("Slice")
    reset()
    add_plane(location=(0.0, 0.0, 0.25))
    cube = add_cube()
    edit(cube)
    bpy.ops.mesh.slice_plane(mode='SLICE')
    check(counts(cube) == (12, 20, 10), "a horizontal plane adds a ring of 4 edges: %r" % (counts(cube),))
    bm = bmesh.from_edit_mesh(cube.data)
    cut = [e for e in bm.edges if e.select]
    check(len(cut) == 4, "the cut edges are selected")
    check(all(abs((cube.matrix_world @ v.co).z - 0.25) < 1e-5 for e in cut for v in e.verts),
          "the cut is on the plane")


def test_remove():
    print("Remove Above / Below")
    reset()
    add_plane(location=(0.0, 0.0, 0.5))
    cube = add_cube()
    edit(cube)
    bpy.ops.mesh.slice_plane(mode='REMOVE_ABOVE')
    check(z_range(cube) == (-1.0, 0.5), "Remove Above keeps what is under the plane: %r" % (z_range(cube),))
    check(counts(cube)[2] == 5, "the top is open without Fill")

    reset()
    add_plane(location=(0.0, 0.0, 0.5))
    cube = add_cube()
    edit(cube)
    bpy.ops.mesh.slice_plane(mode='REMOVE_BELOW')
    check(z_range(cube) == (0.5, 1.0), "Remove Below keeps what is over the plane: %r" % (z_range(cube),))

    reset()
    add_plane(location=(0.0, 0.0, 0.5))
    cube = add_cube()
    edit(cube)
    bpy.ops.mesh.slice_plane(mode='REMOVE_ABOVE', use_fill=True)
    check(counts(cube)[2] == 6, "Fill closes the cut: %r" % (counts(cube),))

    # A plane turned upside down: "Above" follows its normal.
    reset()
    add_plane(location=(0.0, 0.0, 0.5), rotation=(math.pi, 0.0, 0.0))
    cube = add_cube()
    edit(cube)
    bpy.ops.mesh.slice_plane(mode='REMOVE_ABOVE')
    check(z_range(cube) == (0.5, 1.0), "Above is the side of the normal: %r" % (z_range(cube),))


def test_split():
    print("Split")
    reset()
    add_plane()
    cube = add_cube()
    edit(cube)
    bpy.ops.mesh.slice_plane(mode='SPLIT')
    check(islands(cube) == 2, "Split makes two parts")


def test_selection_only():
    print("Only the selection is cut")
    reset()
    add_plane()
    cube = add_cube()
    edit(cube, select_all=False)
    bm = bmesh.from_edit_mesh(cube.data)
    for f in bm.faces:
        if f.normal.x > 0.5:
            f.select = True
    bmesh.update_edit_mesh(cube.data)
    bpy.ops.mesh.slice_plane(mode='SLICE')
    check(counts(cube) == (10, 15, 7), "only the selected face is cut: %r" % (counts(cube),))


def test_planes():
    print("Planes used")
    reset()
    add_plane(location=(0.0, 0.0, 0.5), select=False)
    add_plane(location=(0.0, 0.0, -0.5), select=False)
    cube = add_cube()
    edit(cube)
    bpy.ops.mesh.slice_plane(mode='SLICE')
    check(counts(cube) == (16, 28, 14), "with none selected, every visible plane cuts: %r" % (counts(cube),))

    reset()
    upper = add_plane(location=(0.0, 0.0, 0.5))
    add_plane(location=(0.0, 0.0, -0.5), select=False)
    cube = add_cube()
    edit(cube)
    upper.select_set(True)
    bpy.ops.mesh.slice_plane(mode='SLICE')
    check(counts(cube) == (12, 20, 10), "only the selected plane cuts: %r" % (counts(cube),))

    # Two crossing planes cut the pieces of each other.
    reset()
    add_plane(rotation=(0.0, math.pi / 2.0, 0.0))
    add_plane(rotation=(math.pi / 2.0, 0.0, 0.0))
    cube = add_cube()
    edit(cube)
    for ob in bpy.context.view_layer.objects:
        if ob.type == 'EMPTY':
            ob.select_set(True)
    bpy.ops.mesh.slice_plane(mode='SLICE')
    check(counts(cube)[2] == 16, "two crossing planes cut all the pieces: %r" % (counts(cube),))

    reset()
    cube = add_cube()
    edit(cube)
    try:
        bpy.ops.mesh.slice_plane()
        ok = False
    except RuntimeError:
        ok = True
    check(ok, "without a Plane empty, Slice reports an error")


def test_multi_object():
    print("Several objects in Edit Mode")
    reset()
    add_plane(location=(0.0, 0.0, 3.25))
    a = add_cube(name="A")
    # The plane is infinite and in world space: a moved, rotated and scaled object is cut there too.
    b = add_cube(location=(5.0, 0.0, 3.0), name="B")
    b.rotation_euler = (0.0, 0.0, 0.7)
    b.scale = (1.0, 1.0, 2.0)
    a.location.z = 3.0
    edit(a, b)
    bpy.ops.mesh.slice_plane(mode='REMOVE_ABOVE')
    check(z_range(a) == (2.0, 3.25), "the first object is cut: %r" % (z_range(a),))
    check(z_range(b) == (1.0, 3.25), "the second object is cut in world space: %r" % (z_range(b),))


def main():
    test_empty_type()
    test_slice()
    test_remove()
    test_split()
    test_selection_only()
    test_planes()
    test_multi_object()
    reset()
    if failures:
        print("\n%d FAILED:" % len(failures))
        for f in failures:
            print("  " + f)
        print("RESULT: FAILED")
        sys.exit(1)
    print("\nRESULT: ALL PASSED")


if __name__ == "__main__":
    main()
