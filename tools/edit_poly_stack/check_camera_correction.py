# Camera Tilt Correction check (Cycles): a camera tilted up at a tall box.
# Automatic: the box's sides stay parallel (same width at its top and bottom) and the point the
# camera aims at stays in the middle of the image; half the amount is in between; off, the sides
# converge as before. Manual: a vertical tilt equal to the camera's does the same, and a
# horizontal tilt straightens a wall seen from the side.
# Run with CHECK_CAMERA_CORRECTION.bat (Blender in background mode).
import math, os
import bpy
from mathutils import Vector

failures = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.samples = 4
scene.cycles.use_denoising = False
scene.render.resolution_x = 120
scene.render.resolution_y = 120
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.world = bpy.data.worlds.new("Black")
scene.world.color = (0, 0, 0)


def emission(name, strength):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    nodes.clear()
    em = nodes.new('ShaderNodeEmission')
    em.inputs['Strength'].default_value = strength
    out = nodes.new('ShaderNodeOutputMaterial')
    mat.node_tree.links.new(em.outputs[0], out.inputs[0])
    return mat


# A tall box, 2 wide, from the ground to 16 high, 12 in front of the camera.
bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0, 12, 8))
box = bpy.context.active_object
box.scale = (2, 0.2, 16)
box.data.materials.append(emission("Box", 1.0))

cam_data = bpy.data.cameras.new("Cam")
cam_data.lens = 24
cam = bpy.data.objects.new("Cam", cam_data)
scene.collection.objects.link(cam)
cam.location = (0, 0, 1.5)
tilt = math.radians(25)
cam.rotation_euler = (math.radians(90) + tilt, 0, 0)   # Looking along +Y, tilted up.
scene.camera = cam

# A small, brighter marker exactly where the camera aims, in front of the box.
aim = cam.matrix_world.to_3x3() @ Vector((0, 0, -1))
bpy.ops.mesh.primitive_uv_sphere_add(radius=0.15, location=cam.location + aim * 8)
marker = bpy.context.active_object
marker.data.materials.append(emission("Marker", 20.0))


def render(name):
    path = os.path.join(bpy.app.tempdir, name + ".exr")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    w, h = img.size
    px = list(img.pixels)
    bpy.data.images.remove(img)
    return w, h, [[sum(px[(y * w + x) * 4:(y * w + x) * 4 + 3]) / 3.0 for x in range(w)] for y in range(h)]


def box_width(rows, y):
    return sum(1 for v in rows[y] if 0.5 < v < 5.0)


def measure(name):
    w, h, rows = render(name)
    box_rows = [y for y in range(h) if box_width(rows, y) > 0]
    low, high = box_rows[0] + 3, box_rows[-1] - 3
    bright = [(x, y) for y in range(h) for x in range(w) if rows[y][x] > 5.0]
    centre = (sum(p[0] for p in bright) / len(bright), sum(p[1] for p in bright) / len(bright)) \
        if bright else None
    return box_width(rows, low), box_width(rows, high), centre, (w, h)


print("Camera tilted up 25 degrees at a tall box")
cam_data.use_tilt_correction = False
off_low, off_high, off_centre, size = measure("off")
print(f"  off:  box {off_low} px wide low, {off_high} px high")
check(off_high < off_low - 2, "without the correction the box narrows towards the top (converging)")

cam_data.use_tilt_correction = True
check(abs(cam_data.tilt_correction_amount - 1.0) < 1e-6, "turning it on starts at the full amount")
on_low, on_high, on_centre, size = measure("on")
print(f"  on:   box {on_low} px wide low, {on_high} px high")
check(abs(on_high - on_low) <= 1, "with the correction the box's sides are parallel")
check(on_centre is not None and abs(on_centre[0] - size[0] / 2) < 2 and abs(on_centre[1] - size[1] / 2) < 2,
      f"the point the camera aims at stays in the middle (at {on_centre}, middle {size[0] / 2})")

cam_data.tilt_correction_amount = 0.5
half_low, half_high, _c, _s = measure("half")
print(f"  half: box {half_low} px wide low, {half_high} px high")
check(off_high < half_high < on_high, "half the amount leaves part of the convergence")

cam_data.tilt_correction_amount = 1.0
cam_data.shift_y = 0.1
_l, _h, shifted_centre, _s = measure("with_shift")
check(shifted_centre is not None and shifted_centre[1] < on_centre[1] - 5,
      "the camera's own Shift Y still adds on top (the aim point moves down in the image)")
cam_data.shift_y = 0.0

print("Manual")
cam_data.tilt_correction_mode = 'MANUAL'
cam_data.tilt_vertical = tilt
man_low, man_high, man_centre, _s = measure("manual_vertical")
print(f"  manual vertical 25: box {man_low} px wide low, {man_high} px high")
check(abs(man_high - man_low) <= 1 and abs(man_low - on_low) <= 1,
      "a manual vertical tilt equal to the camera's gives the automatic result")
check(man_centre is not None and abs(man_centre[0] - on_centre[0]) < 1.5 and
      abs(man_centre[1] - on_centre[1]) < 1.5, "and keeps the aim point in the middle")
cam_data.tilt_vertical = 0.0
cam_data.tilt_correction_mode = 'AUTO'

cam_data.type = 'ORTHO'
cam_data.ortho_scale = 30
ortho_on = measure("ortho_on")[:2]
cam_data.use_tilt_correction = False
ortho_off = measure("ortho_off")[:2]
check(ortho_on == ortho_off, f"orthographic cameras are not changed ({ortho_on} against {ortho_off})")


# A wide wall seen from the side: a level camera turned 20 degrees to the right.
cam_data.type = 'PERSP'
cam_data.use_tilt_correction = False
box.scale = (16, 0.2, 3)
box.location = (0, 12, 1.5)
turn = math.radians(20)
cam.rotation_euler = (math.radians(90), 0, -turn)
aim = cam.matrix_world.to_3x3() @ Vector((0, 0, -1))
marker.location = cam.location + aim * 8


def wall_heights(rows, w, h):
    cols = [x for x in range(w) if sum(1 for y in range(h) if 0.5 < rows[y][x] < 5.0) > 0]
    height = lambda x: sum(1 for y in range(h) if 0.5 < rows[y][x] < 5.0)
    return height(cols[0] + 3), height(cols[-1] - 3), cols


def measure_wall(name):
    w, h, rows = render(name)
    left, right, cols = wall_heights(rows, w, h)
    bright = [(x, y) for y in range(h) for x in range(w) if rows[y][x] > 5.0]
    centre = (sum(p[0] for p in bright) / len(bright), sum(p[1] for p in bright) / len(bright)) \
        if bright else None
    return left, right, centre, (w, h)


print("Wall seen from the side (camera turned 20 degrees)")
w_off = measure_wall("wall_off")
print(f"  off:  wall {w_off[0]} px high left, {w_off[1]} px right")
check(abs(w_off[0] - w_off[1]) > 2, "without the correction the wall's top and bottom converge")
cam_data.use_tilt_correction = True
cam_data.tilt_correction_mode = 'MANUAL'
cam_data.tilt_horizontal = turn
w_on = measure_wall("wall_on")
print(f"  on:   wall {w_on[0]} px high left, {w_on[1]} px right")
check(abs(w_on[0] - w_on[1]) <= 1, "a manual horizontal tilt of 20 degrees makes them parallel")
check(w_on[2] is not None and abs(w_on[2][0] - w_on[3][0] / 2) < 2 and
      abs(w_on[2][1] - w_on[3][1] / 2) < 2,
      f"and keeps the aim point in the middle (at {w_on[2]})")
cam_data.tilt_correction_mode = 'AUTO'
w_auto = measure_wall("wall_auto")
check(abs(w_auto[0] - w_off[0]) <= 1 and abs(w_auto[1] - w_off[1]) <= 1,
      "the automatic mode leaves a level camera unchanged")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
