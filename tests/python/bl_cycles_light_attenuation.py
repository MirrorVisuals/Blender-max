# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Map Range and Light Falloff of Cycles lights: the light is multiplied by a Map Range and a Light
# Falloff of the ray length, as Light Path > Map Range and Light Falloff nodes would do. With Full
# Visible Surface, the surface of an area light as seen by the camera is not changed by them.
# ./blender.bin --background --factory-startup --python tests/python/bl_cycles_light_attenuation.py
import sys
import math
import os
import bpy

failures = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


SIZE = 20.0      # Floor and view size (meters).
RES = 64         # Image size (pixels).
HEIGHT = 1.0     # Light height above the floor.

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.samples = 16
scene.cycles.use_denoising = False
scene.cycles.max_bounces = 0
scene.cycles.seed = 1
scene.cycles.filter_width = 0.01
scene.render.resolution_x = RES
scene.render.resolution_y = RES
scene.render.image_settings.file_format = 'OPEN_EXR'

bpy.ops.mesh.primitive_plane_add(size=SIZE, location=(0, 0, 0))
cam_data = bpy.data.cameras.new("Cam")
cam_data.type = 'ORTHO'
cam_data.ortho_scale = SIZE
cam = bpy.data.objects.new("Cam", cam_data)
scene.collection.objects.link(cam)
cam.location = (0, 0, 10)
scene.camera = cam

light_data = bpy.data.lights.new("Lamp", 'POINT')
light_data.shadow_soft_size = 0.0
light_data.energy = 1000.0
light = bpy.data.objects.new("Lamp", light_data)
scene.collection.objects.link(light)
light.location = (0, 0, HEIGHT)


def render(name):
    path = os.path.join(bpy.app.tempdir, name + ".exr")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    px = list(img.pixels)
    bpy.data.images.remove(img)
    return [sum(px[i:i + 3]) / 3.0 for i in range(0, len(px), 4)]


def distance(i):
    """Distance from the centre of pixel i (on the floor) to the light."""
    px, py = i % RES, i // RES
    x = -SIZE / 2 + (px + 0.5) * SIZE / RES
    y = -SIZE / 2 + (py + 0.5) * SIZE / RES
    return math.sqrt(x * x + y * y + HEIGHT * HEIGHT)


def smoothstep(e0, e1, x):
    if x < e0:
        return 0.0
    if x >= e1:
        return 1.0
    t = (x - e0) / (e1 - e0)
    return (3.0 - 2.0 * t) * t * t


def map_range(d, kind, from_min, from_max, to_min, to_max, clamp=True):
    """The Map Range node (Float), as Cycles computes it."""
    if from_max == from_min:
        result = 0.0
    else:
        if kind == 'SMOOTHSTEP':
            f = smoothstep(from_min, from_max, d)
        else:
            f = (d - from_min) / (from_max - from_min)
        result = to_min + f * (to_max - to_min)
    if clamp:
        lo, hi = min(to_min, to_max), max(to_min, to_max)
        result = min(max(result, lo), hi)
    return result


def light_falloff(d, output, strength, smooth):
    """The Light Falloff node, as Cycles computes it."""
    s = strength * {'QUADRATIC': 1.0, 'LINEAR': d, 'CONSTANT': d * d}[output]
    if smooth > 0.0:
        s *= d * d / (smooth + d * d)
    return s


def compare(label, on, off, fn, tolerance):
    """Check the brightness ratio per lit pixel against fn(distance)."""
    lit = [i for i in range(len(on)) if off[i] > 1e-4]
    worst = max(abs(on[i] / off[i] - fn(distance(i))) / max(1.0, abs(fn(distance(i)))) for i in lit)
    check(worst < tolerance, f"{label} ({len(lit)} pixels, worst error {worst:.4f})")


clight = light_data.cycles
check(not clight.use_map_range and not clight.use_light_falloff, "off by default")
off = render("attenuation_off")

clight.map_range_from_max = 6.0
clight.light_falloff_output = 'LINEAR'
same = render("attenuation_not_enabled")
check(same == off, "the values do nothing until Map Range / Light Falloff are enabled")

# Pixels cover a range of distances: steep parts of a curve get a larger tolerance.
print("Map Range")
clight.use_map_range = True
clight.map_range_interpolation_type = 'SMOOTHSTEP'
clight.map_range_from_min = 0.0
clight.map_range_from_max = 6.0
clight.map_range_to_min = 1.0
clight.map_range_to_max = 0.0
on = render("map_range_smoothstep")
compare("Smooth Step, From 0..6, To 1..0", on, off,
        lambda d: map_range(d, 'SMOOTHSTEP', 0.0, 6.0, 1.0, 0.0), 0.06)
beyond = [i for i in range(len(on)) if distance(i) > 6.0 + SIZE / RES]
check(beyond and max(on[i] for i in beyond) == 0.0, "no light beyond From Max (To Max 0)")

clight.map_range_from_min = 3.0
on = render("map_range_from_min")
inside = [i for i in range(len(on)) if distance(i) < 3.0 - SIZE / RES]
check(all(on[i] == off[i] for i in inside), "full strength up to From Min (To Min 1)")
compare("Smooth Step, From 3..6", on, off, lambda d: map_range(d, 'SMOOTHSTEP', 3.0, 6.0, 1.0, 0.0), 0.06)

clight.map_range_interpolation_type = 'LINEAR'
clight.map_range_from_min = 2.0
clight.map_range_from_max = 8.0
clight.map_range_to_min = 1.5
clight.map_range_to_max = 1.0
on = render("map_range_linear")
compare("Linear, From 2..8, To 1.5..1.0, Clamp", on, off,
        lambda d: map_range(d, 'LINEAR', 2.0, 8.0, 1.5, 1.0), 0.03)
clight.map_range_clamp = False
on = render("map_range_linear_unclamped")
compare("Linear without Clamp", on, off,
        lambda d: map_range(d, 'LINEAR', 2.0, 8.0, 1.5, 1.0, clamp=False), 0.03)
clight.map_range_clamp = True
clight.use_map_range = False

print("Light Falloff")
clight.use_light_falloff = True
for output, strength, smooth in (('QUADRATIC', 2.0, 0.0), ('LINEAR', 1.0, 0.0),
                                 ('CONSTANT', 1.0, 0.0), ('QUADRATIC', 1.0, 4.0)):
    clight.light_falloff_output = output
    clight.light_falloff_strength = strength
    clight.light_falloff_smooth = smooth
    on = render("falloff_" + output.lower())
    compare(f"{output.title()}, Strength {strength}, Smooth {smooth}", on, off,
            lambda d: light_falloff(d, output, strength, smooth), 0.06)

print("Both: Map Range x Light Falloff")
clight.use_map_range = True
clight.map_range_interpolation_type = 'SMOOTHSTEP'
clight.map_range_from_min = 0.0
clight.map_range_from_max = 3.8
clight.map_range_to_min = 1.0
clight.map_range_to_max = 0.0
clight.light_falloff_output = 'QUADRATIC'
clight.light_falloff_strength = 4.0
clight.light_falloff_smooth = 0.0
on = render("both")
compare("Smooth Step 0..3.8 x Quadratic 4 (the node setup)", on, off,
        lambda d: map_range(d, 'SMOOTHSTEP', 0.0, 3.8, 1.0, 0.0) * 4.0, 0.06)

print("Area light seen by the camera, Full Visible Surface")
light_data.type = 'AREA'
light_data = bpy.data.lights["Lamp"]
light_data.size = 2.0
light.visible_camera = True
light.rotation_euler = (math.pi, 0.0, 0.0)   # Facing up, at the camera.
clight = light_data.cycles
clight.use_light_falloff = False
clight.use_map_range = False
light_data.use_full_visible_surface = True
seen_off = render("seen_off")
surface = [i for i in range(len(seen_off)) if seen_off[i] > 1e-3]
clight.use_map_range = True
clight.map_range_from_max = 2.0         # The camera is 9 m away: far beyond.
seen_fvs = render("seen_fvs")
check(surface and all(abs(seen_fvs[i] - seen_off[i]) <= 1e-4 * seen_off[i] for i in surface),
      f"Full Visible Surface: the light's surface looks the same ({len(surface)} pixels)")
light_data.use_full_visible_surface = False
seen_plain = render("seen_plain")
check(all(seen_plain[i] == 0.0 for i in surface),
      "without Full Visible Surface the Map Range applies to the seen surface too")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
for f in failures:
    print("  -", f)
# A non-zero exit code makes the test fail when run by CTest.
sys.exit(1 if failures else 0)
