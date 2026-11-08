# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
UVW Map modifier tools, as in 3ds Max.

The map gizmo is drawn in the 3D Viewport while a UVW Map is the active modifier: a plane,
box, cylinder or sphere with the seam (or the right edge of a planar map) in green.

Tab over the modifier (or its Edit Gizmo button) edits the gizmo like a sub-object: a helper
point is selected and the normal tools move, rotate and scale it, with snapping. The helper is
not linked to the modifier (a linked object would stop snapping to the mesh, because the mesh
depends on it); its transform is copied into the modifier after every change instead. Tab, or
selecting something else, puts the helper away. Helpers of deleted modifiers are removed.
"""

import bpy
from bpy.app.handlers import persistent
from bpy.types import Operator
from bpy.props import StringProperty
from mathutils import Euler, Matrix, Vector

KEY_GIZMO_FOR = "_uvw_gizmo_for"
KEY_GIZMO_MODIFIER = "_uvw_gizmo_modifier"

# Gizmo axes (x, y, z) from the aligned axes (u, v, w): the inverse of the modifier's alignment.
_ALIGN_INV = {
    'X': Matrix(((0, 0, 1, 0), (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 0, 1))),
    'Y': Matrix(((1, 0, 0, 0), (0, 0, -1, 0), (0, 1, 0, 0), (0, 0, 0, 1))),
    'Z': Matrix.Identity(4),
}


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
    helper = helper_of(ob, mod)
    if helper is not None:
        helper.matrix_world = ob.matrix_world @ Matrix.LocRotScale(location, rotation, size * 0.5)
    _store(mod, location, rotation, size)


def _store(mod, location, rotation, size):
    """Write the gizmo values, only if they changed (avoids endless depsgraph updates)."""
    import math
    values = list(location) + list(rotation) + list(size)
    if not all(math.isfinite(v) for v in values) or min(abs(v) for v in size) < 1e-6:
        return  # A flat or broken gizmo (e.g. scaled to zero while snapping): keep the last good one.
    euler = rotation.to_euler('XYZ', Euler(mod.gizmo_rotation, 'XYZ'))
    def differs(a, b):
        return any(abs(x - y) > 1e-6 for x, y in zip(a, b))
    if differs(mod.gizmo_location, location):
        mod.gizmo_location = location
    if differs(mod.gizmo_rotation, euler):
        mod.gizmo_rotation = euler
    if differs(mod.gizmo_size, size):
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
    region = context.region_data
    if region is None:
        for area in context.screen.areas if context.screen else ():
            if area.type == 'VIEW_3D':
                region = area.spaces.active.region_3d
                break
    if region is None:
        return False
    location, _rotation, size = current_gizmo(ob, mod)
    rotation = ob.matrix_world.to_quaternion().inverted() @ region.view_rotation
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


def owner_of(helper):
    """(object, modifier) a helper edits, or (None, None)."""
    ob = bpy.data.objects.get(helper.get(KEY_GIZMO_FOR, ""))
    if ob is None or ob.type != 'MESH':
        return None, None
    mod = ob.modifiers.get(helper.get(KEY_GIZMO_MODIFIER, ""))
    if mod is None or mod.type != 'UVW_MAP':
        return ob, None
    return ob, mod


def helper_of(ob, mod):
    for o in bpy.data.objects:
        if o.get(KEY_GIZMO_FOR) == ob.name and o.get(KEY_GIZMO_MODIFIER) == mod.name:
            return o
    return None


def _select_only(context, ob):
    for o in context.view_layer.objects:
        if o.select_get():
            o.select_set(False)
    ob.select_set(True)
    context.view_layer.objects.active = ob


def sync(helper):
    """Copy the helper's transform into its modifier."""
    ob, mod = owner_of(helper)
    if mod is None:
        return
    g = ob.matrix_world.inverted_safe() @ helper.matrix_world
    location, rotation, half = g.decompose()
    _store(mod, location, rotation, half * 2.0)


_last_matrix = {}


def _flat(matrix):
    return tuple(round(x, 6) for row in matrix for x in row)


def _sync_both_ways(helper):
    """Moving the helper moves the map; changing the gizmo in the panel (e.g. Alignment, which
    resets the rotation) moves the helper."""
    ob, mod = owner_of(helper)
    if mod is None:
        return
    current = _flat(helper.matrix_world)
    if _last_matrix.get(helper.name) != current:
        sync(helper)
    else:
        expected = ob.matrix_world @ gizmo_matrix(ob, mod)
        if _flat(expected) != current:
            helper.matrix_world = expected
            current = _flat(helper.matrix_world)
    _last_matrix[helper.name] = current


def start_gizmo_edit(context, ob, mod):
    """Select the gizmo of `mod`: its Gizmo Object, or a helper point that edits its values."""
    ob.modifiers.active = mod
    if mod.gizmo_object is not None:
        _select_only(context, mod.gizmo_object)
        return mod.gizmo_object
    helper = helper_of(ob, mod)
    if helper is None:
        location, rotation, size = current_gizmo(ob, mod)
        helper = bpy.data.objects.new(ob.name + " UVW Gizmo", None)
        helper.empty_display_type = 'PLAIN_AXES'
        helper.empty_display_size = 0.0001  # The gizmo itself is drawn by this module.
        helper.hide_render = True
        for collection in ob.users_collection:
            collection.objects.link(helper)
        helper.matrix_world = ob.matrix_world @ Matrix.LocRotScale(location, rotation, size * 0.5)
        helper[KEY_GIZMO_FOR] = ob.name
        helper[KEY_GIZMO_MODIFIER] = mod.name
    _select_only(context, helper)
    return helper


def finish_gizmo_edit(context, helper):
    """Store the helper's transform in its modifier, remove the helper, select the object."""
    ob, mod = owner_of(helper)
    if mod is not None:
        sync(helper)
    bpy.data.objects.remove(helper)
    if ob is not None and context is not None:
        _select_only(context, ob)
        if mod is not None:
            ob.modifiers.active = mod
    return ob


def cleanup(context=None):
    """Put away helpers that are no longer being edited: their modifier is gone, or they were
    deselected. Returns the number of helpers removed."""
    removed = 0
    for helper in [o for o in bpy.data.objects if KEY_GIZMO_FOR in o]:
        ob, mod = owner_of(helper)
        active = False
        try:
            active = helper.select_get()
        except RuntimeError:
            pass  # Not in the view layer.
        if mod is None:
            bpy.data.objects.remove(helper)
            removed += 1
        elif not active:
            sync(helper)
            bpy.data.objects.remove(helper)
            removed += 1
    return removed


_cleanup_pending = [False]


def _cleanup_timer():
    _cleanup_pending[0] = False
    try:
        if cleanup():
            bpy.ops.ed.undo_push(message="UVW Map Gizmo")
    except Exception as ex:
        print("UVW Map gizmo cleanup:", ex)
    return None


@persistent
def _uvw_map_depsgraph_update(_scene, _depsgraph):
    try:
        view_layer = bpy.context.view_layer
        active = view_layer.objects.active
    except Exception:
        return
    if is_gizmo(active) and active.select_get():
        _sync_both_ways(active)
    # Helpers that need putting away are removed outside the depsgraph update.
    if not _cleanup_pending[0]:
        for o in view_layer.objects:
            if KEY_GIZMO_FOR in o and (not o.select_get() or owner_of(o)[1] is None):
                _cleanup_pending[0] = True
                bpy.app.timers.register(_cleanup_timer, first_interval=0.0)
                break


# -----------------------------------------------------------------------------
# Drawing the gizmo

def _circle(axis_a, axis_b, offset, n=48):
    import math
    pts = []
    for i in range(n):
        for k in (i, i + 1):
            t = 2.0 * math.pi * k / n
            p = [0.0, 0.0, 0.0]
            p[axis_a] = math.cos(t)
            p[axis_b] = math.sin(t)
            p[3 - axis_a - axis_b] = offset
            pts.append(Vector(p))
    return pts


def gizmo_lines(map_type):
    """(lines, highlight lines) in aligned gizmo space (u, v, w in [-1, 1])."""
    import math
    lines, accent = [], []
    if map_type in {'PLANAR', 'FACE'}:
        c = [Vector((-1, -1, 0)), Vector((1, -1, 0)), Vector((1, 1, 0)), Vector((-1, 1, 0))]
        lines += [c[0], c[1], c[2], c[3], c[3], c[0]]
        accent += [c[1], c[2]]  # Right edge (+U), as in 3ds Max.
        lines += [Vector((0, 1, 0)), Vector((0, 1.3, 0))]  # Up tick (+V).
    elif map_type == 'BOX':
        for a in (-1, 1):
            for b in (-1, 1):
                lines += [Vector((-1, a, b)), Vector((1, a, b))]
                lines += [Vector((a, -1, b)), Vector((a, 1, b))]
                lines += [Vector((a, b, -1)), Vector((a, b, 1))]
    elif map_type == 'CYLINDRICAL':
        lines += _circle(0, 1, -1) + _circle(0, 1, 1)
        for x, y in ((1, 0), (0, 1), (0, -1)):
            lines += [Vector((x, y, -1)), Vector((x, y, 1))]
        accent += [Vector((-1, 0, -1)), Vector((-1, 0, 1))]  # Seam.
    else:  # Spherical, shrink wrap.
        lines += _circle(0, 1, 0) + _circle(0, 2, 0) + _circle(1, 2, 0)
        n = 24
        for i in range(n):
            for k in (i, i + 1):
                t = -math.pi / 2 + math.pi * k / n
                accent.append(Vector((-math.cos(t), 0.0, math.sin(t))))  # Seam.
    return lines, accent


def _draw_lines(shader, coords, color, width):
    from gpu_extras.batch import batch_for_shader
    import gpu
    if not coords:
        return
    batch = batch_for_shader(shader, 'LINES', {"pos": coords})
    shader.uniform_float("color", color)
    shader.uniform_float("lineWidth", width)
    batch.draw(shader)


def _draw_gizmo():
    import gpu
    context = bpy.context
    ob = context.active_object
    if ob is None:
        return
    editing = False
    if is_gizmo(ob):
        helper = ob
        ob, mod = owner_of(helper)
        if mod is None:
            return
        matrix = helper.matrix_world
        editing = True
    else:
        if ob.type != 'MESH' or ob.mode != 'OBJECT':
            return
        mod = ob.modifiers.active
        if mod is None or mod.type != 'UVW_MAP' or not mod.show_viewport:
            return
        matrix = ob.matrix_world @ gizmo_matrix(ob, mod)
    matrix = matrix @ _ALIGN_INV[mod.axis]
    lines, accent = gizmo_lines(mod.map_type)
    shader = gpu.shader.from_builtin('POLYLINE_UNIFORM_COLOR')
    _x, _y, w, h = gpu.state.viewport_get()
    shader.bind()
    shader.uniform_float("viewportSize", (w, h))
    gpu.state.blend_set('ALPHA')
    gpu.state.depth_test_set('NONE')
    alpha = 1.0 if editing else 0.6
    _draw_lines(shader, [matrix @ p for p in lines], (1.0, 0.8, 0.1, alpha), 2.0 if editing else 1.5)
    _draw_lines(shader, [matrix @ p for p in accent], (0.2, 1.0, 0.25, alpha), 2.5 if editing else 2.0)
    gpu.state.blend_set('NONE')


# -----------------------------------------------------------------------------
# Operators

class _UVWMapOperator:
    bl_options = {'REGISTER', 'UNDO'}

    modifier: StringProperty(name="Modifier")

    def get(self, context):
        ob = context.active_object
        if is_gizmo(ob):
            ob = owner_of(ob)[0]
        if ob is None or ob.type != 'MESH':
            return None, None
        mod = ob.modifiers.get(self.modifier) if self.modifier else ob.modifiers.active
        if mod is None or mod.type != 'UVW_MAP':
            return None, None
        return ob, mod


class OBJECT_OT_uvw_map_gizmo_edit(_UVWMapOperator, Operator):
    """Edit the map gizmo with Move / Rotate / Scale and snapping (Tab returns to the object)"""
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


def _ensure_handlers():
    handlers = bpy.app.handlers.depsgraph_update_post
    for fn in list(handlers):
        if getattr(fn, "__name__", "") == "_uvw_map_depsgraph_update":
            handlers.remove(fn)
    handlers.append(_uvw_map_depsgraph_update)

    store = bpy.app.driver_namespace
    old = store.get("_uvw_map_draw_handle")
    if old is not None:
        try:
            bpy.types.SpaceView3D.draw_handler_remove(old, 'WINDOW')
        except (ValueError, RuntimeError):
            pass
    store["_uvw_map_draw_handle"] = bpy.types.SpaceView3D.draw_handler_add(
        _draw_gizmo, (), 'WINDOW', 'POST_VIEW')


_ensure_handlers()
