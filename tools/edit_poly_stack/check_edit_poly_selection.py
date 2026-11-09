# Edit Poly selection check: edges / vertices selected in a level are passed up the stack, Bevel with
# Limit Method Selection bevels only them, and the selection is written to the vertex group
# "Edit Poly Selection" for other modifiers. Run with CHECK_EDIT_POLY_SELECTION.bat.
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

def evaluated(ob):
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    ev = ob.evaluated_get(dg)
    me = ev.to_mesh()
    group = ob.vertex_groups.get("Edit Poly Selection")
    weights = []
    if group is not None:
        for v in me.vertices:
            w = [g.weight for g in v.groups if g.group == group.index]
            weights.append(w[0] if w else 0.0)
    out = {"verts": len(me.vertices), "weights": weights}
    ev.to_mesh_clear()
    return out

def select_only(ob, mode, pick):
    bpy.context.tool_settings.mesh_select_mode = (mode == 'VERT', mode == 'EDGE', mode == 'FACE')
    bm = bmesh.from_edit_mesh(ob.data)
    for f in bm.faces:
        f.select_set(False)
    for e in bm.edges:
        e.select_set(False)
    for v in bm.verts:
        v.select_set(False)
    pick(bm).select_set(True)
    bm.select_flush_mode()
    bmesh.update_edit_mesh(ob.data)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_cube_add()
ob = bpy.context.active_object

print("Select one edge in a level")
tab()
ep = ob.modifiers.get("Edit Poly")
select_only(ob, 'EDGE', lambda bm: next(e for e in bm.edges
                                        if all(v.co.z > 0.9 for v in e.verts)))
tab()
check(ob.mode == 'OBJECT', "Tab: back in Object Mode")
check("Edit Poly Selection" in ob.vertex_groups, "the object has the vertex group \"Edit Poly Selection\"")
data = evaluated(ob)
check(sorted(data["weights"]) == [0.0] * 6 + [1.0, 1.0],
      "the two vertices of the selected edge have weight 1 in it")

bev = ob.modifiers.new("Bevel", 'BEVEL')
bev.affect = 'EDGES'
bev.width = 0.1
bev.segments = 1
bev.limit_method = 'NONE'
all_verts = evaluated(ob)["verts"]
bev.limit_method = 'SELECTION'
data = evaluated(ob)
check(data["verts"] == 10, f"Bevel, Limit Method Selection: only the selected edge is beveled ({data['verts']} vertices, all edges: {all_verts})")

print("Select one vertex instead")
ob.modifiers.active = ep
tab()
bm = bmesh.from_edit_mesh(ob.data)
check(sum(1 for e in bm.edges if e.select) == 1, "entering the level again shows the selected edge")
select_only(ob, 'VERT', lambda bm: max(bm.verts, key=lambda v: v.co.x + v.co.y + v.co.z))
tab()
bev.affect = 'VERTICES'
data = evaluated(ob)
check(data["verts"] == 10, f"Bevel on Vertices, Limit Method Selection: one corner is beveled ({data['verts']} vertices)")

print("Faces still reach the UVW Map")
ob.modifiers.active = ep
tab()
select_only(ob, 'FACE', lambda bm: max(bm.faces, key=lambda f: f.calc_center_median().z))
tab()
check(ep.selected_faces_num == 1, "one face is stored")
bev.affect = 'EDGES'
data = evaluated(ob)
check(data["verts"] == 16, f"selecting a face selects its 4 edges for the Bevel ({data['verts']} vertices)")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
