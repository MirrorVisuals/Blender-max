# Edit Poly check: bevel weights, creases, sharp edges and materials edited in a level are stored
# in it (by stable ID), not lost when leaving the level, and the base mesh is not changed.
# Run with CHECK_EDIT_POLY_ATTRS.bat (Blender in background mode).
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
    def values(name):
        a = me.attributes.get(name)
        return [d.value for d in a.data] if a else None
    out = {
        "verts": len(me.vertices),
        "edges": [(tuple(me.vertices[e.vertices[0]].co), tuple(me.vertices[e.vertices[1]].co)) for e in me.edges],
        "bevel": values("bevel_weight_edge"),
        "crease_vert": values("crease_vert"),
        "material": values("material_index"),
    }
    ev.to_mesh_clear()
    return out

def top_edge(bm_edge):
    return all(v.co.z > 0.9 for v in bm_edge.verts)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_cube_add()
ob = bpy.context.active_object
base = ob.data

print("Edit the data in a level")
tab()
ep = ob.modifiers.get("Edit Poly")
check(ob.mode == 'EDIT' and ep is not None, "Tab starts an Edit Poly level")
bm = bmesh.from_edit_mesh(ob.data)
bw = bm.edges.layers.float.get("bevel_weight_edge") or bm.edges.layers.float.new("bevel_weight_edge")
cv = bm.verts.layers.float.get("crease_vert") or bm.verts.layers.float.new("crease_vert")
for e in bm.edges:
    if top_edge(e):
        e[bw] = 1.0
corner = max(bm.verts, key=lambda v: v.co.x + v.co.y + v.co.z)
corner[cv] = 0.75
top_face = max(bm.faces, key=lambda f: f.calc_center_median().z)
top_face.material_index = 1
bmesh.update_edit_mesh(ob.data)
tab()
check(ob.mode == 'OBJECT' and ob.data == base, "Tab again: back to the base mesh")
check(ep.attribute_edits_num == 6, f"6 edits stored: 4 edge weights, 1 vertex crease, 1 material ({ep.attribute_edits_num})")
base_bw = base.attributes.get("bevel_weight_edge")
check(base_bw is None or all(d.value == 0 for d in base_bw.data), "the base mesh has no bevel weights")

data = evaluated(ob)
weighted = [e for e, w in zip(data["edges"], data["bevel"] or []) if w > 0.99]
check(len(weighted) == 4 and all(a[2] > 0.9 and b[2] > 0.9 for a, b in weighted),
      "the 4 top edges have bevel weight 1 on the result")
check(data["crease_vert"] is not None and sorted(data["crease_vert"])[-1] == 0.75, "the vertex crease is on the result")
check(data["material"] is not None and sorted(data["material"]) == [0, 0, 0, 0, 0, 1], "one face has material index 1")

print("A Bevel above uses the weights")
bev = ob.modifiers.new("Bevel", 'BEVEL')
bev.limit_method = 'WEIGHT'
bev.width = 0.1
data = evaluated(ob)
check(data["verts"] > 8, f"Bevel (Limit Method Weight) bevels the edges ({data['verts']} vertices)")
bev.show_viewport = False

print("Entering the level again")
ob.modifiers.active = ep
tab()
bm = bmesh.from_edit_mesh(ob.data)
bw = bm.edges.layers.float.get("bevel_weight_edge")
check(bw is not None and sum(1 for e in bm.edges if e[bw] > 0.99) == 4, "the weights are there to edit")
for e in bm.edges:
    if top_edge(e) and e[bw] > 0.99:
        e[bw] = 0.0
        break
bmesh.update_edit_mesh(ob.data)
tab()
check(ep.attribute_edits_num == 5, f"setting one weight back to 0 removes it ({ep.attribute_edits_num} left)")

print("Moves and data together")
tab()
bm = bmesh.from_edit_mesh(ob.data)
v = max(bm.verts, key=lambda v: v.co.z - v.co.x)
v.co.z += 0.5
bm.normal_update()
bmesh.update_edit_mesh(ob.data)
tab()
check(ep.moves_num == 1 and ep.attribute_edits_num == 5, "a move is added, the data edits stay")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
