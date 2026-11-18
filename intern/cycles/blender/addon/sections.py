# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

"""
Sections: architectural sections and cut-aways, rendered by Cycles without changing the geometry.

A section is an empty (a Plane empty, or a Cube empty for a box) with the Section option of its
Cycles settings. The camera sees the objects it cuts with the part on one side removed, and the
cut of closed objects filled (the cap), while the light still sees the whole scene unless Keep
Lighting is turned off. See intern/cycles/kernel/geom/section.h.
"""

from __future__ import annotations

import bpy
from bpy.app.translations import pgettext_tip as tip_
from bpy.props import EnumProperty, IntProperty, StringProperty
from bpy.types import Operator, Panel

from .ui import CyclesButtonsPanel


def scene_sections(scene):
    """The section empties of the scene, in a stable order."""
    return sorted(
        (ob for ob in scene.objects if ob.type == 'EMPTY' and ob.cycles.is_section),
        key=lambda ob: ob.name,
    )


def _section(context, name):
    ob = context.scene.objects.get(name)
    if ob is None or ob.type != 'EMPTY' or not ob.cycles.is_section:
        return None
    return ob


class CyclesSectionOperator:
    bl_options = {'REGISTER', 'UNDO'}

    section: StringProperty(name="Section", description="Name of the section empty", options={'SKIP_SAVE'})

    def get_section(self, context):
        if self.section:
            return _section(context, self.section)
        ob = context.object
        return ob if ob is not None and ob.type == 'EMPTY' and ob.cycles.is_section else None


class CYCLES_OT_section_add(Operator):
    """Add a section at the 3D cursor"""
    bl_idname = "cycles.section_add"
    bl_label = "Add Section"
    bl_options = {'REGISTER', 'UNDO'}

    shape: EnumProperty(
        name="Shape",
        items=(
            ('PLANE', "Plane", "Cut away everything on the side the normal points to"),
            ('BOX', "Box", "Cut away everything inside the box"),
        ),
        default='PLANE',
    )

    def execute(self, context):
        is_box = self.shape == 'BOX'
        ob = bpy.data.objects.new("Section Box" if is_box else "Section", None)
        ob.empty_display_type = 'CUBE' if is_box else 'PLANE'
        ob.empty_display_size = 1.0
        ob.location = context.scene.cursor.location
        ob.show_in_front = True
        ob.cycles.is_section = True
        ob.cycles.section_shape = self.shape
        context.collection.objects.link(ob)
        for other in context.selected_objects:
            other.select_set(False)
        ob.select_set(True)
        context.view_layer.objects.active = ob
        return {'FINISHED'}


class CYCLES_OT_section_remove(CyclesSectionOperator, Operator):
    """Delete the section"""
    bl_idname = "cycles.section_remove"
    bl_label = "Remove Section"

    def execute(self, context):
        ob = self.get_section(context)
        if ob is None:
            return {'CANCELLED'}
        bpy.data.objects.remove(ob, do_unlink=True)
        return {'FINISHED'}


class CYCLES_OT_section_select(CyclesSectionOperator, Operator):
    """Select the section empty, to move or rotate it"""
    bl_idname = "cycles.section_select"
    bl_label = "Select Section"

    def execute(self, context):
        ob = self.get_section(context)
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


class CYCLES_OT_section_add_selected(CyclesSectionOperator, Operator):
    """Make the section cut the selected objects (and only the objects and collections listed)"""
    bl_idname = "cycles.section_add_selected"
    bl_label = "Add Selected"

    def execute(self, context):
        ob = self.get_section(context)
        if ob is None:
            return {'CANCELLED'}
        targets = ob.cycles.section_targets
        existing = {entry.object for entry in targets}
        added = 0
        for other in context.selected_objects:
            if other == ob or other in existing:
                continue
            if other.type in {'LIGHT', 'CAMERA', 'SPEAKER', 'LIGHT_PROBE'}:
                continue
            if other.type == 'EMPTY' and other.instance_type != 'COLLECTION':
                continue
            targets.add().object = other
            added += 1
        self.report({'INFO'}, tip_("Added {:d} object(s)").format(added))
        return {'FINISHED'}


class CYCLES_OT_section_target_remove(CyclesSectionOperator, Operator):
    """Stop cutting this object"""
    bl_idname = "cycles.section_target_remove"
    bl_label = "Remove Target"

    index: IntProperty(options={'SKIP_SAVE'})

    def execute(self, context):
        ob = self.get_section(context)
        if ob is None or not 0 <= self.index < len(ob.cycles.section_targets):
            return {'CANCELLED'}
        ob.cycles.section_targets.remove(self.index)
        return {'FINISHED'}


class CYCLES_OT_section_collection_add(CyclesSectionOperator, Operator):
    """Make the section cut the objects of a collection"""
    bl_idname = "cycles.section_collection_add"
    bl_label = "Add Collection"

    def execute(self, context):
        ob = self.get_section(context)
        if ob is None:
            return {'CANCELLED'}
        entry = ob.cycles.section_target_collections.add()
        collection = context.collection
        if collection is not None and collection != context.scene.collection:
            entry.collection = collection
        return {'FINISHED'}


class CYCLES_OT_section_collection_remove(CyclesSectionOperator, Operator):
    """Stop cutting the objects of this collection"""
    bl_idname = "cycles.section_collection_remove"
    bl_label = "Remove Collection"

    index: IntProperty(options={'SKIP_SAVE'})

    def execute(self, context):
        ob = self.get_section(context)
        if ob is None or not 0 <= self.index < len(ob.cycles.section_target_collections):
            return {'CANCELLED'}
        ob.cycles.section_target_collections.remove(self.index)
        return {'FINISHED'}


class CYCLES_OT_section_clean_all(Operator):
    """Delete every section of the scene"""
    bl_idname = "cycles.section_clean_all"
    bl_label = "Clean All Sections"
    bl_options = {'REGISTER', 'UNDO'}

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(self, event)

    def execute(self, context):
        sections = scene_sections(context.scene)
        for ob in sections:
            bpy.data.objects.remove(ob, do_unlink=True)
        self.report({'INFO'}, tip_("Removed {:d} section(s)").format(len(sections)))
        return {'FINISHED'}


def draw_section_settings(layout, ob):
    """The settings of one section, as in the ArchViz Clipper add-on."""
    cob = ob.cycles
    col = layout.column()
    col.active = cob.section_enabled

    row = col.row(align=True)
    row.prop(cob, "section_shape", expand=True)
    if cob.section_shape == 'BOX':
        keeping = "Keeping: Inside the Box" if cob.section_invert else "Keeping: Outside the Box"
    else:
        # The plane's normal (the line the empty draws) points to the side that is cut away.
        keeping = "Keeping: Above the Plane" if cob.section_invert else "Keeping: Below the Plane"
    col.prop(cob, "section_invert", text=keeping, toggle=True, icon='ARROW_LEFTRIGHT')

    box = col.box()
    row = box.row()
    row.prop(cob, "section_cap", text="Cut Surface (Cap)")
    sub = box.column()
    sub.active = cob.section_cap
    sub.prop(cob, "section_cap_mode", text="")
    if cob.section_cap_mode != 'MATERIAL':
        sub.prop(cob, "section_cap_color", text="Color")

    box = col.box()
    box.label(text="Target Objects", icon='OBJECT_DATA')
    row = box.row(align=True)
    row.operator("cycles.section_add_selected", text="Add Selected", icon='EYEDROPPER').section = ob.name
    row.operator("cycles.section_collection_add", text="Add Collection", icon='OUTLINER_COLLECTION').section = ob.name
    for i, entry in enumerate(cob.section_targets):
        row = box.row(align=True)
        row.prop(entry, "object", text="")
        op = row.operator("cycles.section_target_remove", text="", icon='X')
        op.section = ob.name
        op.index = i
    for i, entry in enumerate(cob.section_target_collections):
        row = box.row(align=True)
        row.prop(entry, "collection", text="", icon='OUTLINER_COLLECTION')
        op = row.operator("cycles.section_collection_remove", text="", icon='X')
        op.section = ob.name
        op.index = i
    if not cob.section_targets and not cob.section_target_collections:
        box.label(text="No targets: cuts every object", icon='INFO')

    box = col.box()
    box.label(text="Lighting", icon='LIGHT_SUN')
    box.prop(cob, "section_affect_lighting")
    box.prop(cob, "section_camera_only")


class CYCLES_VIEW3D_PT_sections(Panel):
    bl_label = "Sections"
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Sections"

    @classmethod
    def poll(cls, context):
        return context.engine == 'CYCLES'

    def draw(self, context):
        layout = self.layout
        row = layout.row(align=True)
        row.scale_y = 1.4
        row.operator("cycles.section_add", text="Add Section", icon='MESH_PLANE').shape = 'PLANE'
        row.operator("cycles.section_add", text="Add Box", icon='MESH_CUBE').shape = 'BOX'

        sections = scene_sections(context.scene)
        if not sections:
            layout.label(text="No sections yet", icon='INFO')
            return

        for ob in sections:
            cob = ob.cycles
            box = layout.box()
            header = box.row(align=True)
            header.prop(
                cob, "section_ui_expanded", text="", emboss=False,
                icon='TRIA_DOWN' if cob.section_ui_expanded else 'TRIA_RIGHT',
            )
            header.prop(
                cob, "section_enabled", text="", emboss=False,
                icon='HIDE_OFF' if cob.section_enabled else 'HIDE_ON',
            )
            header.prop(ob, "name", text="")
            header.operator("cycles.section_select", text="", icon='RESTRICT_SELECT_OFF').section = ob.name
            header.operator("cycles.section_remove", text="", icon='X').section = ob.name
            if cob.section_ui_expanded:
                draw_section_settings(box, ob)

        layout.separator()
        layout.operator("cycles.section_clean_all", icon='TRASH')


class CYCLES_OBJECT_PT_section(CyclesButtonsPanel, Panel):
    bl_label = "Section"
    bl_context = "object"

    @classmethod
    def poll(cls, context):
        return CyclesButtonsPanel.poll(context) and context.object is not None and context.object.type == 'EMPTY'

    def draw_header(self, context):
        self.layout.prop(context.object.cycles, "is_section", text="")

    def draw(self, context):
        ob = context.object
        layout = self.layout
        layout.active = ob.cycles.is_section
        layout.prop(ob.cycles, "section_enabled")
        draw_section_settings(layout, ob)


def draw_add_menu(self, context):
    if context.engine != 'CYCLES':
        return
    layout = self.layout
    layout.separator()
    layout.operator("cycles.section_add", text="Section", icon='MESH_PLANE').shape = 'PLANE'
    layout.operator("cycles.section_add", text="Section Box", icon='MESH_CUBE').shape = 'BOX'


classes = (
    CYCLES_OT_section_add,
    CYCLES_OT_section_remove,
    CYCLES_OT_section_select,
    CYCLES_OT_section_add_selected,
    CYCLES_OT_section_target_remove,
    CYCLES_OT_section_collection_add,
    CYCLES_OT_section_collection_remove,
    CYCLES_OT_section_clean_all,
    CYCLES_VIEW3D_PT_sections,
    CYCLES_OBJECT_PT_section,
)


def register():
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
    bpy.types.VIEW3D_MT_add.append(draw_add_menu)


def unregister():
    from bpy.utils import unregister_class
    bpy.types.VIEW3D_MT_add.remove(draw_add_menu)
    for cls in reversed(classes):
        unregister_class(cls)
