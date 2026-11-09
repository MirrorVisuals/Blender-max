# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Edit Poly stack, phase 3: Tab edits an Edit Poly modifier level instead of the base mesh.

When the active modifier is a UVW Map, Tab edits its gizmo instead (see uvw_map.py); when it is
an Unwrap, Tab edits its UVs (see unwrap.py).
Leaving a level stores its face selection, which is passed up the stack (e.g. to a UVW Map
with Selected Faces Only).

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

import bmesh
import bpy
from bpy.app.handlers import persistent
from bpy.types import Operator
from bpy.props import StringProperty

KEY_MESH = "_edit_poly_mesh"
KEY_INPUT = "_edit_poly_input"
KEY_MODIFIER = "_edit_poly_modifier"
KEY_HIDDEN = "_edit_poly_hidden"

SELECTION_GROUP = "Edit Poly Selection"

# Tab on an object without an Edit Poly adds one, so the base mesh is never edited directly.
AUTO_ADD = True

# Session ID attributes (see MOD_edit_poly.hh).
VERT_LO, VERT_HI = "edit_poly_vertex_id_lo", "edit_poly_vertex_id_hi"
FACE_LO, FACE_HI = "edit_poly_face_id_lo", "edit_poly_face_id_hi"

# Above this size the selection is not tracked, so editing stays responsive; operations are then
# not logged and the level falls back to pausing when the mesh below changes.
CAPTURE_MAX_FACES = 200000


# -----------------------------------------------------------------------------
# Operation capture (phase 4b)
#
# While a level is edited, extrude / inset / bevel are logged with their settings and the IDs of
# the faces or edges they used. The C++ side keeps the log only if replaying it explains the edit.

_capture = {}


def _modal_running():
    wm = bpy.context.window_manager
    return any(len(win.modal_operators) for win in wm.windows)


def _snapshot(ob, cap):
    """Remember the current selection as session IDs (the selection an operation will use)."""
    bm = bmesh.from_edit_mesh(ob.data)
    if len(bm.faces) > CAPTURE_MAX_FACES:
        cap["faces"], cap["edges"], cap["enabled"] = [], [], False
        return
    fl, fh = bm.faces.layers.int.get(FACE_LO), bm.faces.layers.int.get(FACE_HI)
    vl, vh = bm.verts.layers.int.get(VERT_LO), bm.verts.layers.int.get(VERT_HI)
    if None in (fl, fh, vl, vh):
        cap["faces"], cap["edges"] = [], []
        return
    cap["faces"] = [(f[fl], f[fh]) for f in bm.faces if f.select]
    cap["edges"] = [((e.verts[0][vl], e.verts[0][vh]), (e.verts[1][vl], e.verts[1][vh]))
                    for e in bm.edges if e.select]


def _extrude_offset(ob):
    """Offset of an extrusion just made: copies of a vertex (same session ID) that are selected,
    minus the copy left behind. None if the offset is not the same for all of them."""
    bm = bmesh.from_edit_mesh(ob.data)
    vl, vh = bm.verts.layers.int.get(VERT_LO), bm.verts.layers.int.get(VERT_HI)
    if vl is None or vh is None:
        return None
    base = {}
    for v in bm.verts:
        if not v.select:
            base[(v[vl], v[vh])] = v.co.copy()
    offsets = []
    for v in bm.verts:
        if v.select:
            b = base.get((v[vl], v[vh]))
            if b is not None:
                offsets.append(v.co - b)
    if not offsets:
        return None
    mean = sum(offsets, offsets[0] * 0.0) / len(offsets)
    if any((o - mean).length > 1e-5 for o in offsets):
        return None
    return mean


_EXTRUDE_OPS = {
    "MESH_OT_extrude_region_move",
    "MESH_OT_extrude_context_move",
    "VIEW3D_OT_edit_mesh_extrude_move_normal",
}

_OFFSET_TYPES = {'OFFSET': 0, 'WIDTH': 1, 'DEPTH': 2, 'PERCENT': 3, 'ABSOLUTE': 4}
_MITERS = {'SHARP': 0, 'PATCH': 1, 'ARC': 2}
_VMESH = {'ADJ': 0, 'CUTOFF': 1}
_FACE_STRENGTH = {'NONE': 0, 'NEW': 1, 'AFFECTED': 2, 'ALL': 3}


def _op_props(op):
    return op.properties


def _entry_from_operator(op, ob, cap):
    """A log entry {kind, params, faces | edges}, or None for operations that are not logged."""
    idname = op.bl_idname
    if idname in _EXTRUDE_OPS:
        if not cap["faces"]:
            return None
        return {"kind": "EXTRUDE", "faces": list(cap["faces"]), "params": None, "op": op}
    if idname == "MESH_OT_inset":
        p = _op_props(op)
        if not cap["faces"]:
            return None
        return {"kind": "INSET", "faces": list(cap["faces"]), "op": op, "params": None}
    if idname == "MESH_OT_bevel":
        p = _op_props(op)
        if p.affect != 'EDGES' or p.profile_type != 'SUPERELLIPSE' or p.harden_normals:
            return None
        if not cap["edges"]:
            return None
        return {"kind": "BEVEL", "edges": list(cap["edges"]), "op": op, "params": None}
    return None


def _refresh_params(entry, ob):
    """Read (or re-read after Adjust Last Operation) the settings of a logged operation."""
    p = _op_props(entry["op"])
    if entry["kind"] == "EXTRUDE":
        offset = _extrude_offset(ob)
        entry["params"] = None if offset is None else [offset.x, offset.y, offset.z]
    elif entry["kind"] == "INSET":
        entry["params"] = [float(p.use_individual), p.thickness, p.depth, float(p.use_even_offset),
                           float(p.use_relative_offset), float(p.use_interpolate),
                           float(p.use_boundary), float(p.use_outset), float(p.use_edge_rail)]
    else:
        offset = p.offset_pct if p.offset_type == 'PERCENT' else p.offset
        entry["params"] = [offset, _OFFSET_TYPES[p.offset_type], p.segments, 0, p.profile,
                           float(p.clamp_overlap), float(p.loop_slide), _MITERS[p.miter_outer],
                           _MITERS[p.miter_inner], p.spread, _VMESH[p.vmesh_method],
                           float(p.mark_seam), float(p.mark_sharp), p.material,
                           _FACE_STRENGTH[p.face_strength_mode]]


def _signature(op):
    try:
        return tuple(str(getattr(op.properties, k)) for k in op.properties.bl_rna.properties.keys()
                     if k != "rna_type")
    except Exception:
        return None


@persistent
def _edit_poly_depsgraph_update(_scene, _depsgraph):
    try:
        ob = bpy.context.view_layer.objects.active
    except Exception:
        return
    if ob is None or ob.mode != 'EDIT' or KEY_MESH not in ob:
        return
    cap = _capture.get(ob.name)
    if cap is None or not cap["enabled"]:
        return
    for op in bpy.context.window_manager.operators:
        ptr = op.as_pointer()
        entry = cap["by_ptr"].get(ptr)
        if entry is not None:
            sig = _signature(op)
            if sig != entry["sig"]:  # Adjusted with Adjust Last Operation.
                entry["sig"] = sig
                _refresh_params(entry, ob)
            continue
        if ptr in cap["seen"]:
            continue
        cap["seen"].add(ptr)
        entry = _entry_from_operator(op, ob, cap)
        if entry is not None:
            entry["sig"] = _signature(op)
            _refresh_params(entry, ob)
            cap["entries"].append(entry)
            cap["by_ptr"][ptr] = entry
    if not _modal_running():
        _snapshot(ob, cap)


def _start_capture(ob):
    cap = {"seen": {op.as_pointer() for op in bpy.context.window_manager.operators},
           "by_ptr": {}, "entries": [], "faces": [], "edges": [], "enabled": True}
    _capture[ob.name] = cap
    _snapshot(ob, cap)


def _capture_text(ob):
    cap = _capture.pop(ob.name, None)
    if not cap or not cap["enabled"]:
        return ""
    lines = []
    for e in cap["entries"]:
        if e["params"] is None:
            return ""  # An operation that cannot be replayed: log nothing.
        params = ",".join(repr(float(x)) for x in e["params"])
        if e["kind"] == "BEVEL":
            elems = ",".join("{}:{}:{}:{}".format(a[0], a[1], b[0], b[1]) for a, b in e["edges"])
            lines.append("BEVEL params={} edges={}".format(params, elems))
        else:
            elems = ",".join("{}:{}".format(lo, hi) for lo, hi in e["faces"])
            lines.append("{} params={} faces={}".format(e["kind"], params, elems))
    return "".join(line + "\n" for line in lines)


def _ensure_handler():
    handlers = bpy.app.handlers.depsgraph_update_post
    for fn in list(handlers):
        if getattr(fn, "__name__", "") == "_edit_poly_depsgraph_update":
            handlers.remove(fn)
    handlers.append(_edit_poly_depsgraph_update)


_ensure_handler()


def classic_toggle():
    """The normal Tab behaviour. Objects without an Edit Mode (empties, cameras...) are ignored,
    as with the default keymap."""
    try:
        return bpy.ops.object.mode_set(mode='EDIT', toggle=True)
    except (TypeError, RuntimeError):
        return {'PASS_THROUGH'}


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
    _start_capture(ob)
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
    _capture.pop(ob.name, None)
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
        level.session_commit(input=ob[KEY_INPUT], mesh=ob.data, operations=_capture_text(ob))
    except RuntimeError as ex:
        return str(ex).strip()
    end_session(ob)
    # The level's vertex selection is written into this vertex group, so modifiers above that
    # take a vertex group (Solidify, Smooth, Displace...) can work on the selection.
    if SELECTION_GROUP not in ob.vertex_groups:
        ob.vertex_groups.new(name=SELECTION_GROUP)
    return None


class OBJECT_OT_edit_poly_tab(Operator):
    """Edit the active Edit Poly level (Tab). Moves are stored in the modifier, """ \
        """the base mesh is never edited"""
    bl_idname = "object.edit_poly_tab"
    bl_label = "Toggle Edit Poly Level"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        from bl_operators import unwrap, uvw_map
        ob = context.active_object
        in_viewport = context.area is None or context.area.type == 'VIEW_3D'

        # Tab on a UVW Map gizmo returns to the object (the gizmo is like a sub-object).
        if uvw_map.is_gizmo(ob):
            uvw_map.finish_gizmo_edit(context, ob)
            return {'FINISHED'}

        # Tab in an Unwrap session stores the UVs.
        if unwrap.in_session(ob):
            warning = unwrap.commit_session(context, ob)
            if warning:
                self.report({'WARNING'}, warning)
            return {'FINISHED'}

        # Tab over a modifier panel in the Properties editor acts on that modifier.
        hovered = getattr(context, "modifier", None)
        if ob is not None and hovered is not None and hovered.id_data == ob:
            ob.modifiers.active = hovered

        # Tab on a UVW Map edits its gizmo.
        active = ob.modifiers.active if ob is not None and hasattr(ob, "modifiers") else None
        if (active is not None and active.type == 'UVW_MAP' and not in_session(ob) and
                context.mode == 'OBJECT'):
            uvw_map.start_gizmo_edit(context, ob, active)
            return {'FINISHED'}

        # Tab on an Unwrap edits its UVs.
        if (active is not None and active.type == 'UNWRAP' and not in_session(ob) and
                context.mode == 'OBJECT' and ob.type == 'MESH'):
            unwrap.start_session(context, ob, active)
            return {'FINISHED'}

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
            return classic_toggle()

        level = pick_level(ob)
        if level is None:
            if not AUTO_ADD:
                return classic_toggle()
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


# Attributes that describe the history of a mesh rather than the mesh itself. A collapsed mesh
# has no history: its elements keep their stable IDs, but not their parents or footprints.
_HISTORY_ATTRIBUTES = (
    ".stable_parent_point", ".stable_parent_edge", ".stable_parent_face", ".stable_subd_uv",
    VERT_LO, VERT_HI, FACE_LO, FACE_HI,
)


class OBJECT_OT_edit_poly_collapse_to(Operator):
    """Bake this modifier and all modifiers below it into the mesh. """ \
        """Stable IDs are kept, so Edit Poly levels above keep finding their elements"""
    bl_idname = "object.edit_poly_collapse_to"
    bl_label = "Collapse to Here"
    bl_options = {'REGISTER', 'UNDO'}

    modifier: StringProperty(name="Modifier")

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        if ob is None or ob.type != 'MESH' or context.mode != 'OBJECT':
            return False
        if in_session(ob) or "_unwrap_mesh" in ob:
            cls.poll_message_set("Leave the Edit Poly or Unwrap level first (Tab)")
            return False
        return True

    def execute(self, context):
        ob = context.active_object
        mods = ob.modifiers[:]
        level = ob.modifiers.get(self.modifier)
        if level is None:
            self.report({'ERROR'}, "Modifier not found")
            return {'CANCELLED'}
        k = mods.index(level)
        disabled = [m.name for m in mods[:k + 1] if not m.show_viewport]

        # Evaluate up to and including the modifier, as shown in the viewport.
        visible = {m.name: m.show_viewport for m in mods}
        for m in mods[k + 1:]:
            m.show_viewport = False
        collapsed = evaluated_copy(context, ob)
        for m in mods[k + 1:]:
            m.show_viewport = visible[m.name]

        for name in _HISTORY_ATTRIBUTES:
            attr = collapsed.attributes.get(name)
            if attr is not None:
                collapsed.attributes.remove(attr)

        original = ob.data
        had_shape_keys = original.shape_keys is not None
        ob.data = collapsed
        for m in mods[:k + 1]:
            ob.modifiers.remove(m)
        name = original.name
        if original.users == 0:
            bpy.data.meshes.remove(original)
        collapsed.name = name

        if disabled:
            self.report({'WARNING'}, "Disabled modifiers were removed without effect: " +
                        ", ".join(disabled))
        if had_shape_keys:
            self.report({'WARNING'}, "Shape keys of the original mesh are not part of the "
                        "collapsed mesh")
        return {'FINISHED'}


classes = (
    OBJECT_OT_edit_poly_tab,
    OBJECT_OT_edit_poly_session_discard,
    OBJECT_OT_edit_poly_level_edit,
    OBJECT_OT_edit_poly_collapse_to,
)
