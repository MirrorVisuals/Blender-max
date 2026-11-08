# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Unwrap modifier: UV sessions, as Unwrap UVW in 3ds Max.

Tab when the active (or hovered) modifier is an Unwrap, or its Edit UVs button: the object
temporarily uses a copy of the mesh as the Unwrap outputs it, in Edit Mode, and a UV Editor is
opened if none is visible. Every UV tool can be used. Tab again stores the UVs in the modifier by
stable IDs and puts the original mesh back. Faces without stored UVs (new topology below) are
selected when the session starts, with UV Sync Selection on, so they are easy to find.

Only UVs are kept: topology changed in an Unwrap session is not.
"""

import bmesh
import bpy
from bpy.types import Operator
from bpy.props import StringProperty

KEY_MESH = "_unwrap_mesh"
KEY_MODIFIER = "_unwrap_modifier"
KEY_HIDDEN = "_unwrap_hidden"
KEY_SYNC = "_unwrap_uv_sync"
KEY_FACES = "_unwrap_faces"

ATTR_NEW = ".unwrap_new"


def in_session(ob):
    return ob is not None and KEY_MESH in ob


def _uv_editor_visible(context):
    for window in context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'IMAGE_EDITOR' and area.ui_type == 'UV':
                return True
    return False


def _open_uv_editor(context):
    if bpy.app.background or _uv_editor_visible(context):
        return
    try:
        bpy.ops.wm.window_new()
        window = context.window_manager.windows[-1]
        area = max(window.screen.areas, key=lambda a: a.width * a.height)
        area.ui_type = 'UV'
    except Exception as ex:
        print("Unwrap: could not open a UV Editor:", ex)


def start_session(context, ob, level, open_editor=True, select_all=False):
    from bl_operators.edit_poly import evaluated_copy, enter_edit_mode_only
    mods = ob.modifiers[:]
    k = mods.index(level)
    visible = {mod.name: mod.show_viewport for mod in mods}

    # The level's result: stored UVs applied, new faces filled and flagged.
    for mod in mods[k + 1:]:
        mod.show_viewport = False
    level.show_viewport = True
    session_mesh = evaluated_copy(context, ob)
    for mod in mods[k + 1:]:
        mod.show_viewport = visible[mod.name]
    # While editing, the session mesh replaces everything up to and including the level.
    for mod in mods[:k + 1]:
        mod.show_viewport = False

    attr = session_mesh.attributes.get(ATTR_NEW)
    new_faces = [i for i, d in enumerate(attr.data) if d.value] if attr is not None else []

    original = ob.data
    session_mesh.name = original.name + " (Unwrap)"
    tool_settings = context.scene.tool_settings
    ob[KEY_MESH] = original
    ob[KEY_MODIFIER] = level.name
    ob[KEY_HIDDEN] = [mod.name for mod in mods[:k + 1] if visible[mod.name]]
    ob[KEY_SYNC] = tool_settings.use_uv_select_sync
    ob[KEY_FACES] = len(session_mesh.polygons)
    ob.data = session_mesh
    ob.modifiers.active = level

    enter_edit_mode_only(context, ob)
    bm = bmesh.from_edit_mesh(ob.data)
    bm.faces.ensure_lookup_table()
    new = set() if select_all else set(new_faces)
    for face in bm.faces:
        face.select_set(not new or face.index in new)
    bm.select_flush_mode()
    bmesh.update_edit_mesh(ob.data)
    if new or select_all:
        # Show every face in the UV Editor, with the new ones selected.
        tool_settings.use_uv_select_sync = True
    if open_editor:
        _open_uv_editor(context)
    return len(new_faces)


def end_session(context, ob):
    session_mesh = ob.data
    ob.data = ob[KEY_MESH]
    for name in ob[KEY_HIDDEN]:
        mod = ob.modifiers.get(name)
        if mod is not None:
            mod.show_viewport = True
    context.scene.tool_settings.use_uv_select_sync = bool(ob.get(KEY_SYNC, False))
    for key in (KEY_MESH, KEY_MODIFIER, KEY_HIDDEN, KEY_SYNC, KEY_FACES):
        if key in ob:
            del ob[key]
    if session_mesh is not None and session_mesh.users == 0:
        bpy.data.meshes.remove(session_mesh)


def commit_session(context, ob):
    """Store the session's UVs. Returns a warning, or None."""
    if ob.mode == 'EDIT':
        bpy.ops.object.mode_set(mode='OBJECT')
    warning = None
    if len(ob.data.polygons) != ob.get(KEY_FACES, len(ob.data.polygons)):
        warning = "Topology changes in an Unwrap level are not kept (only UVs are)"
    level = ob.modifiers.get(ob[KEY_MODIFIER])
    if level is not None and level.type == 'UNWRAP':
        level.session_commit(mesh=ob.data)
    end_session(context, ob)
    return warning


class _UnwrapOperator:
    bl_options = {'REGISTER', 'UNDO'}

    modifier: StringProperty(name="Modifier")

    def get(self, context):
        ob = context.active_object
        if ob is None or ob.type != 'MESH':
            return None, None
        mod = ob.modifiers.get(self.modifier) if self.modifier else ob.modifiers.active
        if mod is None or mod.type != 'UNWRAP':
            return None, None
        return ob, mod


class OBJECT_OT_unwrap_level_edit(_UnwrapOperator, Operator):
    """Edit the UVs of this Unwrap level in the UV Editor (Tab stores them)"""
    bl_idname = "object.unwrap_level_edit"
    bl_label = "Edit UVs"

    def execute(self, context):
        from bl_operators import edit_poly
        ob, mod = self.get(context)
        if mod is None:
            return {'CANCELLED'}
        if in_session(ob):
            if ob[KEY_MODIFIER] == mod.name:
                return {'CANCELLED'}
            warning = commit_session(context, ob)
            if warning:
                self.report({'WARNING'}, warning)
        if edit_poly.in_session(ob):
            self.report({'ERROR'}, "Leave the Edit Poly level first (Tab)")
            return {'CANCELLED'}
        if context.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        start_session(context, ob, mod)
        return {'FINISHED'}


class OBJECT_OT_unwrap_clear(_UnwrapOperator, Operator):
    """Remove the stored UVs of this Unwrap level"""
    bl_idname = "object.unwrap_clear"
    bl_label = "Reset Unwrap"

    def execute(self, context):
        ob, mod = self.get(context)
        if mod is None:
            return {'CANCELLED'}
        mod.clear()
        return {'FINISHED'}


class OBJECT_OT_unwrap_quick(_UnwrapOperator, Operator):
    """Unwrap or pack all UVs of this level without a UV session"""
    bl_idname = "object.unwrap_quick"
    bl_label = "Quick Unwrap"

    method: StringProperty(
        name="Method",
        description="SMART: Smart UV Project, UNWRAP: unwrap along the seams, PACK: pack islands",
        default='SMART',
    )

    @classmethod
    def description(cls, _context, properties):
        return {
            'SMART': "Smart UV Project all faces of this level (no seams needed)",
            'UNWRAP': "Unwrap all faces of this level along their seams",
            'PACK': "Pack the UV islands of this level into the 0..1 tile",
        }.get(properties.method, cls.__doc__)

    def execute(self, context):
        from bl_operators import edit_poly
        ob, mod = self.get(context)
        if mod is None:
            return {'CANCELLED'}
        if in_session(ob) or edit_poly.in_session(ob):
            self.report({'ERROR'}, "Leave the level being edited first (Tab)")
            return {'CANCELLED'}
        if context.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        start_session(context, ob, mod, open_editor=False, select_all=True)
        try:
            if self.method == 'SMART':
                bpy.ops.uv.smart_project()
            elif self.method == 'UNWRAP':
                bpy.ops.uv.unwrap()
            elif self.method == 'PACK':
                bpy.ops.uv.pack_islands()
            else:
                raise RuntimeError("Unknown method " + self.method)
        except RuntimeError as ex:
            if ob.mode == 'EDIT':
                bpy.ops.object.mode_set(mode='OBJECT')
            end_session(context, ob)
            self.report({'ERROR'}, str(ex).strip())
            return {'CANCELLED'}
        commit_session(context, ob)
        return {'FINISHED'}


classes = (
    OBJECT_OT_unwrap_level_edit,
    OBJECT_OT_unwrap_quick,
    OBJECT_OT_unwrap_clear,
)
