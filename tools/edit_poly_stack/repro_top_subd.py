# Reproduce: Edit Poly with an extrusion, then add a Subdivision on top.
import bpy, bmesh, sys
def step(msg):
    print(">>>", msg); sys.stdout.flush()
def evaluate(ob):
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    me = ob.evaluated_get(dg).to_mesh()
    n = (len(me.vertices), len(me.polygons))
    ob.evaluated_get(dg).to_mesh_clear()
    return n
for with_subd_below in (True, False):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.mesh.primitive_cube_add()
    ob = bpy.context.active_object
    if with_subd_below:
        ob.modifiers.new("Subdivision", 'SUBSURF').levels = 1
    step(f"case: subdivision below = {with_subd_below}")
    bpy.ops.object.edit_poly_tab()
    bpy.ops.mesh.select_mode(type='FACE'); bpy.ops.mesh.select_all(action='DESELECT')
    bm = bmesh.from_edit_mesh(ob.data)
    for f in bm.faces:
        if f.normal.z > 0.5 and f.calc_center_median().z > 0.5: f.select_set(True)
    bm.select_flush_mode(); bmesh.update_edit_mesh(ob.data); bpy.context.view_layer.update()
    bpy.ops.mesh.extrude_region_move('EXEC_DEFAULT', True, TRANSFORM_OT_translate={"value": (0, 0, 0.4)})
    bpy.context.view_layer.update()
    bpy.ops.object.edit_poly_tab()
    step(f"extruded, result {evaluate(ob)}")
    top = ob.modifiers.new("Subdivision Top", 'SUBSURF')
    top.levels = 1
    step("added Subdivision on top")
    step(f"result with top subdivision {evaluate(ob)}")
    top.levels = 2
    step(f"top level 2 {evaluate(ob)}")
    if with_subd_below:
        ob.modifiers["Subdivision"].levels = 2
        step(f"below level 2 + top {evaluate(ob)}")
step("DONE, no crash")
