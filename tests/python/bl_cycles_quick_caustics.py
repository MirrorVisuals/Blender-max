# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Quick Caustics: sunlight through a rippled water surface makes a pattern of light on the floor
# below it, without tracing caustic paths. A flat surface lets the light through evenly.
# ./blender.bin --background --factory-startup --python tests/python/bl_cycles_quick_caustics.py
import math
import os
import sys

import bpy

failures = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


RES = 64

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.samples = 4
scene.cycles.use_denoising = False
scene.cycles.use_adaptive_sampling = False
scene.cycles.seed = 1
scene.cycles.max_bounces = 0
scene.render.resolution_x = RES
scene.render.resolution_y = RES
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.use_compositing = False

# Floor: white diffuse, seen from above by an orthographic camera.
bpy.ops.mesh.primitive_plane_add(size=8.0)
floor = bpy.context.object
floor.data.materials.append(bpy.data.materials.new("Floor"))

cam_data = bpy.data.cameras.new("Cam")
cam_data.type = 'ORTHO'
cam_data.ortho_scale = 4.0
cam = bpy.data.objects.new("Cam", cam_data)
scene.collection.objects.link(cam)
cam.location = (0, 0, 10)
scene.camera = cam

# Water surface 1 m above the floor, a dense grid with smooth normals. Invisible to the camera, so
# the camera sees the floor directly.
bpy.ops.mesh.primitive_grid_add(x_subdivisions=160, y_subdivisions=160, size=8.0, location=(0, 0, 1))
water = bpy.context.object
water.visible_camera = False
water.data.shade_smooth()


def set_ripples(amplitude):
    for v in water.data.vertices:
        x, y = v.co.x, v.co.y
        v.co.z = amplitude * (math.sin(x * 3.0) * math.cos(y * 2.3) + 0.5 * math.sin((x + y) * 4.1))
    water.data.update()


glass = bpy.data.materials.new("Water")
glass.use_nodes = True
nodes = glass.node_tree.nodes
nodes.clear()
bsdf = nodes.new("ShaderNodeBsdfGlass")
bsdf.inputs["IOR"].default_value = 1.33
bsdf.inputs["Roughness"].default_value = 0.0
out = nodes.new("ShaderNodeOutputMaterial")
glass.node_tree.links.new(bsdf.outputs[0], out.inputs["Surface"])
water.data.materials.append(glass)
cglass = glass.cycles

sun_data = bpy.data.lights.new("Sun", 'SUN')
sun_data.energy = 3.0
sun_data.angle = 0.0
sun = bpy.data.objects.new("Sun", sun_data)
scene.collection.objects.link(sun)
sun.rotation_euler = (0.2, 0.1, 0.0)


def render(name):
    path = os.path.join(bpy.app.tempdir, "quick_caustics_" + name + ".exr")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    px = list(img.pixels)
    bpy.data.images.remove(img)
    values = [px[i] for i in range(0, len(px), 4)]
    mean = sum(values) / len(values)
    spread = math.sqrt(sum((v - mean) ** 2 for v in values) / len(values))
    return mean, spread


print("Settings")
check(not cglass.use_quick_caustics, "Quick Caustics is off by default")
check(abs(cglass.quick_caustics_ior - 1.33) < 1e-6 and cglass.quick_caustics_depth > 0.0,
      "water IOR and a depth by default")

print("Without Quick Caustics")
set_ripples(0.0)
dark, _ = render("off")
water.hide_render = True
lit, lit_spread = render("no_water")
water.hide_render = False
check(dark < 0.05 * lit, "the glass water shadows the floor (%.4f of %.4f)" % (dark, lit))

print("Flat water")
cglass.use_quick_caustics = True
flat, flat_spread = render("flat")
check(0.85 * lit < flat <= 1.01 * lit, "light goes through, less the reflection (%.4f of %.4f)" % (flat, lit))
check(flat_spread < 0.05 * flat, "evenly (spread %.4f)" % flat_spread)

print("Rippled water")
set_ripples(0.1)
rippled, rippled_spread = render("ripples")
check(rippled_spread > 0.15 * rippled, "the ripples make a pattern (spread %.4f of %.4f)" % (rippled_spread, rippled))
check(0.6 * flat < rippled < 1.5 * flat, "with about the same light overall (%.4f, flat %.4f)" % (rippled, flat))

cglass.quick_caustics_strength = 0.0
_, none_spread = render("strength_zero")
check(none_spread < 0.05 * flat, "Strength 0: no pattern (spread %.4f)" % none_spread)
cglass.quick_caustics_strength = 2.0
_, strong_spread = render("strength_two")
check(strong_spread > 1.3 * rippled_spread, "Strength 2: more contrast (%.4f > %.4f)" % (strong_spread, rippled_spread))
cglass.quick_caustics_strength = 1.0

cglass.quick_caustics_depth = 0.5
_, shallow_spread = render("depth")
check(shallow_spread < 0.05 * flat, "the floor below Depth gets no pattern (spread %.4f)" % shallow_spread)
cglass.quick_caustics_depth = 2.0

print("Other lights")
sun.hide_render = True
point_data = bpy.data.lights.new("Point", 'POINT')
point_data.energy = 2000.0
point = bpy.data.objects.new("Point", point_data)
scene.collection.objects.link(point)
point.location = (0, 0, 4)
point_mean, _ = render("point")
water.hide_render = True
point_lit, _ = render("point_no_water")
water.hide_render = False
check(point_mean < 0.05 * point_lit, "a point light is still shadowed by the water (%.4f of %.4f)" %
      (point_mean, point_lit))

if failures:
    print("\n%d FAILED:" % len(failures))
    for f in failures:
        print("  " + f)
    print("RESULT: FAILED")
    sys.exit(1)
print("\nRESULT: ALL PASSED")
