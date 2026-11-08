# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
UVW Map modifier tools, as in 3ds Max.

The gizmo of a UVW Map is edited like a sub-object: Tab over the modifier (or its Edit Gizmo
button) selects a box-shaped helper that is the gizmo, and the normal Move / Rotate / Scale
tools move the map. Tab on the helper puts its transform back into the modifier and removes the
helper. Fit, Center and View Align work on the faces the modifier maps.
"""

import bpy
from bpy.types import Operator
from bpy.props import StringProperty
from mathutils import Euler, Matrix, Vector

KEY_GIZMO_FOR = "_uvw_gizmo_for"
KEY_GIZMO_MODIFIER = "_uvw_gizmo_modifier"


# -----------------------------------------------------------------------------
# Gizmo transform (object space)

def gizmo_matrix(ob, mod):
    """Gizmo box to object space; the box spans [-1, 1]."""
    gizmo = mod.gizmo_object
    if gizmo is not None:
        size = gizmo.empty_display_size if gizmo.type == 'EMPTY' else 1.0
        return ob.matrix_world.inverted_safe() @ gizmo.matrix_world @ Matrix.Scale(max(size, 1e-6), 4)
    half = Vector(mod.gizmo_size) * 0.5
    return Matrix.LocRotScale(Vector(mod.gizmo_location), Euler(mod.gizmo_rotation, 'XYZ'), half)


def set_gizmo(ob, mod, location, rotation, size):
    """Set the gizmo (object space location, rotation quaternion, full size)."""
    size = Vector([s if abs(s) > 1e-6 else 1e-6 for s in size])
    gizmo = mod.gizmo_object
    if gizmo is not None:
        display = gizmo.empty_display_size if gizmo.type == 'EMPTY' else 1.0
        local = Matrix.LocRotScale(location, rotation, size * (0.5 / max(display, 1e-6)))
        gizmo.matrix_world = ob.matrix_world @ local
        return
    mod.gizmo_location = location
    mod.gizmo_rotation = rotation.to_euler('XYZ')
    mod.gizmo_size = size


def current_gizmo(ob, mod):
    location, rotation, half = gizmo_matrix(ob, mod).decompose()
    return location, rotation, half * 2.0


# -----------------------------------------------------------------------------
# The mesh as it arrives at the modifier

def input_points(context, ob, mod):
    """Object space positions of the vertices the modifier maps."""
    mods = ob.modifiers[:]
    k = mods.index(mod)
    visible = [m.show_viewport for m in mods]
    for m in mods[k:]:
        m.show_viewport = False
    try:
        depsgraph = context.evaluated_depsgraph_get()
        depsgraph.update()
        mesh = bpy.data.meshes.new_from_object(
            ob.evaluated_get(depsgraph), preserve_all_data_layers=True, depsgraph=depsgraph)
    finally:
        for m, v in zip(mods, visible):
            m.show_viewport = v
    try:
        faces = mesh.polygons
        if mod.use_selection:
            faces = [f for f in faces if f.select]
        verts = {v for f in faces for v in f.vertices}
        if not verts:
            verts = range(len(mesh.vertices))
        return [mesh.vertices[v].co.copy() for v in verts]
    finally:
        bpy.data.meshes.remove(mesh)


def fit(context, ob, mod, keep_size):
    points = input_points(context, ob, mod)
    if not points:
        return False
    _location, rotation, size = current_gizmo(ob, mod)
    to_local = rotation.to_matrix().transposed()
    local = [to_local @ p for p in points]
    lo = Vector([min(p[i] for p in local) for i in range(3)])
    hi = Vector([max(p[i] for p in local) for i in range(3)])
    center = rotation.to_matrix() @ ((lo + hi) * 0.5)
    if not keep_size:
        size = Vector([max(hi[i] - lo[i], 1e-4) for i in range(3)])
    set_gizmo(ob, mod, center, rotation, size)
    return True


def view_align(context, ob, mod):
    region = None
    if context.region_data is not None:
        region = context.region_data
    else:
        for area in context.screen.areas if context.screen else ():
            if area.type == 'VIEW_3D':
                region = area.spaces.active.region_3d
                break
    if region is None:
        return False
    location, _rotation, size = current_gizmo(ob, mod)
    view = region.view_rotation
    object_rotation = ob.matrix_world.to_quaternion()
    rotation = object_rotation.inverted() @ view
    # Turn the gizmo so that its U, V and projection axes are the view's right, up and depth
    # (the modifier reads them from gizmo axes Y Z X for X alignment and X Z -Y for Y).
    if mod.axis == 'X':
        rotation = rotation @ Matrix(((0, 1, 0), (0, 0, 1), (1, 0, 0))).to_quaternion()
    elif mod.axis == 'Y':
        rotation = rotation @ Matrix(((1, 0, 0), (0, 0, 1), (0, -1, 0))).to_quaternion()
    set_gizmo(ob, mod, location, rotation, size)
    return True


# -----------------------------------------------------------------------------
# Gizmo editing (Tab)

def is_gizmo(ob):
    return ob is not None and KEY_GIZMO_FOR in ob


def _select_only(context, ob):
    for o in context.selected_objects:
        o.select_set(False)
    ob.select_set(True)
    context.view_layer.objects.active = ob


def start_gizmo_edit(context, ob, mod):
    """Select the gizmo of `mod`, making a helper empty for it if it has none."""
    gizmo = mod.gizmo_object
    if gizmo is None:
        location, rotation, size = current_gizmo(ob, mod)
        gizmo = bpy.data.objects.new(ob.name + " UVW Gizmo", None)
        gizmo.empty_display_type = 'CUBE'
        gizmo.empty_display_size = 1.0
        gizmo.show_in_front = True
        for collection in ob.users_collection:
            collection.objects.link(gizmo)
        gizmo.parent = ob
        gizmo.matrix_parent_inverse = Matrix.Identity(4)
        gizmo.matrix_basis = Matrix.LocRotScale(location, rotation, size * 0.5)
        gizmo[KEY_GIZMO_FOR] = ob.name
        gizmo[KEY_GIZMO_MODIFIER] = mod.name
        mod.gizmo_object = gizmo
    ob.modifiers.active = mod
    _select_only(context, gizmo)
    return gizmo


def finish_gizmo_edit(context, gizmo):
    """Store the helper's transform in its modifier, remove the helper, select the object."""
    ob = bpy.data.objects.get(gizmo.get(KEY_GIZMO_FOR, ""))
    mod = ob.modifiers.get(gizmo.get(KEY_GIZMO_MODIFIER, "")) if ob is not None else None
    if mod is not None and mod.type == 'UVW_MAP' and mod.gizmo_object == gizmo:
        location, rotation, size = current_gizmo(ob, mod)
        mod.gizmo_object = None
        set_gizmo(ob, mod, location, rotation, size)
    bpy.data.objects.remove(gizmo)
    if ob is not None:
        _select_only(context, ob)
        if mod is not None:
            ob.modifiers.active = mod
    return ob


# -----------------------------------------------------------------------------
# Operators

class _UVWMapOperator:
    bl_options = {'REGISTER', 'UNDO'}

    modifier: StringProperty(name="Modifier")

    def get(self, context):
        ob = context.active_object
        if is_gizmo(ob):
            ob = bpy.data.objects.get(ob[KEY_GIZMO_FOR])
        if ob is None or ob.type != 'MESH':
            return None, None
        mod = ob.modifiers.get(self.modifier) if self.modifier else ob.modifiers.active
        if mod is None or mod.type != 'UVW_MAP':
            return None, None
        return ob, mod


class OBJECT_OT_uvw_map_gizmo_edit(_UVWMapOperator, Operator):
    """Edit the map gizmo with Move / Rotate / Scale (Tab returns to the object)"""
    bl_idname = "object.uvw_map_gizmo_edit"
    bl_label = "Edit UVW Map Gizmo"

    def execute(self, context):
        if is_gizmo(context.active_object):
            finish_gizmo_edit(context, context.active_object)
            return {'FINISHED'}
        ob, mod = self.get(context)
        if mod is None:
            return {'CANCELLED'}
        if context.mode != 'OBJECT':
            bpy.ops.object.mode_set(mode='OBJECT')
        start_gizmo_edit(context, ob, mod)
        return {'FINISHED'}


class OBJECT_OT_uvw_map_fit(_UVWMapOperator, Operator):
    """Fit the gizmo to the faces the modifier maps"""
    bl_idname = "object.uvw_map_fit"
    bl_label = "Fit UVW Map Gizmo"

    def execute(self, context):
        ob, mod = self.get(context)
        if mod is None or not fit(context, ob, mod, keep_size=False):
            return {'CANCELLED'}
        return {'FINISHED'}


class OBJECT_OT_uvw_map_center(_UVWMapOperator, Operator):
    """Center the gizmo on the faces the modifier maps"""
    bl_idname = "object.uvw_map_center"
    bl_label = "Center UVW Map Gizmo"

    def execute(self, context):
        ob, mod = self.get(context)
        if mod is None or not fit(context, ob, mod, keep_size=True):
            return {'CANCELLED'}
        return {'FINISHED'}


class OBJECT_OT_uvw_map_view_align(_UVWMapOperator, Operator):
    """Turn the gizmo so that it projects along the view of the 3D Viewport"""
    bl_idname = "object.uvw_map_view_align"
    bl_label = "View Align UVW Map Gizmo"

    def execute(self, context):
        ob, mod = self.get(context)
        if mod is None:
            return {'CANCELLED'}
        if not view_align(context, ob, mod):
            self.report({'ERROR'}, "No 3D Viewport to align to")
            return {'CANCELLED'}
        return {'FINISHED'}


classes = (
    OBJECT_OT_uvw_map_gizmo_edit,
    OBJECT_OT_uvw_map_fit,
    OBJECT_OT_uvw_map_center,
    OBJECT_OT_uvw_map_view_align,
)
