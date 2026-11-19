# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Caustics per material: with the caustics of Light Paths off for the scene, a material can still
# let light through it make caustics (its Reflective / Refractive Caustics settings).
# A floor under a glass slab, lit by a white sky only, seen from above: the floor under the slab is
# lit only through the glass, a refractive caustic.
# ./blender.bin --background --factory-startup --python tests/python/bl_cycles_material_caustics.py
import os
import sys

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
scene.cycles.samples = 64
scene.cycles.use_denoising = False
scene.cycles.use_adaptive_sampling = False
scene.cycles.max_bounces = 8
scene.cycles.transmission_bounces = 8
scene.cycles.seed = 1
scene.render.resolution_x = RES
scene.render.resolution_y = RES
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.use_compositing = False

world = bpy.data.worlds.new("World")
scene.world = world
world.use_nodes = True
world.node_tree.nodes["Background"].inputs[0].default_value = (1, 1, 1, 1)
world.node_tree.nodes["Background"].inputs[1].default_value = 1.0

bpy.ops.mesh.primitive_plane_add(size=20.0)

# A wide glass slab above the floor.
bpy.ops.mesh.primitive_cube_add(location=(0, 0, 0.5))
slab = bpy.context.object
slab.scale = (3.0, 3.0, 0.05)
glass = bpy.data.materials.new("Glass")
glass.use_nodes = True
nt = glass.node_tree
nt.nodes.clear()
out = nt.nodes.new("ShaderNodeOutputMaterial")
bsdf = nt.nodes.new("ShaderNodeBsdfGlass")
bsdf.inputs["Roughness"].default_value = 0.0
bsdf.inputs["IOR"].default_value = 1.33
nt.links.new(bsdf.outputs[0], out.inputs["Surface"])
slab.data.materials.append(glass)

cam_data = bpy.data.cameras.new("Cam")
cam_data.type = 'ORTHO'
cam_data.ortho_scale = 1.0
cam = bpy.data.objects.new("Cam", cam_data)
scene.collection.objects.link(cam)
cam.location = (0, 0, 10)
scene.camera = cam


def render(name):
    path = os.path.join(bpy.app.tempdir, "material_caustics_" + name + ".exr")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    px = list(img.pixels)
    bpy.data.images.remove(img)
    return sum(px[i] + px[i + 1] + px[i + 2] for i in range(0, len(px), 4)) / (3.0 * RES * RES)


print("Caustics on for the scene")
scene.cycles.caustics_refractive = True
scene.cycles.caustics_reflective = True
on = render("scene_on")
check(on > 0.1, "the floor under the glass is lit through it (%.3f)" % on)

print("Caustics off for the scene")
scene.cycles.caustics_refractive = False
scene.cycles.caustics_reflective = False
off = render("scene_off")
check(off < 0.3 * on, "the floor under the glass gets no light through it (%.3f of %.3f)" % (off, on))

print("Refractive caustics of the glass only")
glass.cycles.use_caustics_refractive = True
material = render("material_on")
check(material > 0.8 * on, "the glass lets its caustics through again (%.3f of %.3f)" % (material, on))
glass.cycles.use_caustics_refractive = False

if failures:
    print("\n%d FAILED:" % len(failures))
    for f in failures:
        print("  " + f)
    print("RESULT: FAILED")
    sys.exit(1)
print("\nRESULT: ALL PASSED")
