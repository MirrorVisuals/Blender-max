# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Decals: an empty projects a material onto the surfaces inside its box, with its own UV, layered
# over the material of the surface by its alpha and opacity.
# A ground plane lit by the sun, seen from above, with decals of an emission material showing
# their UV, so the coordinates can be read back from the pixels.
# ./blender.bin --background --factory-startup --python tests/python/bl_cycles_decals.py
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
VIEW = 6.0

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.samples = 4
scene.cycles.use_denoising = False
scene.cycles.use_adaptive_sampling = False
scene.cycles.max_bounces = 0
scene.render.resolution_x = RES
scene.render.resolution_y = RES
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_depth = '32'
scene.render.use_compositing = False

bpy.ops.mesh.primitive_plane_add(size=20.0, location=(0, 0, 0))
ground = bpy.context.object

cam_data = bpy.data.cameras.new("Cam")
cam_data.type = 'ORTHO'
cam_data.ortho_scale = VIEW
cam = bpy.data.objects.new("Cam", cam_data)
scene.collection.objects.link(cam)
cam.location = (0, 0, 10)
scene.camera = cam

sun_data = bpy.data.lights.new("Sun", 'SUN')
sun_data.energy = 3.0
sun_data.angle = 0.0
sun = bpy.data.objects.new("Sun", sun_data)
scene.collection.objects.link(sun)


def pixel_of(x, y):
    px = int((x + VIEW / 2) / VIEW * RES)
    py = int((y + VIEW / 2) / VIEW * RES)
    return py * RES + px


def render(name):
    path = os.path.join(bpy.app.tempdir, "decals_" + name + ".exr")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    px = list(img.pixels)
    bpy.data.images.remove(img)

    def at(x, y):
        i = pixel_of(x, y) * 4
        return px[i], px[i + 1], px[i + 2]
    return at


def close(a, b, tol):
    return all(abs(x - y) < tol for x, y in zip(a, b))


def uv_material(name):
    """Emission of the decal UV: red = U, green = V."""
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    emission = nt.nodes.new("ShaderNodeEmission")
    texco = nt.nodes.new("ShaderNodeTexCoord")
    nt.links.new(texco.outputs["UV"], emission.inputs["Color"])
    nt.links.new(emission.outputs["Emission"], out.inputs["Surface"])
    return mat


def half_transparent_material(name, color):
    """Half emission, half transparent."""
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    emission = nt.nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (*color, 1.0)
    transparent = nt.nodes.new("ShaderNodeBsdfTransparent")
    mix = nt.nodes.new("ShaderNodeMixShader")
    mix.inputs["Fac"].default_value = 0.5
    nt.links.new(transparent.outputs[0], mix.inputs[1])
    nt.links.new(emission.outputs[0], mix.inputs[2])
    nt.links.new(mix.outputs[0], out.inputs["Surface"])
    return mat


CENTER = (0.0, 0.0)
POINT = (1.0, -1.0)      # In the decal box (-2 to 2).
OUTSIDE = (2.6, 0.3)     # Outside the decal box.

print("Without a decal")
plain = render("plain")
ground_rgb = plain(*CENTER)
g = sum(ground_rgb) / 3.0
check(g > 0.1, "the ground is lit (%.3f)" % g)

print("Planar decal")
bpy.ops.cycles.decal_add(shape='PLANE')
decal = bpy.context.object
cd = decal.cycles
check(cd.is_decal and cd.decal_enabled and decal.empty_display_type == 'CUBE', "a new decal, drawn as a cube")
check(cd.decal_material is not None, "with a new decal material")
decal.location = (0, 0, 0)
decal.scale = (2.0, 2.0, 1.0)
cd.decal_material = uv_material("UV")
cut = render("planar")
uv = cut(*POINT)
# The pixel center of POINT.
px = (int((POINT[0] + VIEW / 2) / VIEW * RES) + 0.5) * VIEW / RES - VIEW / 2
py = (int((POINT[1] + VIEW / 2) / VIEW * RES) + 0.5) * VIEW / RES - VIEW / 2
expected = ((px / 2.0 + 1.0) / 2.0, (py / 2.0 + 1.0) / 2.0, 0.0)
check(close(uv, expected, 0.02), "the decal shows its UV (%.3f, %.3f, %.3f), expected (%.3f, %.3f)" %
      (uv + expected[:2]))
check(close(cut(*OUTSIDE), plain(*OUTSIDE), 0.01 * g + 1e-4), "outside the box, the ground is unchanged")

cd.decal_material = half_transparent_material("Half", (0.0, 0.0, 1.0))
half = render("half")
expected = (0.5 * ground_rgb[0], 0.5 * ground_rgb[1], 0.5 * ground_rgb[2] + 0.5)
check(close(half(*CENTER), expected, 0.03),
      "alpha: half of the ground shows through (%.3f, %.3f, %.3f)" % half(*CENTER))

cd.decal_material = uv_material("UV2")
cd.decal_opacity = 0.5
faded = render("opacity")
expected = (0.5 * ground_rgb[0] + 0.5 * uv[0], 0.5 * ground_rgb[1] + 0.5 * uv[1], 0.5 * ground_rgb[2])
check(close(faded(*POINT), expected, 0.03), "opacity 0.5 (%.3f, %.3f, %.3f)" % faded(*POINT))
cd.decal_opacity = 1.0

print("Objects")
cd.decal_objects.add().object = ground
excluded = render("excluded")
check(close(excluded(*POINT), plain(*POINT), 0.01 * g + 1e-4), "an excluded object has no decal")
cd.decal_list_mode = 'INCLUDE'
included = render("included")
check(close(included(*POINT), uv, 0.02), "an included object has the decal")
cd.decal_objects.clear()
none_included = render("none_included")
check(close(none_included(*POINT), plain(*POINT), 0.01 * g + 1e-4), "include with no objects: no decal")
cd.decal_list_mode = 'EXCLUDE'

print("Angles")
decal.rotation_euler = (math.radians(90.0), 0.0, 0.0)
decal.scale = (2.0, 1.0, 2.0)
side = render("side")
check(close(side(*CENTER), plain(*CENTER), 0.01 * g + 1e-4),
      "projected along the ground, beyond the angle limit: no decal")
decal.rotation_euler = (math.radians(180.0), 0.0, 0.0)
decal.scale = (2.0, 2.0, 1.0)
flipped = render("flipped")
check(close(flipped(*CENTER), plain(*CENTER), 0.01 * g + 1e-4), "projected from below: no decal on the top")
cd.decal_back_faces = True
back = render("back_faces")
check(not close(back(*CENTER), plain(*CENTER), 0.05), "with Back Faces it is projected")
cd.decal_back_faces = False
decal.rotation_euler = (0.0, 0.0, 0.0)

cd.decal_enabled = False
disabled = render("disabled")
check(close(disabled(*POINT), plain(*POINT), 0.01 * g + 1e-4), "a disabled decal is not projected")
cd.decal_enabled = True

print("Cylindrical decal")
bpy.data.objects.remove(decal, do_unlink=True)
bpy.ops.cycles.decal_add(shape='CYLINDER')
cyl = bpy.context.object
cyl.cycles.decal_material = uv_material("UV3")
# Axis along +Y, below the ground: its angle 0 (local -Y) points up.
cyl.location = (0.0, 0.0, -0.5)
cyl.rotation_euler = (math.radians(-90.0), 0.0, 0.0)
cyl.scale = (1.0, 1.0, 1.0)
cyl.empty_display_size = 1.0
wrapped = render("cylinder")
top = wrapped(0.0, 0.0)
check(abs(top[0] - 0.5) < 0.03 and abs(top[1] - 0.5) < 0.05, "above the axis: U = 0.5 (%.3f, %.3f)" % top[:2])
side_px = (int((0.5 + VIEW / 2) / VIEW * RES) + 0.5) * VIEW / RES - VIEW / 2
expected_u = 0.5 + math.atan2(side_px, 0.5) / math.pi
side_uv = wrapped(0.5, 0.0)
check(abs(side_uv[0] - expected_u) < 0.03,
      "around the axis, U follows the angle (%.3f, expected %.3f)" % (side_uv[0], expected_u))
check(close(wrapped(1.2, 0.0), plain(1.2, 0.0), 0.01 * g + 1e-4), "outside the cylinder: no decal")

bpy.ops.cycles.decal_remove(decal=cyl.name)
check(not any(ob.cycles.is_decal for ob in scene.objects), "the decal is removed")

if failures:
    print("\n%d FAILED:" % len(failures))
    for f in failures:
        print("  " + f)
    print("RESULT: FAILED")
    sys.exit(1)
print("\nRESULT: ALL PASSED")
