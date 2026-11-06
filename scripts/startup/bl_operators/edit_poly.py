# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Edit Poly stack, phase 3: Tab edits an Edit Poly modifier level instead of the base mesh.

Tab picks a level:
- the active modifier, if it is an Edit Poly;
- otherwise the nearest Edit Poly below the active modifier;
- otherwise the topmost Edit Poly;
- otherwise a new Edit Poly is added at the top of the stack.

Entering a level starts a session: the object temporarily uses a copy of the mesh as it arrives
at that level (with the level's moves applied), and the modifiers up to and including the level
are hidden because their result is already in that copy. Modifiers above stay visible, which
shows the end result. Clicking another Edit Poly while editing, then Tab (or its Edit Level button), records the
current level and switches to that one. Tab over a modifier panel acts on that modifier. Tab again records how far each vertex moved as moves of the level and
puts the original mesh back. The base mesh is never edited.

Any tool can be used. Moves of existing vertices become moves; topology changes become the
level's patch: removed elements and new geometry anchored to the vertices of the mesh below, so
it follows when the mesh below is deformed. If the topology below changes (e.g. a Subdivision
level), the patch pauses; such a level cannot be edited until the mesh below matches again.
"""

import bpy
from bpy.types import Operator
from bpy.props import StringProperty

KEY_MESH = "_edit_poly_mesh"
KEY_INPUT = "_edit_poly_input"
KEY_MODIFIER = "_edit_poly_modifier"
KEY_HIDDEN = "_edit_poly_hidden"

# Tab on an object without an Edit Poly adds one, so the base mesh is never edited directly.
AUTO_ADD = True


def in_session(ob):
    return ob is not None and KEY_MESH in ob


def pick_level(ob):
    mods = ob.modifiers
    active = mods.active
    if active is not None:
        if active.type == 'EDIT_POLY':
            return active
        index = mods.find(active.name)
        for mod in reversed(mods[:index]):
            if mod.type == 'EDIT_POLY':
                return mod
    for mod in reversed(mods[:]):
        if mod.type == 'EDIT_POLY':
            return mod
    return None


def evaluated_copy(context, ob):
    depsgraph = context.evaluated_depsgraph_get()
    depsgraph.update()
    return bpy.data.meshes.new_from_object(
        ob.evaluated_get(depsgraph), preserve_all_data_layers=True, depsgraph=depsgraph)


def enter_edit_mode_only(context, ob):
    """Enter Edit Mode on `ob` alone, without pulling other selected meshes in."""
    others = [o for o in context.selected_objects if o != ob]
    for o in others:
        o.select_set(False)
    ob.select_set(True)
    context.view_layer.objects.active = ob
    bpy.ops.object.mode_set(mode='EDIT')
    for o in others:
        o.select_set(True)


def start_session(context, ob, level):
    """Returns an error message if the level cannot be edited, else None."""
    mods = ob.modifiers[:]
    k = mods.index(level)
    visible = {mod.name: mod.show_viewport for mod in mods}

    # The mesh as it arrives at the level.
    for mod in mods[k:]:
        mod.show_viewport = False
    input_mesh = evaluated_copy(context, ob)
    try:
        level.session_check(input=input_mesh)
    except RuntimeError as ex:
        bpy.data.meshes.remove(input_mesh)
        for mod in mods:
            mod.show_viewport = visible[mod.name]
        return str(ex).strip()

    # The level's current result (its patch and moves applied) is what gets edited.
    level.show_viewport = True
    session_mesh = evaluated_copy(context, ob)
    level.session_prepare(input=input_mesh, mesh=session_mesh)
    input_mesh.name = ob.data.name + " (Edit Poly input)"

    # While editing, the session mesh replaces everything up to and including the level.
    for mod in mods[:k + 1]:
        mod.show_viewport = False
    for mod in mods[k + 1:]:
        mod.show_viewport = visible[mod.name]

    original = ob.data
    session_mesh.name = original.name + " (Edit Poly)"
    ob[KEY_MESH] = original
    ob[KEY_INPUT] = input_mesh
    ob[KEY_MODIFIER] = level.name
    ob[KEY_HIDDEN] = [mod.name for mod in mods[:k + 1] if visible[mod.name]]
    ob.data = session_mesh
    ob.modifiers.active = level

    enter_edit_mode_only(context, ob)
    return None


def end_session(ob):
    """Put the original mesh and modifier visibility back."""
    session_mesh = ob.data
    input_mesh = ob.get(KEY_INPUT)
    ob.data = ob[KEY_MESH]
    for name in ob[KEY_HIDDEN]:
        mod = ob.modifiers.get(name)
        if mod is not None:
            mod.show_viewport = True
    for key in (KEY_MESH, KEY_INPUT, KEY_MODIFIER, KEY_HIDDEN):
        if key in ob:
            del ob[key]
    for mesh in (session_mesh, input_mesh):
        if mesh is not None and mesh.users == 0:
            bpy.data.meshes.remove(mesh)


def commit_session(context, ob):
    """Record the session as moves. Returns an error message, or None."""
    if ob.mode == 'EDIT':
        bpy.ops.object.mode_set(mode='OBJECT')
    level = ob.modifiers.get(ob[KEY_MODIFIER])
    if level is None or level.type != 'EDIT_POLY':
        end_session(ob)
        return None  # The level was removed while editing: nothing to record into.
    try:
        level.session_commit(input=ob[KEY_INPUT], mesh=ob.data)
    except RuntimeError as ex:
        return str(ex).strip()
    end_session(ob)
    return None


class OBJECT_OT_edit_poly_tab(Operator):
    """Edit the active Edit Poly level (Tab). Moves are stored in the modifier, """ \
        """the base mesh is never edited"""
    bl_idname = "object.edit_poly_tab"
    bl_label = "Toggle Edit Poly Level"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        ob = context.active_object
        in_viewport = context.area is None or context.area.type == 'VIEW_3D'

        # Tab over a modifier panel in the Properties editor acts on that modifier.
        hovered = getattr(context, "modifier", None)
        if ob is not None and hovered is not None and hovered.id_data == ob:
            ob.modifiers.active = hovered

        if in_session(ob):
            # Clicking another Edit Poly in the stack while editing, then Tab, switches levels.
            active = ob.modifiers.active
            switch_to = None
            if active is not None and active.type == 'EDIT_POLY' and active.name != ob[KEY_MODIFIER]:
                switch_to = active.name
            error = commit_session(context, ob)
            if error:
                self.report({'ERROR'}, error)
                if ob.mode != 'EDIT':
                    enter_edit_mode_only(context, ob)
                return {'CANCELLED'}
            if switch_to is not None:
                error = start_session(context, ob, ob.modifiers[switch_to])
                if error:
                    self.report({'ERROR'}, error)
            return {'FINISHED'}

        # Everything that is not a mesh in Object Mode behaves as before. Outside the 3D Viewport
        # Tab had no meaning, so leave it alone there.
        if ob is None or ob.type != 'MESH' or context.mode != 'OBJECT' or ob.data.library:
            if not in_viewport:
                return {'PASS_THROUGH'}
            return bpy.ops.object.mode_set(mode='EDIT', toggle=True)

        level = pick_level(ob)
        if level is None:
            if not AUTO_ADD:
                return bpy.ops.object.mode_set(mode='EDIT', toggle=True)
            level = ob.modifiers.new("Edit Poly", 'EDIT_POLY')
        error = start_session(context, ob, level)
        if error:
            self.report({'ERROR'}, error)
            return {'CANCELLED'}
        return {'FINISHED'}


class OBJECT_OT_edit_poly_session_discard(Operator):
    """Leave the Edit Poly session without recording anything"""
    bl_idname = "object.edit_poly_session_discard"
    bl_label = "Discard Edit Poly Session"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return in_session(context.active_object)

    def execute(self, context):
        ob = context.active_object
        if ob.mode == 'EDIT':
            bpy.ops.object.mode_set(mode='OBJECT')
        end_session(ob)
        return {'FINISHED'}


class OBJECT_OT_edit_poly_level_edit(Operator):
    """Edit this Edit Poly level"""
    bl_idname = "object.edit_poly_level_edit"
    bl_label = "Edit Level"
    bl_options = {'REGISTER', 'UNDO'}

    modifier: StringProperty(name="Modifier")

    def execute(self, context):
        ob = context.active_object
        if ob is None or ob.type != 'MESH':
            return {'CANCELLED'}
        mod = ob.modifiers.get(self.modifier)
        if mod is None or mod.type != 'EDIT_POLY':
            return {'CANCELLED'}
        if in_session(ob):
            if ob[KEY_MODIFIER] == mod.name:
                return {'CANCELLED'}  # Already editing this level.
            error = commit_session(context, ob)
            if error:
                self.report({'ERROR'}, error)
                if ob.mode != 'EDIT':
                    enter_edit_mode_only(context, ob)
                return {'CANCELLED'}
        if context.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        ob.modifiers.active = mod
        error = start_session(context, ob, mod)
        if error:
            self.report({'ERROR'}, error)
            return {'CANCELLED'}
        return {'FINISHED'}


classes = (
    OBJECT_OT_edit_poly_tab,
    OBJECT_OT_edit_poly_session_discard,
    OBJECT_OT_edit_poly_level_edit,
)
