# Full Visible Surface check (Cycles): an area light with a narrow Spread, seen from outside its
# beam, shows its whole surface only with the option on; nothing else in the image changes.
# Run with CHECK_LIGHT_SURFACE.bat (Blender in background mode).
import math, os
import bpy

failures = []
def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.cycles.samples = 8
scene.cycles.use_denoising = False
scene.cycles.seed = 1
scene.render.resolution_x = 64
scene.render.resolution_y = 64
scene.render.image_settings.file_format = 'OPEN_EXR'

bpy.ops.mesh.primitive_plane_add(size=20, location=(0, 0, -1))
cam_data = bpy.data.cameras.new("Cam")
cam = bpy.data.objects.new("Cam", cam_data)
scene.collection.objects.link(cam)
cam.location = (0, -6, 0)
cam.rotation_euler = (math.radians(90), 0, 0)
scene.camera = cam

light_data = bpy.data.lights.new("Softbox", 'AREA')
light_data.size = 2.0
light_data.energy = 500.0
light_data.spread = math.radians(15)
light = bpy.data.objects.new("Softbox", light_data)
scene.collection.objects.link(light)
light.location = (0, 0, 0.5)
light.rotation_euler = (math.radians(-60), 0, 0)   # Faces 30 degrees away from the camera.
light.visible_camera = True

def render(name):
    path = os.path.join(bpy.app.tempdir, name + ".exr")
    scene.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    px = list(img.pixels)
    bpy.data.images.remove(img)
    return [sum(px[i:i + 3]) / 3.0 for i in range(0, len(px), 4)]

check(hasattr(light_data, "use_full_visible_surface"), "the light has the Full Visible Surface option")
light_data.use_full_visible_surface = False
off = render("surface_off")
light_data.use_full_visible_surface = True
on = render("surface_on")

changed = [i for i in range(len(off)) if abs(on[i] - off[i]) > 1e-6]
check(len(changed) > 4, f"with the option the light's surface shows ({len(changed)} pixels changed)")
check(all(on[i] > off[i] for i in changed), "those pixels only got brighter")
# Reference: the same light without spread (180 degrees), option off.
light_data.use_full_visible_surface = False
light_data.spread = math.radians(180)
reference = render("surface_reference")
# Compare only where the light is seen: elsewhere the narrow beam makes a brighter hot spot on
# the floor, which is not the light's surface.
worst = max(abs(on[i] - reference[i]) / max(reference[i], 1e-6) for i in changed) if changed else 1.0
inner = max(on[i] for i in changed) if changed else 0.0
check(worst < 0.02,
      f"the surface has the brightness it has without spread (surface {inner:.1f}, "
      f"without spread {max(reference[i] for i in changed):.1f}, largest difference {worst * 100:.2f}%)")
check(len(changed) < 0.25 * len(off), "the rest of the image (the light it casts) is unchanged")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
