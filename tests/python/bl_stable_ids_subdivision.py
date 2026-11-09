# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Checks that Subdivision gives the same stable IDs to the same points at every level.
# ./blender.bin --background --factory-startup --python tests/python/bl_stable_ids_subdivision.py
import sys
import bpy


def ids(obj, domain_attr):
    dg = bpy.context.evaluated_depsgraph_get()
    me = obj.evaluated_get(dg).to_mesh()
    attr = me.attributes.get(domain_attr)
    out = [] if attr is None else [tuple(d.value) for d in attr.data]
    obj.evaluated_get(dg).to_mesh_clear()
    return out


def ids_at_levels(obj, mod, levels):
    result = {}
    for lv in levels:
        mod.levels = lv
        bpy.context.view_layer.update()
        result[lv] = ids(obj, ".stable_id_point")
    return result


failures = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


bpy.ops.wm.read_factory_settings(use_empty=True)
for name, add in [("cube (quads)", lambda: bpy.ops.mesh.primitive_cube_add()),
                  ("cylinder (8-gon caps)", lambda: bpy.ops.mesh.primitive_cylinder_add(vertices=8))]:
    print(name)
    add()
    obj = bpy.context.active_object
    mod = obj.modifiers.new("Subdivision", 'SUBSURF')
    per_level = ids_at_levels(obj, mod, [1, 2, 3])
    for lv, v in per_level.items():
        check(len(v) > 0, f"level {lv}: has .stable_id_point ({len(v)} points)")
        check(len(set(v)) == len(v), f"level {lv}: IDs are unique")
    check(set(per_level[1]) <= set(per_level[2]), "every level 1 point keeps its ID at level 2")
    check(set(per_level[2]) <= set(per_level[3]), "every level 2 point keeps its ID at level 3")
    check(per_level[1] == ids_at_levels(obj, mod, [1])[1], "same IDs when evaluated again")

    # Flip normals on the base mesh: IDs must not change.
    before = set(ids_at_levels(obj, mod, [2])[2])
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.mesh.flip_normals()
    bpy.ops.object.mode_set(mode='OBJECT')
    check(set(ids_at_levels(obj, mod, [2])[2]) == before, "flip normals keeps all IDs")
    for d in (".stable_id_edge", ".stable_id_face", ".stable_parent_point"):
        check(len(ids(obj, d)) > 0, f"has {d}")
    bpy.data.objects.remove(obj)

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
# A non-zero exit code makes the test fail when run by CTest.
sys.exit(1 if failures else 0)
