# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# Sections: a section empty cuts the objects for the camera, fills the cut of closed objects (the
# cap), and keeps the lighting of the whole scene unless Keep Lighting is turned off.
# A room (a cube shell) under the sun, seen from above, cut through its walls by the section.
# ./blender.bin --background --factory-startup --python tests/python/bl_cycles_sections.py
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

# The ground, just below the room so they do not overlap.
bpy.ops.mesh.primitive_plane_add(size=20.0, location=(0, 0, -0.01))
floor = bpy.context.object

# The room: a 4 m cube shell with 0.2 m thick walls, floor and roof, from z = 0 to 4.
bpy.ops.mesh.primitive_cube_add(size=4.0, location=(0, 0, 2))
room = bpy.context.object
solidify = room.modifiers.new("Walls", 'SOLIDIFY')
solidify.thickness = 0.2
solidify.offset = -1.0
solidify.use_even_offset = True

cam_data = bpy.data.cameras.new("Cam")
cam_data.type = 'ORTHO'
cam_data.ortho_scale = VIEW
cam = bpy.data.objects.new("Cam", cam_data)
scene.collection.objects.link(cam)
cam.location = (0, 0, 20)
scene.camera = cam

sun_data = bpy.data.lights.new("Sun", 'SUN')
sun_data.energy = 3.0
sun_data.angle = 0.0
sun = bpy.data.objects.new("Sun", sun_data)
scene.collection.objects.link(sun)
sun.rotation_euler = (0.05, 0.05, 0.0)


def pixel_of(x, y):
    """Pixel index of a point seen from above."""
    px = int((x + VIEW / 2) / VIEW * RES)
    py = int((y + VIEW / 2) / VIEW * RES)
    return py * RES + px


def render(name):
    path = os.path.join(bpy.app.tempdir, "sections_" + name + ".exr")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    px = list(img.pixels)
    bpy.data.images.remove(img)

    def at(x, y):
        i = pixel_of(x, y) * 4
        return px[i], px[i + 1], px[i + 2]
    return at


def brightness(rgb):
    return sum(rgb) / 3.0


CENTER = (0.0, 0.3)      # Inside the room.
WALL = (1.9, 0.3)        # In the wall (x from 1.8 to 2).
OUTSIDE = (2.6, 0.3)     # The ground outside the room.

print("Without a section")
closed = render("closed")
roof = brightness(closed(*CENTER))
ground = brightness(closed(*OUTSIDE))
check(roof > 0.1 and ground > 0.1, "the roof and the ground are lit (%.3f, %.3f)" % (roof, ground))

print("Section plane, through the walls")
bpy.ops.cycles.section_add(shape='PLANE')
section = bpy.context.object
section.location = (0, 0, 3)
cs = section.cycles
check(cs.is_section and cs.section_enabled and cs.section_affect_lighting, "a new section, keeping the lighting")
check(section.empty_display_type == 'PLANE', "drawn as a plane")
check(cs.section_cap_mode == 'MATERIAL', "caps are the object's material by default")

# The walls: a lit red material, to tell the caps apart.
walls = bpy.data.materials.new("Walls")
walls.use_nodes = True
walls.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.8, 0.05, 0.05, 1.0)
room.data.materials.append(walls)
material_cut = render("material_cap")
material_cap = material_cut(*WALL)
check(material_cap[0] > 0.1 and material_cap[0] > 3.0 * material_cap[1],
      "material cap: the wall's material, lit by the sun (%.3f, %.3f, %.3f)" % material_cap)

capmat = bpy.data.materials.new("Cap")
capmat.use_nodes = True
capmat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.05, 0.8, 0.05, 1.0)
cs.section_cap_mode = 'CUSTOM'
cs.section_cap_material = capmat
custom_cut = render("custom_cap")
custom_cap = custom_cut(*WALL)
check(custom_cap[1] > 0.1 and custom_cap[1] > 3.0 * custom_cap[0],
      "custom cap material: the chosen material, lit (%.3f, %.3f, %.3f)" % custom_cap)
check(abs(brightness(custom_cut(*OUTSIDE)) - ground) < 0.02 * ground + 1e-4, "the ground is unchanged")
cs.section_cap_mode = 'FLAT'
room.data.materials.clear()

cut = render("cut")
inside = brightness(cut(*CENTER))
check(inside < 0.05 * roof, "inside, the roof that is cut away still shadows the floor (%.4f)" % inside)
cap = cut(*WALL)
check(abs(cap[0] - 0.8) < 0.02 and abs(cap[1] - 0.2) < 0.02 and abs(cap[2] - 0.1) < 0.02,
      "the cut of the wall is filled with the flat cap color (%.3f, %.3f, %.3f)" % cap)
check(abs(brightness(cut(*OUTSIDE)) - ground) < 0.02 * ground + 1e-4, "the ground outside is as before")

cs.section_affect_lighting = False
open_cut = render("open")
check(brightness(open_cut(*CENTER)) > 0.5 * roof,
      "Keep Lighting off: the sun comes in through the cut (%.3f)" % brightness(open_cut(*CENTER)))
cs.section_affect_lighting = True

cs.section_cap = False
no_cap = render("no_cap")
check(abs(no_cap(*WALL)[0] - 0.8) > 0.05, "no cap: the cut is not filled")
cs.section_cap = True

cs.section_cap_mode = 'LIT'
lit = render("lit")
lit_cap = lit(*WALL)
check(
    lit_cap[0] > 0.05 and lit_cap[0] > 2.0 *
    lit_cap[1],
    "lit cap: lit by the sun, in the cap color (%.3f, %.3f, %.3f)" %
    lit_cap)
cs.section_cap_mode = 'FLAT'

cs.section_invert = True
inverted = render("inverted")
check(brightness(inverted(*OUTSIDE)) < 1e-4, "inverted: only above the plane is kept, the ground is cut away")
check(brightness(inverted(*CENTER)) > 0.5 * roof, "and the roof is seen")
cs.section_invert = False

print("Targets")
cs.section_targets.add().object = floor
targeted = render("targeted")
check(abs(brightness(targeted(*CENTER)) - roof) < 0.02 * roof, "a section cuts only its target objects")
cs.section_targets.clear()

cs.section_enabled = False
disabled = render("disabled")
check(abs(brightness(disabled(*CENTER)) - roof) < 0.02 * roof, "a disabled section cuts nothing")
cs.section_enabled = True

print("Section box")
cs.section_shape = 'BOX'
check(section.empty_display_type == 'CUBE', "a box is drawn as a cube")
section.location = (0, 0, 4)
section.empty_display_size = 1.0
box = render("box")
check(brightness(box(*CENTER)) < 0.05 * roof, "the box cuts the roof away inside it, the room stays shadowed")
check(abs(brightness(box(1.5, 0.3)) - roof) < 0.05 * roof, "outside the box the roof is kept")

bpy.ops.cycles.section_clean_all()
check(not any(ob.cycles.is_section for ob in scene.objects), "Clean All removes the sections")

if failures:
    print("\n%d FAILED:" % len(failures))
    for f in failures:
        print("  " + f)
    print("RESULT: FAILED")
    sys.exit(1)
print("\nRESULT: ALL PASSED")
