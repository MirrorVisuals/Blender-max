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
    view_layer.light_mix_value = 1.0
    view_layer.light_mix_exposure = 0.0
    view_layer.light_mix_color = (1.0, 1.0, 1.0)
    for group in (group_a, group_b):
        group.use_mix = True
        group.use_mix_solo = False
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

print("Solo and Master")
reset()
group_b.use_mix_solo = True
check(tuple(group_a.mix_weight) == (0.0, 0.0, 0.0), "a group that is not soloed has a weight of zero")
check(tuple(group_b.mix_weight) == (1.0, 1.0, 1.0), "the soloed group keeps its weight")
solo = render("solo")
ok, worst = close(solo, no_a)
check(ok, "solo B is the image without A (worst %.2e)" % worst)

reset()
view_layer.light_mix_value = 2.0
check(tuple(group_a.mix_weight) == (2.0, 2.0, 2.0), "the master multiplies the weight")
master = render("master")
expected = [b + la + lb for b, la, lb in zip(base, light_a, light_b)]
ok, worst = close(master, expected)
check(ok, "master value 2 doubles every group (worst %.2e)" % worst)

reset()
view_layer.light_mix_exposure = -1.0
group_a.mix_value = 4.0
check(tuple(group_a.mix_weight) == (2.0, 2.0, 2.0), "group value 4 with master exposure -1 is a weight of 2")

print("Finished render")


def result_pixels(name):
    path = os.path.join(bpy.app.tempdir, "light_mix_result_" + name + ".exr")
    bpy.data.images["Render Result"].save_render(path)
    img = bpy.data.images.load(path)
    px = list(img.pixels)
    bpy.data.images.remove(img)
    return px


reset()
bpy.ops.render.render()
ok, worst = close(result_pixels("neutral"), base)
check(ok, "a finished render with a neutral mix is the base (worst %.2e)" % worst)
group_a.use_mix = False
ok, worst = close(result_pixels("no_a"), no_a)
check(ok, "disabling A after the render removes it without rendering (worst %.2e)" % worst)
group_a.use_mix = True
group_b.mix_value = 2.0
expected = [b + lb for b, lb in zip(base, light_b)]
ok, worst = close(result_pixels("b_x2"), expected)
check(ok, "B x2 after the render, from the kept Combined pass (worst %.2e)" % worst)
view_layer.light_mix_value = 0.0
ok, worst = close(result_pixels("master_zero"), [b - la - lb for b, la, lb in zip(base, light_a, light_b)])
check(ok, "master zero after the render removes every group (worst %.2e)" % worst)
reset()
ok, worst = close(result_pixels("back"), base)
check(ok, "back to neutral gives the render again (worst %.2e)" % worst)
view_layer.use_light_mix = False
group_a.use_mix = False
ok, worst = close(result_pixels("off"), base)
check(ok, "turning Light Mix off after the render gives the render as rendered (worst %.2e)" % worst)

print("Bake")
reset()
group_a.mix_value = 0.5
group_b.mix_color = (1.0, 0.5, 0.25)
bpy.ops.cycles.light_mix_bake(after='NEUTRAL')
check(abs(bpy.data.lights["A"].energy - 150.0) < 1e-3, "bake halves the energy of light A")
check(tuple(round(c, 4) for c in bpy.data.lights["B"].color) == (1.0, 0.5, 0.25), "bake tints light B")
check(tuple(group_a.mix_weight) == (1.0, 1.0, 1.0), "the mix is neutral after the bake")
check(len(view_layer.lightgroups) == 2, "Keep and Reset keeps the light groups")
bpy.data.lights["A"].energy = 300.0
bpy.data.lights["B"].color = (1.0, 1.0, 1.0)

print("Import from the add-on")
reset()
scene["lightmix"] = {
    "groups": [{"name": "A", "value": 3.0, "exposure": 1.0, "tint": (1.0, 0.0, 0.0), "enabled": 1, "solo": 0}],
    "master_value": 0.5,
}
tree = bpy.data.node_groups.new("Compositing Nodes", "CompositorNodeTree")
scene.compositing_node_group = tree
render_layers = tree.nodes.new("CompositorNodeRLayers")
owned = tree.nodes.new("CompositorNodeRGB")
owned["lightmix_owner"] = 1
viewer = tree.nodes.new("CompositorNodeViewer")
tree.links.new(owned.outputs[0], viewer.inputs[0])
check(bpy.ops.cycles.light_mix_import_addon.poll(), "the add-on mix is found")
bpy.ops.cycles.light_mix_import_addon()
check(abs(group_a.mix_value - 3.0) < 1e-6 and abs(group_a.mix_exposure - 1.0) < 1e-6, "value and exposure imported")
check(tuple(group_a.mix_color) == (1.0, 0.0, 0.0), "tint imported")
check(abs(view_layer.light_mix_value - 0.5) < 1e-6, "master imported")
check(not any(n.get("lightmix_owner") for n in tree.nodes),
      "the add-on nodes are removed")
check(viewer.inputs[0].is_linked and viewer.inputs[0].links[0].from_node == render_layers,
      "what they fed gets the render again")
del scene["lightmix"]
scene.compositing_node_group = None

print("Scan")
for lightgroup in list(view_layer.lightgroups):
    view_layer.lightgroups.remove(lightgroup)
for ob in list(bpy.data.objects):
    ob.lightgroup = ""
lamp = bpy.data.lights.new("Lamp", 'POINT')
for i in range(3):
    ob = bpy.data.objects.new("Lamp" if i == 0 else "Lamp.%03d" % i, lamp)
    scene.collection.objects.link(ob)
copy = bpy.data.objects.new("Spot", bpy.data.lights.new("Spot", 'SPOT'))
scene.collection.objects.link(copy)
glow = bpy.data.materials.new("Glow")
glow.use_nodes = True
glow.node_tree.nodes["Principled BSDF"].inputs["Emission Strength"].default_value = 5.0
bpy.ops.mesh.primitive_cube_add()
bpy.context.object.data.materials.append(glow)
scene.world = bpy.data.worlds.new("World")
settings = scene.cycles_light_mix
settings.naming_mode = 'AUTO'
view_layer.use_light_mix = False
bpy.ops.cycles.light_mix_scan()
names = sorted(lightgroup.name for lightgroup in view_layer.lightgroups)
check("Lamp" in names, "linked duplicates share one group named after their data")
check(all(bpy.data.objects[n].lightgroup == "Lamp" for n in ("Lamp", "Lamp.001", "Lamp.002")),
      "every linked duplicate is assigned")
check("Spot" in names and "A" in names and "B" in names, "lights with their own data get their own group")
check("Glow" in names, "emissive objects are grouped per material")
check("World" in names and scene.world.lightgroup == "World", "the world gets a group")
check(view_layer.use_light_mix, "scanning turns Light Mix on")

settings.naming_mode = 'PREFIX'
bpy.data.objects.remove(bpy.context.object)
bpy.ops.cycles.light_mix_scan()
names = sorted(lightgroup.name for lightgroup in view_layer.lightgroups)
check("Lamp" in names and "Spot" in names, "name prefix grouping strips the .001 suffix")
check("Glow" not in names, "purge removes groups that lost their members")

bpy.ops.cycles.light_mix_select(group="Lamp")
check(sorted(ob.name for ob in bpy.context.selected_objects) == ["Lamp", "Lamp.001", "Lamp.002"],
      "Select Objects selects the members")

view_layer.lightgroups[0].mix_value = 3.0
view_layer.light_mix_value = 2.0
bpy.ops.cycles.light_mix_reset()
check(all(abs(lg.mix_value - 1.0) < 1e-6 for lg in view_layer.lightgroups) and view_layer.light_mix_value == 1.0,
      "Reset Mix returns everything to neutral")

bpy.ops.cycles.light_mix_clear_all()
check(len(view_layer.lightgroups) == 0, "Clear All removes the light groups")
check(not any(ob.lightgroup for ob in bpy.data.objects) and not scene.world.lightgroup, "and their assignments")

if failures:
    print("\n%d FAILED:" % len(failures))
    for f in failures:
        print("  " + f)
    print("RESULT: FAILED")
    sys.exit(1)
print("\nRESULT: ALL PASSED")
