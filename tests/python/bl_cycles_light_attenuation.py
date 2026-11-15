# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Map Range and Light Falloff of Cycles lights, by the ray length (the distance to the light): no
# falloff up to Min Distance, a fade to zero at Max Distance, and the Light Falloff node's outputs.
# With Full Visible Surface, the surface of an area light as seen by the camera is not changed.
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


def fade(d, kind, min_distance, max_distance):
    """Map Range part: 1 up to Min Distance, fading to 0 at Max Distance (Smooth Step or Linear)."""
    if d >= max_distance:
        return 0.0
    if d <= min_distance:
        return 1.0
    if kind == 'SMOOTHSTEP':
        return 1.0 - smoothstep(min_distance, max_distance, d)
    return 1.0 - (d - min_distance) / (max_distance - min_distance)


def light_falloff(d, output, smooth):
    """The Light Falloff node (Strength 1), as Cycles computes it."""
    s = {'QUADRATIC': 1.0, 'LINEAR': d, 'CONSTANT': d * d}[output]
    if smooth > 0.0:
        s *= d * d / (smooth + d * d)
    return s


def attenuation(d, kind, min_distance, max_distance, output=None, smooth=0.0, to_min=1.0, to_max=0.0):
    """Expected factor: inside Min Distance the light is as bright as at Min Distance (the usual
    1 / d^2 undone) times To Min, with the Light Falloff evaluated there; To Max from Max Distance."""
    factor = 1.0
    e = d
    if kind is not None:
        if d >= max_distance:
            factor = to_max
        elif d < min_distance:
            factor = to_min * (d / min_distance) ** 2
            e = min_distance
        else:
            f = 1.0 - fade(d, kind, min_distance, max_distance)
            factor = to_min + f * (to_max - to_min)
    if output is not None:
        factor *= light_falloff(e, output, smooth)
    return factor


def compare(label, on, off, fn, tolerance):
    """Check the brightness ratio per lit pixel against fn(distance)."""
    lit = [i for i in range(len(on)) if off[i] > 1e-4]
    worst = max(abs(on[i] / off[i] - fn(distance(i))) / max(1.0, abs(fn(distance(i)))) for i in lit)
    check(worst < tolerance, f"{label} ({len(lit)} pixels, worst error {worst:.4f})")


clight = light_data.cycles
check(not clight.use_map_range and not clight.use_light_falloff, "off by default")
check(not hasattr(clight, "map_range_fade_in"), "no Fade In")
check(clight.map_range_to_min == 1.0 and clight.map_range_to_max == 0.0, "To Min 1, To Max 0 by default")
off = render("attenuation_off")

clight.map_range_max_distance = 6.0
clight.light_falloff_output = 'LINEAR'
same = render("attenuation_not_enabled")
check(same == off, "the values do nothing until Map Range / Light Falloff are enabled")

print("Map Range: Max Distance")
clight.use_map_range = True
clight.map_range_interpolation_type = 'SMOOTHSTEP'
clight.map_range_min_distance = 0.0
clight.map_range_max_distance = 6.0
on = render("max_distance")
compare("Smooth Step to zero at 6 m", on, off, lambda d: attenuation(d, 'SMOOTHSTEP', 0.0, 6.0), 0.06)
beyond = [i for i in range(len(on)) if distance(i) > 6.0 + SIZE / RES]
check(beyond and max(on[i] for i in beyond) == 0.0, "no light beyond Max Distance")

print("Map Range: Min Distance 3, Max Distance 6")
clight.map_range_min_distance = 3.0
on = render("min_distance")
compare("constant inside 3 m, Smooth Step to zero at 6 m", on, off,
        lambda d: attenuation(d, 'SMOOTHSTEP', 3.0, 6.0), 0.06)
clight.map_range_interpolation_type = 'LINEAR'
clight.map_range_min_distance = 2.0
clight.map_range_max_distance = 8.0
on = render("linear")
compare("Linear, Min 2, Max 8", on, off, lambda d: attenuation(d, 'LINEAR', 2.0, 8.0), 0.06)

print("Map Range: reversed fade (To Min 0, To Max 1)")
clight.map_range_interpolation_type = 'SMOOTHSTEP'
clight.map_range_min_distance = 2.0
clight.map_range_max_distance = 6.0
clight.map_range_to_min = 0.0
clight.map_range_to_max = 1.0
on = render("reversed")
compare("dark up to 2 m, rising to the usual light at 6 m and beyond", on, off,
        lambda d: attenuation(d, 'SMOOTHSTEP', 2.0, 6.0, to_min=0.0, to_max=1.0), 0.06)
far = [i for i in range(len(on)) if distance(i) > 6.0 + SIZE / RES and off[i] > 1e-4]
check(far and all(abs(on[i] / off[i] - 1.0) < 1e-3 for i in far), "beyond Max Distance the light is as usual")

print("Map Range: To Min 0.5, To Max 0.25")
clight.map_range_to_min = 0.5
clight.map_range_to_max = 0.25
on = render("half")
compare("half up to 2 m, a quarter from 6 m", on, off,
        lambda d: attenuation(d, 'SMOOTHSTEP', 2.0, 6.0, to_min=0.5, to_max=0.25), 0.06)
clight.map_range_to_min = 1.0
clight.map_range_to_max = 0.0
clight.use_map_range = False

print("Light Falloff")
check(not hasattr(clight, "light_falloff_strength"), "Light Falloff has no Strength (it would change the brightness)")
clight.use_light_falloff = True
for output, smooth in (('QUADRATIC', 0.0), ('LINEAR', 0.0), ('CONSTANT', 0.0), ('QUADRATIC', 4.0)):
    clight.light_falloff_output = output
    clight.light_falloff_smooth = smooth
    on = render("falloff_" + output.lower())
    compare(f"{output.title()}, Smooth {smooth}", on, off,
            lambda d: attenuation(d, None, 0.0, 0.0, output, smooth), 0.06)

print("Both: Map Range and Light Falloff")
clight.use_map_range = True
clight.map_range_interpolation_type = 'SMOOTHSTEP'
clight.map_range_min_distance = 2.0
clight.map_range_max_distance = 6.0
clight.light_falloff_output = 'LINEAR'
clight.light_falloff_smooth = 0.0
on = render("both")
compare("Min 2, Max 6, Linear falloff", on, off,
        lambda d: attenuation(d, 'SMOOTHSTEP', 2.0, 6.0, 'LINEAR'), 0.06)

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
clight.map_range_min_distance = 0.0
clight.map_range_max_distance = 2.0     # The camera is 9 m away: far beyond.
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
