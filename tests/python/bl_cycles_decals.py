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
import time

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


def pixel_of(x, y, res=RES):
    px = int((x + VIEW / 2) / VIEW * res)
    py = int((y + VIEW / 2) / VIEW * res)
    return py * res + px


def render(name, res=RES):
    path = os.path.join(bpy.app.tempdir, "decals_" + name + ".exr")
    scene.render.filepath = path
    scene.render.resolution_x = res
    scene.render.resolution_y = res
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    px = list(img.pixels)
    bpy.data.images.remove(img)

    def at(x, y):
        i = pixel_of(x, y, res) * 4
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


def emission_material(name, color):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    emission = nt.nodes.new("ShaderNodeEmission")
    emission.inputs["Color"].default_value = (*color, 1.0)
    nt.links.new(emission.outputs[0], out.inputs["Surface"])
    return mat


def new_decal(name, location, scale, material, order=0):
    """A decal made without the operator (fast, for many)."""
    ob = bpy.data.objects.new(name, None)
    ob.empty_display_type = 'CUBE'
    ob.location = location
    ob.scale = scale
    ob.cycles.is_decal = True
    ob.cycles.decal_material = material
    ob.cycles.decal_order = order
    scene.collection.objects.link(ob)
    return ob


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

print("Spherical decal")
bpy.ops.cycles.decal_add(shape='SPHERE')
sph = bpy.context.object
check(sph.empty_display_type == 'SPHERE', "a spherical decal draws as a sphere")
sph.cycles.decal_material = uv_material("UV4")
sph.cycles.decal_span = math.pi
# Center below the ground, its front (local -Y) up: U is the longitude along X, V the latitude
# along Y.
sph.location = (0.0, 0.0, -0.5)
sph.rotation_euler = (math.radians(-90.0), 0.0, 0.0)
sph.scale = (1.0, 1.0, 1.0)
sph.empty_display_size = 1.0
ball = render("sphere")
center = ball(0.0, 0.0)
# At the pixel center, a little off the front.
pc = (int((0.0 + VIEW / 2) / VIEW * RES) + 0.5) * VIEW / RES - VIEW / 2
expected = (0.5 + math.atan2(pc, 0.5) / math.pi, 0.5 + math.asin(pc / math.sqrt(2 * pc * pc + 0.25)) / math.pi)
check(abs(center[0] - expected[0]) < 0.03 and abs(center[1] - expected[1]) < 0.03,
      "at its front: U = V = 0.5 (%.3f, %.3f), expected (%.3f, %.3f)" % (center[:2] + expected))
cx = (int((0.5 + VIEW / 2) / VIEW * RES) + 0.5) * VIEW / RES - VIEW / 2
cy = (int((0.0 + VIEW / 2) / VIEW * RES) + 0.5) * VIEW / RES - VIEW / 2
expected_u = 0.5 + math.atan2(cx, 0.5) / math.pi
check(abs(ball(0.5, 0.0)[0] - expected_u) < 0.03,
      "U follows the longitude (%.3f, expected %.3f)" % (ball(0.5, 0.0)[0], expected_u))
cx = (int((0.0 + VIEW / 2) / VIEW * RES) + 0.5) * VIEW / RES - VIEW / 2
cy = (int((0.5 + VIEW / 2) / VIEW * RES) + 0.5) * VIEW / RES - VIEW / 2
expected_v = 0.5 + math.asin(cy / math.sqrt(cx * cx + cy * cy + 0.25)) / math.pi
check(abs(ball(0.0, 0.5)[1] - expected_v) < 0.03,
      "V follows the latitude (%.3f, expected %.3f)" % (ball(0.0, 0.5)[1], expected_v))
check(close(ball(1.2, 0.0), plain(1.2, 0.0), 0.01 * g + 1e-4), "outside the sphere: no decal")
bpy.data.objects.remove(sph, do_unlink=True)

print("Order")
red = emission_material("Red", (1.0, 0.0, 0.0))
green = emission_material("Green", (0.0, 1.0, 0.0))
bottom = new_decal("Bottom", (0, 0, 0), (1, 1, 1), red, order=0)
top = new_decal("Top", (0, 0, 0), (1, 1, 1), green, order=1)
layered = render("order")(*CENTER)
check(layered[1] > 0.9 and layered[0] < 0.05, "the decal with the higher order is on top (%.3f, %.3f, %.3f)" % layered)
top.cycles.decal_order = -1
layered = render("order_swapped")(*CENTER)
check(layered[0] > 0.9 and layered[1] < 0.05, "changing the order swaps them (%.3f, %.3f, %.3f)" % layered)
bpy.data.objects.remove(bottom, do_unlink=True)
bpy.data.objects.remove(top, do_unlink=True)

print("Layered materials")
# Lit decals over each other need more closures than any one material: none may be lost.


def lit_material(name, color, alpha=1.0):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes["Principled BSDF"]
    bsdf.inputs["Base Color"].default_value = (*color, 1.0)
    bsdf.inputs["Alpha"].default_value = alpha
    return mat


upper = new_decal("Upper", (0, 0, 0), (1, 1, 1), lit_material("White", (0.9, 0.9, 0.9)), order=1)
alone = render("lit_alone")(*CENTER)
lower = new_decal("Lower", (0, 0, 0), (1, 1, 1), lit_material("Blue", (0.1, 0.1, 0.9), 0.5), order=0)
layered = render("lit_layered")(*CENTER)
check(close(layered, alone, 0.03 * sum(alone) / 3.0 + 1e-3),
      "a lit decal over another one is shaded as alone (%.3f, %.3f, %.3f)" % layered)
bpy.data.objects.remove(upper, do_unlink=True)
bpy.data.objects.remove(lower, do_unlink=True)

print("Many decals on one point")
stack = []
for i in range(40):
    shade = (i + 1) / 40.0
    stack.append(new_decal("Stack%02d" % i, (0, 0, 0), (1, 1, 1), emission_material("S%02d" % i, (shade, 0, 0)),
                           order=i))
stacked = render("stack")(*CENTER)
check(abs(stacked[0] - 1.0) < 0.02, "40 decals on one point: the top one shows (%.3f)" % stacked[0])
for ob in stack:
    bpy.data.objects.remove(ob, do_unlink=True)

print("Thousands of decals")
GRID = 60
SPACING = 5.0 / GRID
HALF = 0.3 * SPACING
uv_many = uv_material("UVMany")
for ix in range(GRID):
    for iy in range(GRID):
        new_decal("Many", (-2.5 + (ix + 0.5) * SPACING, -2.5 + (iy + 0.5) * SPACING, 0.0), (HALF, HALF, 0.5),
                  uv_many)
num = sum(1 for ob in scene.objects if ob.type == 'EMPTY' and ob.cycles.is_decal)
check(num == GRID * GRID, "%d decals" % num)
scene.cycles.samples = 1
start = time.time()
many = render("many", 600)
many_time = time.time() - start
for ob in [ob for ob in scene.objects if ob.type == 'EMPTY' and ob.cycles.is_decal]:
    bpy.data.objects.remove(ob, do_unlink=True)
start = time.time()
none = render("many_none", 600)
none_time = time.time() - start
print("  render with %d decals: %.2f s, without: %.2f s" % (GRID * GRID, many_time, none_time))
checked = 0
ok_centers = 0
ok_gaps = 0
for ix in range(0, GRID, 7):
    for iy in range(0, GRID, 11):
        cx = -2.5 + (ix + 0.5) * SPACING
        cy = -2.5 + (iy + 0.5) * SPACING
        if abs(cx) > 2.9 or abs(cy) > 2.9:
            continue
        checked += 1
        c = many(cx, cy)
        ok_centers += abs(c[0] - 0.5) < 0.15 and abs(c[1] - 0.5) < 0.15
        gap = many(cx + 0.5 * SPACING, cy + 0.5 * SPACING)
        ok_gaps += close(gap, none(cx + 0.5 * SPACING, cy + 0.5 * SPACING), 0.01 * g + 1e-4)
check(checked > 20 and ok_centers == checked, "every decal shows its own UV at its center (%d of %d)" %
      (ok_centers, checked))
check(ok_gaps == checked, "and the ground between them is unchanged (%d of %d)" % (ok_gaps, checked))

if failures:
    print("\n%d FAILED:" % len(failures))
    for f in failures:
        print("  " + f)
    print("RESULT: FAILED")
    sys.exit(1)
print("\nRESULT: ALL PASSED")
