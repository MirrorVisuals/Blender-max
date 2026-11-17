# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Light Mix: the combined image with each light group multiplied by its Color x Value x 2^Exposure
# (zero when disabled), made from the light group passes without changing what is rendered.
# The renders use the same seed, so they only differ by the mix: removing a group and doubling it
# are exact sums of the same samples.
# ./blender.bin --background --factory-startup --python tests/python/bl_cycles_light_mix.py
import sys
import os
import bpy

failures = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


RES = 32

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.samples = 8
scene.cycles.use_denoising = False
scene.cycles.use_adaptive_sampling = False
scene.cycles.seed = 1
scene.render.resolution_x = RES
scene.render.resolution_y = RES
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.use_compositing = False

bpy.ops.mesh.primitive_plane_add(size=10.0)
cam_data = bpy.data.cameras.new("Cam")
cam_data.type = 'ORTHO'
cam_data.ortho_scale = 10.0
cam = bpy.data.objects.new("Cam", cam_data)
scene.collection.objects.link(cam)
cam.location = (0, 0, 10)
scene.camera = cam

view_layer = scene.view_layers[0]
for name, x in (("A", -2.0), ("B", 2.0)):
    data = bpy.data.lights.new(name, 'POINT')
    data.energy = 300.0
    ob = bpy.data.objects.new(name, data)
    scene.collection.objects.link(ob)
    ob.location = (x, 0, 1.5)
    view_layer.lightgroups.add(name=name)
    ob.lightgroup = name
group_a = view_layer.lightgroups["A"]
group_b = view_layer.lightgroups["B"]


def render(name):
    path = os.path.join(bpy.app.tempdir, "light_mix_" + name + ".exr")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    px = list(img.pixels)
    bpy.data.images.remove(img)
    return px


def reset():
    view_layer.use_light_mix = True
    for group in (group_a, group_b):
        group.use_mix = True
        group.mix_value = 1.0
        group.mix_exposure = 0.0
        group.mix_color = (1.0, 1.0, 1.0)


def close(a, b, tol=1e-3):
    worst = max(abs(x - y) / max(1.0, abs(y)) for x, y in zip(a, b))
    return worst <= tol, worst


def mean(px, channel=None):
    values = [px[i] for i in range(len(px)) if i % 4 != 3 and (channel is None or i % 4 == channel)]
    return sum(values) / len(values)


print("Defaults")
check(tuple(group_a.mix_weight) == (1.0, 1.0, 1.0), "a new light group has a weight of one")
check(not view_layer.use_light_mix, "Light Mix is off by default")

view_layer.use_light_mix = False
base = render("base")
check(mean(base) > 0.0, "the scene is lit")

reset()
same = render("same")
ok, worst = close(same, base)
check(ok, "all weights one: the same image (worst %.2e)" % worst)

print("Groups")
reset()
group_a.use_mix = False
no_a = render("no_a")
reset()
group_b.use_mix = False
no_b = render("no_b")
check(mean(no_a) < mean(base) and mean(no_b) < mean(base), "a disabled group is removed")
light_a = [x - y for x, y in zip(base, no_a)]
light_b = [x - y for x, y in zip(base, no_b)]
check(mean(light_a) > 0.0 and mean(light_b) > 0.0, "each group has light")

reset()
group_a.use_mix = False
group_b.mix_value = 2.0
mixed = render("mixed")
expected = [b - a + lb for b, a, lb in zip(base, light_a, light_b)]
ok, worst = close(mixed, expected)
check(ok, "A off and B x2 is the base without A and with B again (worst %.2e)" % worst)

reset()
group_b.mix_exposure = 1.0
exposure = render("exposure")
expected = [b + lb for b, lb in zip(base, light_b)]
ok, worst = close(exposure, expected)
check(ok, "Exposure 1 doubles the group (worst %.2e)" % worst)

reset()
group_b.mix_color = (1.0, 0.0, 0.0)
tinted = render("tinted")
check(abs(mean(tinted, 0) - mean(base, 0)) < 1e-4, "red tint keeps the red of the group")
check(abs(mean(tinted, 1) - (mean(base, 1) - mean(light_b, 1))) < 1e-4, "red tint removes its green")

reset()
group_a.mix_value = 0.0
group_b.mix_value = 0.0
dark = render("dark")
check(mean(dark) < 1e-4, "both groups at zero: no light left")

print("Off")
reset()
group_a.use_mix = False
view_layer.use_light_mix = False
off = render("off")
ok, worst = close(off, base)
check(ok, "with Light Mix off the settings do nothing (worst %.2e)" % worst)

if failures:
    print("\n%d FAILED:" % len(failures))
    for f in failures:
        print("  " + f)
    print("RESULT: FAILED")
    sys.exit(1)
print("\nRESULT: ALL PASSED")
