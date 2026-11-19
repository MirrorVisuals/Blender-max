# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

"""
Decals: a material projected onto the surfaces inside a box, rendered by Cycles without changing
the geometry (labels, signs, stains, cracks...).

A decal is an empty (a Cube, an arrow for a cylinder, a Sphere) with the Decal option of its
Cycles settings and a material. There can be any number of them. It projects along its -Z axis
(planar), wraps around its Z axis (cylindrical) or is projected from its center outwards
(spherical). In the material, the default UV and the generated coordinates are the ones of the
decal, so an Image Texture and a Normal Map
follow it, and the alpha of the material shows the surface under it. See
intern/cycles/kernel/geom/decal.h.
"""

from __future__ import annotations

import math

import bpy
from bpy.app.translations import pgettext_tip as tip_
from bpy.props import EnumProperty, IntProperty, StringProperty
from bpy.types import Operator, Panel

from .properties import decal_display_type
from .ui import CyclesButtonsPanel, panel_node_draw


SHAPE_ICONS = {'PLANE': 'MOD_UVPROJECT', 'CYLINDER': 'MESH_CYLINDER', 'SPHERE': 'MESH_UVSPHERE'}

# Decals listed in the sidebar at most: past that, only the selected ones.
MAX_LISTED_DECALS = 30


def scene_decals(scene):
    """The decal empties of the scene, in the order they are layered."""
    return sorted(
        (ob for ob in scene.objects if ob.type == 'EMPTY' and ob.cycles.is_decal),
        key=lambda ob: (ob.cycles.decal_order, ob.name),
    )


def _decal(context, name):
    ob = context.scene.objects.get(name)
    if ob is None or ob.type != 'EMPTY' or not ob.cycles.is_decal:
        return None
    return ob


def new_decal_material(name="Decal"):
    """A material for decals: an Image Texture on the decal UV, its alpha showing the surface under it."""
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    bsdf = nodes.get("Principled BSDF")
    image = nodes.new("ShaderNodeTexImage")
    image.location = (bsdf.location.x - 400, bsdf.location.y)
    image.extension = 'CLIP'
    links.new(image.outputs["Color"], bsdf.inputs["Base Color"])
    links.new(image.outputs["Alpha"], bsdf.inputs["Alpha"])
    return mat


class CyclesDecalOperator:
    bl_options = {'REGISTER', 'UNDO'}

    decal: StringProperty(name="Decal", description="Name of the decal empty", options={'SKIP_SAVE'})

    def get_decal(self, context):
        if self.decal:
            return _decal(context, self.decal)
        ob = context.object
        return ob if ob is not None and ob.type == 'EMPTY' and ob.cycles.is_decal else None


class CYCLES_OT_decal_add(Operator):
    """Add a decal at the 3D cursor"""
    bl_idname = "cycles.decal_add"
    bl_label = "Add Decal"
    bl_options = {'REGISTER', 'UNDO'}

    shape: EnumProperty(
        name="Projection",
        items=(
            ('PLANE', "Planar", "Project along the -Z axis of the box"),
            ('CYLINDER', "Cylindrical", "Wrap around the Z axis of the box"),
            ('SPHERE', "Spherical", "Project from the center of the sphere outwards"),
        ),
        default='PLANE',
    )

    def execute(self, context):
        ob = bpy.data.objects.new("Decal", None)
        ob.empty_display_type = decal_display_type(self.shape)
        ob.empty_display_size = 1.0
        ob.location = context.scene.cursor.location
        if self.shape == 'PLANE':
            # A thin box, projecting down onto the floor: rotate it onto walls.
            ob.scale = (0.5, 0.5, 0.1)
        else:
            ob.scale = (0.5, 0.5, 0.5)
        if self.shape == 'SPHERE':
            # All around.
            ob.cycles.decal_span = 2.0 * math.pi
        ob.show_in_front = True
        ob.cycles.is_decal = True
        ob.cycles.decal_shape = self.shape
        ob.cycles.decal_material = new_decal_material()
        context.collection.objects.link(ob)
        for other in context.selected_objects:
            other.select_set(False)
        ob.select_set(True)
        context.view_layer.objects.active = ob
        return {'FINISHED'}


class CYCLES_OT_decal_material_new(CyclesDecalOperator, Operator):
    """Make a new material for the decal: an Image Texture on the decal UV, with its alpha"""
    bl_idname = "cycles.decal_material_new"
    bl_label = "New Decal Material"

    def execute(self, context):
        ob = self.get_decal(context)
        if ob is None:
            return {'CANCELLED'}
        ob.cycles.decal_material = new_decal_material()
        return {'FINISHED'}


class CYCLES_OT_decal_remove(CyclesDecalOperator, Operator):
    """Delete the decal"""
    bl_idname = "cycles.decal_remove"
    bl_label = "Remove Decal"

    def execute(self, context):
        ob = self.get_decal(context)
        if ob is None:
            return {'CANCELLED'}
        bpy.data.objects.remove(ob, do_unlink=True)
        return {'FINISHED'}


class CYCLES_OT_decal_select(CyclesDecalOperator, Operator):
    """Select the decal empty, to move, rotate or scale it"""
    bl_idname = "cycles.decal_select"
    bl_label = "Select Decal"

    def execute(self, context):
        ob = self.get_decal(context)
        if ob is None:
            return {'CANCELLED'}
        for other in context.selected_objects:
            other.select_set(False)
        try:
            ob.select_set(True)
        except RuntimeError:
            return {'CANCELLED'}
        context.view_layer.objects.active = ob
        return {'FINISHED'}


class CYCLES_OT_decal_add_selected(CyclesDecalOperator, Operator):
    """Add the selected objects to the list of the decal (excluded, or the only ones it is projected on)"""
    bl_idname = "cycles.decal_add_selected"
    bl_label = "Add Selected"

    def execute(self, context):
        ob = self.get_decal(context)
        if ob is None:
            return {'CANCELLED'}
        listed = ob.cycles.decal_objects
        existing = {entry.object for entry in listed}
        added = 0
        for other in context.selected_objects:
            if other == ob or other in existing:
                continue
            if other.type in {'LIGHT', 'CAMERA', 'SPEAKER', 'LIGHT_PROBE'}:
                continue
            if other.type == 'EMPTY' and other.instance_type != 'COLLECTION':
                continue
            listed.add().object = other
            added += 1
        self.report({'INFO'}, tip_("Added {:d} object(s)").format(added))
        return {'FINISHED'}


class CYCLES_OT_decal_object_remove(CyclesDecalOperator, Operator):
    """Remove the object from the list of the decal"""
    bl_idname = "cycles.decal_object_remove"
    bl_label = "Remove Object"

    index: IntProperty(name="Index", default=-1, options={'SKIP_SAVE'})

    def execute(self, context):
        ob = self.get_decal(context)
        if ob is None or not (0 <= self.index < len(ob.cycles.decal_objects)):
            return {'CANCELLED'}
        ob.cycles.decal_objects.remove(self.index)
        return {'FINISHED'}


class CYCLES_OT_decal_collection_add(CyclesDecalOperator, Operator):
    """Add the active collection to the list of the decal"""
    bl_idname = "cycles.decal_collection_add"
    bl_label = "Add Collection"

    def execute(self, context):
        ob = self.get_decal(context)
        if ob is None:
            return {'CANCELLED'}
        collection = context.collection
        if collection is None or collection == context.scene.collection:
            self.report({'WARNING'}, "Make a collection active in the Outliner first")
            return {'CANCELLED'}
        if any(entry.collection == collection for entry in ob.cycles.decal_collections):
            return {'CANCELLED'}
        ob.cycles.decal_collections.add().collection = collection
        return {'FINISHED'}


class CYCLES_OT_decal_collection_remove(CyclesDecalOperator, Operator):
    """Remove the collection from the list of the decal"""
    bl_idname = "cycles.decal_collection_remove"
    bl_label = "Remove Collection"

    index: IntProperty(name="Index", default=-1, options={'SKIP_SAVE'})

    def execute(self, context):
        ob = self.get_decal(context)
        if ob is None or not (0 <= self.index < len(ob.cycles.decal_collections)):
            return {'CANCELLED'}
        ob.cycles.decal_collections.remove(self.index)
        return {'FINISHED'}


def draw_decal_settings(layout, ob):
    """The settings of one decal."""
    cob = ob.cycles
    col = layout.column()
    col.active = cob.decal_enabled

    row = col.row(align=True)
    row.prop(cob, "decal_shape", expand=True)

    row = col.row(align=True)
    row.prop(cob, "decal_material", text="")
    row.operator("cycles.decal_material_new", text="", icon='ADD').decal = ob.name
    if cob.decal_material is None:
        col.label(text="Choose a material for the decal", icon='ERROR')
    else:
        # The material is edited here, or in the Shader Editor with the decal active.
        header, body = col.panel("CYCLES_decal_material_" + ob.name, default_closed=True)
        header.label(text="Material Settings", icon='MATERIAL')
        if body:
            body.label(text="Also in the Shader Editor, with the decal active", icon='NODETREE')
            panel_node_draw(body, cob.decal_material, 'Surface')

    sub = col.column(align=True)
    sub.prop(cob, "decal_opacity")
    sub.prop(cob, "decal_surface_bump")
    if cob.decal_shape in {'CYLINDER', 'SPHERE'}:
        col.prop(cob, "decal_span")

    box = col.box()
    box.label(text="Projection", icon='MOD_UVPROJECT')
    sub = box.column(align=True)
    sub.prop(cob, "decal_angle_limit")
    sub.prop(cob, "decal_angle_fade")
    sub.prop(cob, "decal_depth_fade")
    box.prop(cob, "decal_back_faces")
    box.prop(cob, "decal_order")

    box = col.box()
    row = box.row()
    row.label(text="Objects", icon='OBJECT_DATA')
    row.prop(cob, "decal_list_mode", text="")
    row = box.row(align=True)
    row.operator("cycles.decal_add_selected", text="Add Selected", icon='EYEDROPPER').decal = ob.name
    row.operator("cycles.decal_collection_add", text="Add Collection", icon='OUTLINER_COLLECTION').decal = ob.name
    for i, entry in enumerate(cob.decal_objects):
        row = box.row(align=True)
        row.prop(entry, "object", text="")
        op = row.operator("cycles.decal_object_remove", text="", icon='X')
        op.decal = ob.name
        op.index = i
    for i, entry in enumerate(cob.decal_collections):
        row = box.row(align=True)
        row.prop(entry, "collection", text="", icon='OUTLINER_COLLECTION')
        op = row.operator("cycles.decal_collection_remove", text="", icon='X')
        op.decal = ob.name
        op.index = i
    if not cob.decal_objects and not cob.decal_collections:
        if cob.decal_list_mode == 'EXCLUDE':
            box.label(text="Projected on every object in the box", icon='INFO')
        else:
            box.label(text="No objects: projected on nothing", icon='ERROR')


class CYCLES_VIEW3D_PT_decals(Panel):
    bl_label = "Decals"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Decals"

    @classmethod
    def poll(cls, context):
        return context.engine == 'CYCLES'

    def draw(self, context):
        layout = self.layout
        row = layout.row(align=True)
        row.scale_y = 1.4
        row.operator("cycles.decal_add", text="Planar", icon='MOD_UVPROJECT').shape = 'PLANE'
        row.operator("cycles.decal_add", text="Cylindrical", icon='MESH_CYLINDER').shape = 'CYLINDER'
        row.operator("cycles.decal_add", text="Spherical", icon='MESH_UVSPHERE').shape = 'SPHERE'

        decals = scene_decals(context.scene)
        if not decals:
            layout.label(text="No decals yet", icon='INFO')
            return

        active = context.active_object
        if active is not None and not (active.type == 'EMPTY' and active.cycles.is_decal):
            active = None

        # The list: one row per decal, click one to edit it.
        listed = decals
        if len(decals) > MAX_LISTED_DECALS:
            # Many decals (scattered stains, dirt...): only the selected ones are listed.
            layout.label(text=tip_("{:d} decals, the selected ones:").format(len(decals)), icon='INFO')
            listed = [ob for ob in decals if ob.select_get() or ob == active][:MAX_LISTED_DECALS]
        col = layout.box().column(align=True)
        for ob in listed:
            cob = ob.cycles
            row = col.row(align=True)
            row.prop(
                cob, "decal_enabled", text="", emboss=False,
                icon='HIDE_OFF' if cob.decal_enabled else 'HIDE_ON',
            )
            op = row.operator(
                "cycles.decal_select", text=ob.name, depress=(ob == active),
                icon=SHAPE_ICONS[cob.decal_shape],
            )
            op.decal = ob.name
            row.operator("cycles.decal_remove", text="", icon='X', emboss=False).decal = ob.name

        # The settings of the active decal only.
        if active is None:
            layout.label(text="Click a decal to edit it", icon='INFO')
            return
        box = layout.box()
        row = box.row()
        row.prop(active, "name", text="", icon='MOD_UVPROJECT')
        draw_decal_settings(box, active)


class CYCLES_OBJECT_PT_decal(CyclesButtonsPanel, Panel):
    bl_label = "Decal"
    bl_context = "object"

    @classmethod
    def poll(cls, context):
        return CyclesButtonsPanel.poll(context) and context.object is not None and context.object.type == 'EMPTY'

    def draw_header(self, context):
        self.layout.prop(context.object.cycles, "is_decal", text="")

    def draw(self, context):
        ob = context.object
        layout = self.layout
        layout.active = ob.cycles.is_decal
        layout.prop(ob.cycles, "decal_enabled")
        draw_decal_settings(layout, ob)


# Preview of cylindrical and spherical decals in the 3D View: the empty draws the cylinder or
# sphere, and the part their span covers is drawn over it.

_draw_handler = None
CYLINDER_SEGMENTS = 48


def _cylinder_lines(ob):
    """Lines of the cylinder of a decal, in world space: (covered, uncovered)."""
    import math
    from mathutils import Vector
    cob = ob.cycles
    size = ob.empty_display_size
    matrix = ob.matrix_world
    span = cob.decal_span
    covered = []
    uncovered = []

    def point(angle, z):
        # Angle 0 towards -Y, as in the kernel (kernel/geom/decal.h).
        return matrix @ Vector((math.sin(angle) * size, -math.cos(angle) * size, z * size))

    for i in range(CYLINDER_SEGMENTS):
        a0 = -math.pi + 2.0 * math.pi * i / CYLINDER_SEGMENTS
        a1 = -math.pi + 2.0 * math.pi * (i + 1) / CYLINDER_SEGMENTS
        mid = 0.5 * (a0 + a1)
        lines = covered if abs(mid) <= 0.5 * span else uncovered
        for z in (-1.0, 1.0):
            lines += [point(a0, z), point(a1, z)]
    # The edges of the span, and a few lines along the cylinder.
    for angle in (-0.5 * span, 0.5 * span):
        covered += [point(angle, -1.0), point(angle, 1.0)]
    for i in range(8):
        angle = -math.pi + 2.0 * math.pi * i / 8
        lines = covered if abs(angle) <= 0.5 * span else uncovered
        lines += [point(angle, -1.0), point(angle, 1.0)]
    # Where the decal is centered.
    covered += [matrix @ Vector((0.0, 0.0, 0.0)), point(0.0, 0.0)]
    return covered, uncovered


def _sphere_lines(ob):
    """Lines of the span of a spherical decal (its edges and center meridians), in world space."""
    import math
    from mathutils import Vector
    size = ob.empty_display_size
    matrix = ob.matrix_world
    span = ob.cycles.decal_span
    lines = []

    def point(angle, latitude):
        c = math.cos(latitude)
        return matrix @ Vector((math.sin(angle) * c * size, -math.cos(angle) * c * size, math.sin(latitude) * size))

    meridians = [0.0]
    if span < 2.0 * math.pi - 1e-3:
        meridians += [-0.5 * span, 0.5 * span]
    for angle in meridians:
        for i in range(CYLINDER_SEGMENTS // 2):
            l0 = -0.5 * math.pi + math.pi * i / (CYLINDER_SEGMENTS // 2)
            l1 = -0.5 * math.pi + math.pi * (i + 1) / (CYLINDER_SEGMENTS // 2)
            lines += [point(angle, l0), point(angle, l1)]
    # Where the decal is centered.
    lines += [matrix @ Vector((0.0, 0.0, 0.0)), point(0.0, 0.0)]
    return lines


def _draw_cylinders():
    context = bpy.context
    space = context.space_data
    if space is None or space.type != 'VIEW_3D' or not space.overlay.show_overlays:
        return
    if not space.overlay.show_extras:
        return
    scene = context.scene
    view_layer = context.view_layer
    if scene is None or view_layer is None:
        return
    active = view_layer.objects.active
    groups = {}
    for ob in scene.objects:
        if ob.type != 'EMPTY':
            continue
        cob = ob.cycles
        if not cob.is_decal or cob.decal_shape not in {'CYLINDER', 'SPHERE'}:
            continue
        if not ob.visible_get(view_layer=view_layer):
            continue
        if ob == active and ob.select_get():
            color = (1.0, 0.67, 0.25)
        elif ob.select_get():
            color = (0.95, 0.45, 0.1)
        else:
            color = (0.0, 0.0, 0.0)
        alpha = 1.0 if cob.decal_enabled else 0.35
        # The empty draws the whole cylinder or sphere: the part the decal covers is drawn over it.
        if cob.decal_shape == 'SPHERE':
            covered = _sphere_lines(ob)
        else:
            covered = _cylinder_lines(ob)[0]
        groups.setdefault((color, alpha, ob.show_in_front), []).append(covered)
    if not groups:
        return

    import gpu
    from gpu_extras.batch import batch_for_shader
    shader = gpu.shader.from_builtin('POLYLINE_UNIFORM_COLOR')
    region = context.region
    shader.uniform_float("viewportSize", (region.width, region.height))
    shader.uniform_float("lineWidth", 1.5)
    gpu.state.blend_set('ALPHA')
    for (color, alpha, in_front), parts in groups.items():
        coords = [p for part in parts for p in part]
        if not coords:
            continue
        gpu.state.depth_test_set('NONE' if in_front else 'LESS_EQUAL')
        batch = batch_for_shader(shader, 'LINES', {"pos": coords})
        shader.uniform_float("color", (*color, alpha))
        batch.draw(shader)
    gpu.state.depth_test_set('NONE')
    gpu.state.blend_set('NONE')


@bpy.app.handlers.persistent
def _update_display_types(*_args):
    # Decals made before cylindrical ones drew as a cylinder.
    for ob in bpy.data.objects:
        if ob.type == 'EMPTY' and ob.cycles.is_decal:
            display = decal_display_type(ob.cycles.decal_shape)
            if ob.empty_display_type != display and not ob.library:
                ob.empty_display_type = display


def draw_add_menu(self, context):
    if context.engine != 'CYCLES':
        return
    layout = self.layout
    layout.operator("cycles.decal_add", text="Decal", icon='MOD_UVPROJECT').shape = 'PLANE'
    layout.operator("cycles.decal_add", text="Cylindrical Decal", icon='MESH_CYLINDER').shape = 'CYLINDER'
    layout.operator("cycles.decal_add", text="Spherical Decal", icon='MESH_UVSPHERE').shape = 'SPHERE'


classes = (
    CYCLES_OT_decal_add,
    CYCLES_OT_decal_material_new,
    CYCLES_OT_decal_remove,
    CYCLES_OT_decal_select,
    CYCLES_OT_decal_add_selected,
    CYCLES_OT_decal_object_remove,
    CYCLES_OT_decal_collection_add,
    CYCLES_OT_decal_collection_remove,
    CYCLES_VIEW3D_PT_decals,
    CYCLES_OBJECT_PT_decal,
)


def register():
    global _draw_handler
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
    bpy.types.VIEW3D_MT_add.append(draw_add_menu)
    if not bpy.app.background:
        _draw_handler = bpy.types.SpaceView3D.draw_handler_add(_draw_cylinders, (), 'WINDOW', 'POST_VIEW')
    bpy.app.handlers.load_post.append(_update_display_types)


def unregister():
    global _draw_handler
    from bpy.utils import unregister_class
    if _update_display_types in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_update_display_types)
    if _draw_handler is not None:
        bpy.types.SpaceView3D.draw_handler_remove(_draw_handler, 'WINDOW')
        _draw_handler = None
    bpy.types.VIEW3D_MT_add.remove(draw_add_menu)
    for cls in reversed(classes):
        unregister_class(cls)
