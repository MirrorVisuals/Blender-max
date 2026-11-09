# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Phase 2 check: an Edit Poly move stays on the same point when the Subdivision below changes.
# ./blender.bin --background --factory-startup --python tests/python/bl_modifier_edit_poly.py
import sys
import bpy
import os
import tempfile

failures = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


def points(obj):
    """{stable_id: (x, y, z)} of the evaluated mesh."""
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(dg)
    me = ev.to_mesh()
    attr = me.attributes.get(".stable_id_point")
    if attr is None:
        ids = [(i, -i) for i in range(len(me.vertices))]  # placeholder, never matches a real ID
    else:
        ids = [tuple(d.value) for d in attr.data]
    out = {ids[i]: tuple(v.co) for i, v in enumerate(me.vertices)}
    ev.to_mesh_clear()
    return out


def moved_by(obj, edit, sid, expected):
    edit.show_viewport = False
    before = points(obj)
    edit.show_viewport = True
    after = points(obj)
    if sid not in before or sid not in after:
        return False
    d = [after[sid][k] - before[sid][k] for k in range(3)]
    others_same = all(after[k] == before[k] for k in before if k != sid)
    return all(abs(d[k] - expected[k]) < 1e-5 for k in range(3)) and others_same


bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_cube_add()
obj = bpy.context.active_object
sub = obj.modifiers.new("Subdivision", 'SUBSURF')
sub.levels = 1
edit = obj.modifiers.new("Edit Poly", 'EDIT_POLY')
check(edit.type == 'EDIT_POLY' and edit.moves_num == 0, "Edit Poly modifier added on top of Subdivision")

# Pick the centre of the top face at level 1 (only exists because of the Subdivision).
edit.show_viewport = False
pts = points(obj)
top = max(pts, key=lambda k: pts[k][2] - abs(pts[k][0]) - abs(pts[k][1]))
print("  top face centre ID:", top, "at", tuple(round(c, 3) for c in pts[top]))
edit.show_viewport = True

move = (0.0, 0.0, 0.5)
edit.move_set(stable_id=top, offset=move)
check(edit.moves_num == 1, "move stored")
check(moved_by(obj, edit, top, move), "level 1: that point moved up 0.5, nothing else moved")

sub.levels = 2
check(moved_by(obj, edit, top, move), "level 2: the same point is still the one moved")
sub.levels = 3
check(moved_by(obj, edit, top, move), "level 3: the same point is still the one moved")

sub.levels = 0
edit.show_viewport = False
base = points(obj)
edit.show_viewport = True
check(points(obj) == base, "level 0: point does not exist, move is paused (mesh unchanged, no crash)")
sub.levels = 1
check(moved_by(obj, edit, top, move), "back to level 1: paused move applies again")

edit.move_set(stable_id=top, offset=(0.0, 0.0, 1.0))
check(edit.moves_num == 1 and moved_by(obj, edit, top, (0.0, 0.0, 1.0)), "setting the same ID again replaces the move")

# Save and reload.
path = os.path.join(tempfile.gettempdir(), "edit_poly_check.blend")
bpy.ops.wm.save_as_mainfile(filepath=path)
bpy.ops.wm.open_mainfile(filepath=path)
obj = bpy.data.objects["Cube"]
edit = obj.modifiers["Edit Poly"]
check(edit.moves_num == 1, "move survives save and reload")
check(moved_by(obj, edit, top, (0.0, 0.0, 1.0)), "after reload: still moves the same point")

# Duplicate the object: the copy has its own moves.
obj.select_set(True)
bpy.context.view_layer.objects.active = obj
bpy.ops.object.duplicate()
dup = bpy.context.active_object
dup.modifiers["Edit Poly"].moves_clear()
check(dup.modifiers["Edit Poly"].moves_num == 0 and edit.moves_num == 1, "duplicate has its own copy of the moves")

edit.moves_clear()
check(edit.moves_num == 0, "moves_clear removes all moves")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
# A non-zero exit code makes the test fail when run by CTest.
sys.exit(1 if failures else 0)
