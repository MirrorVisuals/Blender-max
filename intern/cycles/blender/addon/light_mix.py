# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

"""
Light Mix: re-balance the light groups of a view layer without rendering again.

The mix itself is native: Cycles multiplies each light group pass by its weight
(Solo, On/Off, Value, Exposure and Tint of the group, times the Master of the view layer)
and adds the difference to the Combined pass while it displays the image. This module adds the
workflow around it:

* Scan Lights builds the light groups from the scene and assigns them. Linked duplicates and
  collection instances share one group, real copies get their own; emissive objects and the
  World can get groups too.
* Bake Mix into Scene writes the mix back onto the lights, the World and emissive materials.
* Select, Reset, Clear All, Quick Render, and a migration from the Light Mix add-on.
"""

from __future__ import annotations

import re

import bpy
from bpy.app.translations import pgettext_tip as tip_
from bpy.props import (
    BoolProperty,
    EnumProperty,
    IntProperty,
    PointerProperty,
    StringProperty,
)
from bpy.types import Operator, Panel, PropertyGroup, UIList

from .ui import CyclesButtonsPanel


# ---------------------------------------------------------------------------
# Settings
# ---------------------------------------------------------------------------

class CyclesLightMixSettings(PropertyGroup):
    """Scan and workflow options. The mix values live on the view layer and its light groups."""

    naming_mode: EnumProperty(
        name="Grouping",
        items=(
            ('AUTO', "Automatic", "Linked duplicates share one group, copies get their own"),
            ('PREFIX', "Name Prefix", "Group by the part of the object name before the separator"),
            ('REGEX', "Regex", "Group by a regular expression capture of the object name"),
        ),
        default='AUTO',
    )
    separator: StringProperty(
        name="Separator",
        default="_",
        description="Group objects by the part of their name before this text",
    )
    custom_regex: StringProperty(
        name="Regex",
        default=r"^([A-Za-z]+)",
        description="The first capture group (or the whole match) of the object name is the group name",
    )
    prefix: StringProperty(
        name="Group Prefix",
        default="",
        description="Text put in front of every light group name",
    )
    strip_suffix: BoolProperty(
        name="Strip .001 Suffix",
        default=True,
        description="Remove Blender's numeric suffix from the group name",
    )
    include_world: BoolProperty(
        name="World",
        default=True,
        description="Put the World in its own light group",
    )
    include_instanced_collections: BoolProperty(
        name="Collection Instances",
        default=True,
        description="Also group lights that only exist inside instanced collections",
    )
    include_emissive: BoolProperty(
        name="Emissive Objects",
        default=True,
        description="Also group mesh objects with emissive materials",
    )
    emissive_mode: EnumProperty(
        name="Emissives",
        items=(
            ('SINGLE', "One Global Group", "All emissive objects share a single group"),
            ('MATERIAL', "Per Material", "One group per emissive material"),
            ('OBJECT', "Per Object", "One group per emissive object, following the grouping rules"),
        ),
        default='MATERIAL',
    )
    emissive_group_name: StringProperty(
        name="Global Emissive Name",
        default="Emissive",
        description="Group name used when all emissive objects share one group",
    )
    purge_unused: BoolProperty(
        name="Purge Unused Groups",
        default=True,
        description="Remove light groups that nothing is assigned to after each scan",
    )
    quick_samples: IntProperty(
        name="Quick Samples",
        default=32,
        min=1,
        max=16384,
        description="Sample count used by Quick Render",
    )
    quick_resolution: IntProperty(
        name="Quick Resolution",
        default=50,
        min=1,
        max=100,
        subtype='PERCENTAGE',
        description="Resolution percentage used by Quick Render",
    )
    show_header_button: BoolProperty(
        name="Image Editor Header Button",
        default=True,
        description="Show a Light Mix popover in the Image Editor header, which is also the header of the render "
        "window",
    )


# ---------------------------------------------------------------------------
# Grouping
# ---------------------------------------------------------------------------

_SUFFIX_RE = re.compile(r"\.\d{3}$")


def sanitize(name):
    """Light group names as Blender stores them: it replaces dots, and pass names need plain text."""
    name = re.sub(r"[^A-Za-z0-9_\-]", "_", name.strip())
    name = re.sub(r"_{2,}", "_", name).strip("_")
    return name or "Lightgroup"


def emission_sockets(ntree):
    """Yield ``(node, color_socket, strength_socket)`` for the emissive shaders of a node tree."""
    if ntree is None:
        return
    for node in ntree.nodes:
        if node.bl_idname in {"ShaderNodeEmission", "ShaderNodeBackground"}:
            yield node, node.inputs.get("Color"), node.inputs.get("Strength")
        elif node.bl_idname == "ShaderNodeBsdfPrincipled":
            color = node.inputs.get("Emission Color") or node.inputs.get("Emission")
            strength = node.inputs.get("Emission Strength")
            if strength is not None:
                yield node, color, strength


def _material_is_emissive(material):
    if material is None or material.node_tree is None:
        return False
    for node, _color, strength in emission_sockets(material.node_tree):
        if node.bl_idname == "ShaderNodeEmission":
            return True
        if strength is not None and (strength.is_linked or strength.default_value > 0.0):
            return True
    return False


def emissive_materials(obj):
    materials = []
    for slot in getattr(obj, "material_slots", ()):
        material = slot.material
        if material is not None and material not in materials and _material_is_emissive(material):
            materials.append(material)
    return materials


def is_instanced(obj):
    """True when the object's data is shared (linked duplicates, Alt-D)."""
    data = getattr(obj, "data", None)
    return bool(data is not None and getattr(data, "users", 1) > 1)


def collect_emitters(scene, settings):
    """``{object name: object}`` for everything that gets a light group."""
    found = {}

    def consider(obj):
        if obj.name in found:
            return
        if obj.type == 'LIGHT':
            found[obj.name] = obj
        elif settings.include_emissive and emissive_materials(obj):
            found[obj.name] = obj

    for obj in scene.objects:
        consider(obj)
        # Lights inside an instanced collection are not in the scene objects, but Cycles reads the
        # light group of the source object, so all instances share its group.
        if settings.include_instanced_collections and obj.instance_type == 'COLLECTION' and obj.instance_collection:
            for source in obj.instance_collection.all_objects:
                consider(source)
    return found


def group_key(obj, settings):
    """The light group name an object belongs to."""
    if obj.type != 'LIGHT':
        if settings.emissive_mode == 'SINGLE':
            return sanitize(settings.prefix + (settings.emissive_group_name or "Emissive"))
        if settings.emissive_mode == 'MATERIAL':
            materials = emissive_materials(obj)
            if materials:
                return sanitize(settings.prefix + materials[0].name)

    if settings.naming_mode == 'AUTO':
        # Shared data: one group for the whole set of linked duplicates. Unique data: keep the .001,
        # so real copies stay separate.
        base = obj.data.name if (obj.data is not None and is_instanced(obj)) else obj.name
    elif settings.naming_mode == 'PREFIX':
        base = obj.name.split(settings.separator or "_")[0]
    else:
        try:
            match = re.search(settings.custom_regex, obj.name)
        except re.error:
            match = None
        if match is None:
            base = obj.name
        elif match.groups():
            base = match.group(1) or obj.name
        else:
            base = match.group(0) or obj.name

    if settings.naming_mode != 'AUTO' and settings.strip_suffix:
        base = _SUFFIX_RE.sub("", base)
    return sanitize(settings.prefix + base)


# ---------------------------------------------------------------------------
# Light group members, for the list icons, Select and Bake
# ---------------------------------------------------------------------------

def group_members(scene, name):
    """``(objects, worlds)`` assigned to the light group ``name``."""
    objects = [obj for obj in bpy.data.objects if obj.lightgroup == name]
    worlds = [world for world in (scene.world,) if world is not None and world.lightgroup == name]
    return objects, worlds


_member_info_cache = None


def _member_info(scene):
    """``{group name: (icon, instanced)}``, cached until the next scene change."""
    global _member_info_cache
    if _member_info_cache is not None and _member_info_cache[0] == scene.as_pointer():
        return _member_info_cache[1]
    kinds = {}
    instanced = set()
    for obj in bpy.data.objects:
        name = obj.lightgroup
        if not name:
            continue
        kinds.setdefault(name, set()).add('LIGHT' if obj.type == 'LIGHT' else 'EMISSIVE')
        if is_instanced(obj):
            instanced.add(name)
    if scene.world is not None and scene.world.lightgroup:
        kinds.setdefault(scene.world.lightgroup, set()).add('WORLD')
    info = {}
    for name, kind in kinds.items():
        if 'WORLD' in kind:
            icon = 'WORLD'
        elif kind == {'EMISSIVE'}:
            icon = 'SHADING_RENDERED'
        else:
            icon = 'OUTLINER_OB_LIGHT'
        info[name] = (icon, name in instanced)
    _member_info_cache = (scene.as_pointer(), info)
    return info


@bpy.app.handlers.persistent
def _member_info_invalidate(*_args):
    global _member_info_cache
    _member_info_cache = None


# ---------------------------------------------------------------------------
# Mix helpers
# ---------------------------------------------------------------------------

def any_solo(view_layer):
    return any(lightgroup.use_mix_solo for lightgroup in view_layer.lightgroups)


def gain_of(view_layer, lightgroup):
    """Everything the mix does to the brightness of a light group, as one factor (no tint)."""
    if not lightgroup.use_mix:
        return 0.0
    if any_solo(view_layer) and not lightgroup.use_mix_solo:
        return 0.0
    return (
        lightgroup.mix_value * (2.0 ** lightgroup.mix_exposure) *
        view_layer.light_mix_value * (2.0 ** view_layer.light_mix_exposure)
    )


def tint_of(view_layer, lightgroup):
    return tuple(a * b for a, b in zip(lightgroup.mix_color, view_layer.light_mix_color))


def reset_mix(view_layer):
    for lightgroup in view_layer.lightgroups:
        lightgroup.use_mix = True
        lightgroup.use_mix_solo = False
        lightgroup.mix_value = 1.0
        lightgroup.mix_exposure = 0.0
        lightgroup.mix_color = (1.0, 1.0, 1.0)
    view_layer.light_mix_value = 1.0
    view_layer.light_mix_exposure = 0.0
    view_layer.light_mix_color = (1.0, 1.0, 1.0)


def clear_light_groups(scene):
    """Remove every light group from the view layers and from the objects and worlds."""
    cleared = 0
    # All objects, so lights inside instanced collections are included.
    for obj in bpy.data.objects:
        if obj.lightgroup:
            obj.lightgroup = ""
            cleared += 1
    for world in bpy.data.worlds:
        if world.lightgroup:
            world.lightgroup = ""
            cleared += 1
    removed = 0
    for view_layer in scene.view_layers:
        while len(view_layer.lightgroups):
            view_layer.lightgroups.remove(view_layer.lightgroups[0])
            removed += 1
    return removed, cleared


def _update_passes(view_layer):
    try:
        view_layer.update_render_passes()
    except Exception:
        pass


# ---------------------------------------------------------------------------
# The Light Mix add-on: its settings and compositor nodes, for the migration
# ---------------------------------------------------------------------------

_ADDON_OWNER = "lightmix_owner"
_ADDON_ADJUST_TAG = "lightmix_adjust"
_ADDON_LEGACY_TINT = "lightmix_tint"


def _addon_settings(scene):
    """The mix of the Light Mix add-on as plain values, or None when the scene has none."""
    data = getattr(scene, "lightmix", None)
    if data is None:
        try:
            props = scene.bl_system_properties_get()
        except (AttributeError, TypeError):
            props = None
        if props is not None:
            data = props.get("lightmix")
        if data is None:
            data = scene.get("lightmix")
    if data is None:
        return None

    def get(item, key, default):
        # The add-on's own properties while it is enabled, plain ID properties once it is not.
        if isinstance(item, bpy.types.bpy_struct):
            value = getattr(item, key, default)
        else:
            try:
                value = item.get(key, default)
            except AttributeError:
                value = default
        return default if value is None else value

    groups = []
    for item in get(data, "groups", ()):
        groups.append({
            "name": str(get(item, "name", "")),
            "value": float(get(item, "value", 1.0)),
            "exposure": float(get(item, "exposure", 0.0)),
            "tint": tuple(get(item, "tint", (1.0, 1.0, 1.0)))[:3],
            "enabled": bool(get(item, "enabled", True)),
            "solo": bool(get(item, "solo", False)),
        })
    return {
        "groups": groups,
        "master_value": float(get(data, "master_value", 1.0)),
        "master_exposure": float(get(data, "master_exposure", 0.0)),
        "master_tint": tuple(get(data, "master_tint", (1.0, 1.0, 1.0)))[:3],
    }


def _comp_tree(scene):
    tree = getattr(scene, "compositing_node_group", None)
    if tree is None and getattr(scene, "use_nodes", False):
        tree = getattr(scene, "node_tree", None)
    return tree


def remove_addon_nodes(scene):
    """Remove the compositor nodes of the Light Mix add-on and connect what they fed to the plain render."""
    tree = _comp_tree(scene)
    if tree is None:
        return 0
    owned = [node for node in tree.nodes if node.get(_ADDON_OWNER)]
    if not owned:
        return 0
    render_layers = next(
        (node for node in tree.nodes if node.bl_idname == "CompositorNodeRLayers" and not node.get(_ADDON_OWNER)),
        None,
    )
    owned_names = {node.name for node in owned}
    targets = []
    for node in owned:
        for socket in node.outputs:
            for link in socket.links:
                if link.to_node.name not in owned_names:
                    index = list(link.to_node.inputs).index(link.to_socket)
                    targets.append((link.to_node.name, index))
    for node in owned:
        tree.nodes.remove(node)
    source = render_layers.outputs.get("Image") if render_layers is not None else None
    if source is not None:
        for name, index in targets:
            node = tree.nodes.get(name)
            if node is not None and index < len(node.inputs):
                tree.links.new(source, node.inputs[index])
    for group in list(bpy.data.node_groups):
        if group.get(_ADDON_ADJUST_TAG) and group.users == 0:
            bpy.data.node_groups.remove(group)
    return len(owned)


def restore_addon_legacy():
    """Undo what the oldest Light Mix add-on (which changed the lights live) left in the scene."""
    restored = 0
    for light in bpy.data.lights:
        if "lm_base_energy" in light.keys():
            light.energy = light["lm_base_energy"]
            del light["lm_base_energy"]
            restored += 1
        if "lm_base_color" in light.keys():
            try:
                light.color = light["lm_base_color"]
            except (TypeError, ValueError):
                pass
            del light["lm_base_color"]

    for holder in list(bpy.data.worlds) + list(bpy.data.materials):
        ntree = holder.node_tree
        if ntree is not None:
            for node in [node for node in ntree.nodes if node.get(_ADDON_LEGACY_TINT)]:
                inputs = [socket for socket in node.inputs if socket.enabled] or list(node.inputs)
                outputs = [socket for socket in node.outputs if socket.enabled] or list(node.outputs)
                source = inputs[1].links[0].from_socket if len(inputs) > 1 and inputs[1].is_linked else None
                for link in list(outputs[0].links):
                    target = link.to_socket
                    ntree.links.remove(link)
                    if source is not None:
                        ntree.links.new(source, target)
                ntree.nodes.remove(node)
                restored += 1
        for key in [key for key in holder.keys() if key.startswith("lm_base_")]:
            value = holder[key]
            if ntree is not None and key.startswith("lm_base_strength_"):
                node = ntree.nodes.get(key[len("lm_base_strength_"):])
                socket = node and (node.inputs.get("Strength") or node.inputs.get("Emission Strength"))
                if socket is not None and not socket.is_linked:
                    socket.default_value = value
                    restored += 1
            elif ntree is not None and key.startswith("lm_base_color_"):
                node = ntree.nodes.get(key[len("lm_base_color_"):])
                socket = node and (node.inputs.get("Color") or node.inputs.get("Emission Color"))
                if socket is not None and not socket.is_linked:
                    try:
                        socket.default_value = value
                        restored += 1
                    except (TypeError, ValueError):
                        pass
            del holder[key]
    return restored


# ---------------------------------------------------------------------------
# Operators
# ---------------------------------------------------------------------------

class CyclesLightMixOperator:
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return context.scene is not None and context.view_layer is not None


class CYCLES_OT_light_mix_scan(CyclesLightMixOperator, Operator):
    """Build light groups from the lights (and emissive objects and World) of the scene, assign them, """ \
        """and turn Light Mix on"""
    bl_idname = "cycles.light_mix_scan"
    bl_label = "Scan Lights"

    def execute(self, context):
        scene = context.scene
        view_layer = context.view_layer
        settings = scene.cycles_light_mix

        emitters = collect_emitters(scene, settings)
        buckets = {}
        for obj in emitters.values():
            buckets.setdefault(group_key(obj, settings), []).append(obj)

        world_key = None
        if settings.include_world and scene.world is not None:
            world_key = sanitize(settings.prefix + "World")
            buckets.setdefault(world_key, [])

        # Create the missing groups and use the name Blender stored. Existing groups keep their mix.
        existing = {lightgroup.name for lightgroup in view_layer.lightgroups}
        canonical = {}
        for key in buckets:
            if key in existing:
                canonical[key] = key
                continue
            lightgroup = view_layer.lightgroups.add(name=key)
            canonical[key] = lightgroup.name
            existing.add(lightgroup.name)

        assigned = 0
        for key, objects in buckets.items():
            name = canonical[key]
            for obj in objects:
                if obj.lightgroup != name:
                    obj.lightgroup = name
                assigned += 1
        if world_key is not None:
            scene.world.lightgroup = canonical[world_key]

        removed = 0
        if settings.purge_unused:
            used = {obj.lightgroup for obj in bpy.data.objects if obj.lightgroup}
            if scene.world is not None and scene.world.lightgroup:
                used.add(scene.world.lightgroup)
            for lightgroup in [lightgroup for lightgroup in view_layer.lightgroups if lightgroup.name not in used]:
                view_layer.lightgroups.remove(lightgroup)
                removed += 1

        view_layer.use_light_mix = True
        _update_passes(view_layer)
        _member_info_invalidate()

        message = tip_("{:d} light group(s) from {:d} emitter(s)").format(len(buckets), len(emitters))
        if removed:
            message += tip_(", removed {:d} unused").format(removed)
        self.report({'INFO'}, message)
        return {'FINISHED'}


class CYCLES_OT_light_mix_select(CyclesLightMixOperator, Operator):
    """Select the objects of this light group"""
    bl_idname = "cycles.light_mix_select"
    bl_label = "Select Objects"

    group: StringProperty(name="Light Group")

    def execute(self, context):
        objects, _worlds = group_members(context.scene, self.group)
        view_layer = context.view_layer
        for obj in context.selectable_objects:
            obj.select_set(False)
        count = 0
        for obj in objects:
            if view_layer.objects.get(obj.name) is None:
                continue
            try:
                obj.select_set(True)
            except RuntimeError:
                continue
            view_layer.objects.active = obj
            count += 1
        self.report({'INFO'}, tip_("Selected {:d} object(s)").format(count))
        return {'FINISHED'}


class CYCLES_OT_light_mix_reset(CyclesLightMixOperator, Operator):
    """Return every light group and the master to neutral"""
    bl_idname = "cycles.light_mix_reset"
    bl_label = "Reset Mix"

    def execute(self, context):
        reset_mix(context.view_layer)
        return {'FINISHED'}


class CYCLES_OT_light_mix_bake(CyclesLightMixOperator, Operator):
    """Write the current mix into the lights, World and emissive materials, then return the mix to """ \
        """neutral. Render again to see the baked result"""
    bl_idname = "cycles.light_mix_bake"
    bl_label = "Bake Mix into Scene"

    after: EnumProperty(
        name="Afterwards",
        items=(
            ('CLEAR', "Clear Light Groups", "Remove the light groups and their assignments"),
            ('NEUTRAL', "Keep and Reset", "Keep the light groups, with every setting back to neutral"),
        ),
        default='CLEAR',
    )

    def invoke(self, context, _event):
        return context.window_manager.invoke_props_dialog(self, width=420)

    def draw(self, _context):
        layout = self.layout
        layout.prop(self, "after", expand=True)
        box = layout.box()
        box.label(text="The scene data changes, the render does not", icon='INFO')
        box.label(text="Render again (F12) to see the baked result")

    def execute(self, context):
        scene = context.scene
        view_layer = context.view_layer
        touched = 0
        skipped = 0
        seen = set()

        def recolor(rgb, tint):
            return tuple(c * t for c, t in zip(rgb[:3], tint))

        def bake_tree(ntree, gain, tint):
            nonlocal touched, skipped
            for _node, color, strength in emission_sockets(ntree):
                if strength is not None:
                    if strength.is_linked:
                        skipped += 1
                    else:
                        strength.default_value *= gain
                        touched += 1
                if color is not None:
                    if color.is_linked:
                        skipped += 1
                    else:
                        value = color.default_value
                        color.default_value = recolor(value, tint) + tuple(value[3:])

        for lightgroup in view_layer.lightgroups:
            gain = gain_of(view_layer, lightgroup)
            tint = tint_of(view_layer, lightgroup)
            if gain == 1.0 and tint == (1.0, 1.0, 1.0):
                continue
            objects, worlds = group_members(scene, lightgroup.name)
            for obj in objects:
                if obj.type == 'LIGHT':
                    light = obj.data
                    if light is None or ('LIGHT', light.name) in seen:
                        continue
                    seen.add(('LIGHT', light.name))
                    light.energy *= gain
                    light.color = recolor(light.color, tint)
                    touched += 1
                else:
                    for material in emissive_materials(obj):
                        if ('MATERIAL', material.name) in seen:
                            continue
                        seen.add(('MATERIAL', material.name))
                        bake_tree(material.node_tree, gain, tint)
            for world in worlds:
                if ('WORLD', world.name) in seen:
                    continue
                seen.add(('WORLD', world.name))
                bake_tree(world.node_tree, gain, tint)

        reset_mix(view_layer)
        removed = 0
        if self.after == 'CLEAR':
            removed, _cleared = clear_light_groups(scene)
            _update_passes(view_layer)
        _member_info_invalidate()

        message = tip_("Baked into {:d} target(s), render again to see it").format(touched)
        if skipped:
            message += tip_(" ({:d} linked socket(s) skipped)").format(skipped)
        if self.after == 'CLEAR':
            message += tip_(". Cleared {:d} light group(s)").format(removed)
        self.report({'INFO'}, message)
        return {'FINISHED'}


class CYCLES_OT_light_mix_clear_all(CyclesLightMixOperator, Operator):
    """Remove every light group from the view layers and from the lights, World and emissive objects, """ \
        """and reset the mix"""
    bl_idname = "cycles.light_mix_clear_all"
    bl_label = "Clear All Light Groups"

    clean_addon: BoolProperty(
        name="Clean Up Light Mix Add-on Data",
        default=True,
        description="Also remove the compositor nodes of the Light Mix add-on (connecting what they fed to the "
        "plain render), and restore the lights, World and materials its oldest version changed",
    )

    def invoke(self, context, _event):
        return context.window_manager.invoke_props_dialog(self)

    def execute(self, context):
        scene = context.scene
        removed, cleared = clear_light_groups(scene)
        for view_layer in scene.view_layers:
            view_layer.light_mix_value = 1.0
            view_layer.light_mix_exposure = 0.0
            view_layer.light_mix_color = (1.0, 1.0, 1.0)
        if self.clean_addon:
            remove_addon_nodes(scene)
            restore_addon_legacy()
        _update_passes(context.view_layer)
        _member_info_invalidate()
        self.report({'INFO'}, tip_("Removed {:d} light group(s), cleared {:d} assignment(s)").format(removed, cleared))
        return {'FINISHED'}


class CYCLES_OT_light_mix_import_addon(CyclesLightMixOperator, Operator):
    """Take over the mix of the Light Mix add-on: its settings move to the native Light Mix, and its """ \
        """compositor nodes are removed (what they fed gets the plain render again)"""
    bl_idname = "cycles.light_mix_import_addon"
    bl_label = "Import Light Mix Add-on Mix"

    @classmethod
    def poll(cls, context):
        return super().poll(context) and _addon_settings(context.scene) is not None

    def execute(self, context):
        scene = context.scene
        view_layer = context.view_layer
        data = _addon_settings(scene)
        imported = 0
        for group in data["groups"]:
            lightgroup = view_layer.lightgroups.get(group["name"])
            if lightgroup is None:
                continue
            lightgroup.use_mix = group["enabled"]
            lightgroup.use_mix_solo = group["solo"]
            lightgroup.mix_value = group["value"]
            lightgroup.mix_exposure = group["exposure"]
            lightgroup.mix_color = group["tint"]
            imported += 1
        view_layer.light_mix_value = data["master_value"]
        view_layer.light_mix_exposure = data["master_exposure"]
        view_layer.light_mix_color = data["master_tint"]
        view_layer.use_light_mix = True
        nodes = remove_addon_nodes(scene)
        _update_passes(view_layer)
        self.report(
            {'INFO'},
            tip_("Imported {:d} light group(s), removed {:d} compositor node(s). "
                 "Disable the Light Mix add-on").format(imported, nodes),
        )
        return {'FINISHED'}


_quick_state = {}


@bpy.app.handlers.persistent
def _quick_render_restore(*_args):
    data = _quick_state.pop("data", None)
    if data:
        try:
            scene = data["scene"]
            scene.render.resolution_percentage = data["resolution"]
            scene.cycles.samples = data["samples"]
        except (AttributeError, ReferenceError, KeyError):
            pass
    for handlers in (bpy.app.handlers.render_complete, bpy.app.handlers.render_cancel):
        if _quick_render_restore in handlers:
            handlers.remove(_quick_render_restore)


class CYCLES_OT_light_mix_quick_render(Operator):
    """Render with fewer samples and a lower resolution, just to have the light groups to mix, """ \
        """then put the render settings back"""
    bl_idname = "cycles.light_mix_quick_render"
    bl_label = "Quick Render"

    @classmethod
    def poll(cls, context):
        return context.scene is not None and context.engine == 'CYCLES'

    def execute(self, context):
        scene = context.scene
        settings = scene.cycles_light_mix
        _quick_state["data"] = {
            "scene": scene,
            "resolution": scene.render.resolution_percentage,
            "samples": scene.cycles.samples,
        }
        for handlers in (bpy.app.handlers.render_complete, bpy.app.handlers.render_cancel):
            if _quick_render_restore not in handlers:
                handlers.append(_quick_render_restore)
        scene.render.resolution_percentage = settings.quick_resolution
        scene.cycles.samples = settings.quick_samples
        try:
            bpy.ops.render.render('INVOKE_DEFAULT')
        except RuntimeError as ex:
            _quick_render_restore()
            self.report({'ERROR'}, tip_("Could not start the render: {:s}").format(str(ex)))
            return {'CANCELLED'}
        return {'FINISHED'}


# ---------------------------------------------------------------------------
# Interface
# ---------------------------------------------------------------------------

class CYCLES_UL_light_mix(UIList):
    def draw_item(self, context, layout, data, item, _icon, _active_data, _active_prop, _index):
        view_layer = data
        icon, instanced = _member_info(context.scene).get(item.name, ('LIGHT_DATA', False))
        solo = any_solo(view_layer)

        row = layout.row(align=True)
        row.prop(item, "use_mix_solo", text="", emboss=False, icon='SOLO_ON' if item.use_mix_solo else 'SOLO_OFF')
        row.prop(item, "use_mix", text="", emboss=False, icon='HIDE_OFF' if item.use_mix else 'HIDE_ON')
        sub = row.row(align=True)
        sub.active = item.use_mix and (item.use_mix_solo or not solo)
        sub.label(text=item.name, icon=icon)
        if instanced:
            sub.label(text="", icon='LINKED')
        color = sub.row(align=True)
        color.ui_units_x = 1.5
        color.prop(item, "mix_color", text="")
        sub.prop(item, "mix_value", text="")
        sub.prop(item, "mix_exposure", text="")
        row.operator("cycles.light_mix_select", text="", icon='RESTRICT_SELECT_OFF', emboss=False).group = item.name


def draw_light_mix(layout, context):
    """Light Mix of the view layer: scan, the mix list, the master and the workflow operators."""
    scene = context.scene
    view_layer = context.view_layer

    col = layout.column()
    row = col.row(align=True)
    row.operator("cycles.light_mix_scan", icon='FILE_REFRESH')
    row.operator("cycles.light_mix_quick_render", icon='SHADING_RENDERED')
    row.operator("render.render", text="Render", icon='RENDER_STILL')

    if CYCLES_OT_light_mix_import_addon.poll(context):
        col.operator("cycles.light_mix_import_addon", icon='IMPORT')

    if not view_layer.lightgroups:
        col.label(text="No light groups yet, press Scan Lights", icon='INFO')
        return

    if not view_layer.use_light_mix:
        col.prop(view_layer, "use_light_mix", text="Enable Light Mix", icon='CHECKBOX_DEHLT')

    mix = col.column()
    mix.active = view_layer.use_light_mix
    mix.template_list(
        "CYCLES_UL_light_mix", "light_mix", view_layer,
        "lightgroups", view_layer, "active_lightgroup_index", rows=6,
    )

    lightgroup = view_layer.active_lightgroup
    if lightgroup is not None:
        box = mix.box()
        box.label(text=lightgroup.name, icon='LIGHT_DATA')
        sub = box.column(align=True)
        sub.use_property_split = True
        sub.use_property_decorate = False
        sub.active = lightgroup.use_mix
        sub.prop(lightgroup, "mix_value", text="Value")
        sub.prop(lightgroup, "mix_exposure", text="Exposure")
        sub.prop(lightgroup, "mix_color", text="Tint")

    box = mix.box()
    box.label(text="Master")
    sub = box.column(align=True)
    sub.use_property_split = True
    sub.use_property_decorate = False
    sub.prop(view_layer, "light_mix_value", text="Value")
    sub.prop(view_layer, "light_mix_exposure", text="Exposure")
    sub.prop(view_layer, "light_mix_color", text="Tint")

    col.separator()
    col.operator("cycles.light_mix_bake", icon='CHECKMARK')
    row = col.row(align=True)
    row.operator("cycles.light_mix_reset", icon='LOOP_BACK')
    row.operator("cycles.light_mix_clear_all", icon='TRASH')


def draw_light_mix_options(layout, context):
    settings = context.scene.cycles_light_mix
    layout.use_property_split = True
    layout.use_property_decorate = False

    layout.prop(settings, "naming_mode")
    if settings.naming_mode == 'PREFIX':
        layout.prop(settings, "separator")
    elif settings.naming_mode == 'REGEX':
        layout.prop(settings, "custom_regex")
    if settings.naming_mode != 'AUTO':
        layout.prop(settings, "strip_suffix")
    layout.prop(settings, "prefix")

    layout.separator()
    col = layout.column(heading="Include")
    col.prop(settings, "include_world")
    col.prop(settings, "include_instanced_collections")
    col.prop(settings, "include_emissive")
    sub = layout.column()
    sub.active = settings.include_emissive
    sub.prop(settings, "emissive_mode")
    if settings.emissive_mode == 'SINGLE':
        sub.prop(settings, "emissive_group_name")

    layout.separator()
    layout.prop(settings, "purge_unused")

    layout.separator()
    col = layout.column(align=True)
    col.prop(settings, "quick_samples")
    col.prop(settings, "quick_resolution")
    layout.prop(settings, "show_header_button")


class CyclesLightMixPanel:
    bl_label = "Light Mix"

    @classmethod
    def poll(cls, context):
        return context.engine == 'CYCLES' and context.view_layer is not None

    def draw_header(self, context):
        self.layout.prop(context.view_layer, "use_light_mix", text="")

    def draw(self, context):
        draw_light_mix(self.layout, context)


class CyclesLightMixOptionsPanel:
    bl_label = "Options"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        draw_light_mix_options(self.layout, context)


class CYCLES_VIEW_LAYER_PT_light_mix(CyclesLightMixPanel, CyclesButtonsPanel, Panel):
    bl_context = "view_layer"
    bl_description = (
        "Mix the light groups without rendering again. Changes show at once in the viewport, the render window and "
        "the render result"
    )


class CYCLES_VIEW_LAYER_PT_light_mix_options(CyclesLightMixOptionsPanel, CyclesButtonsPanel, Panel):
    bl_context = "view_layer"
    bl_parent_id = "CYCLES_VIEW_LAYER_PT_light_mix"


class CYCLES_IMAGE_PT_light_mix(CyclesLightMixPanel, Panel):
    bl_space_type = 'IMAGE_EDITOR'
    bl_region_type = 'UI'
    bl_category = "Light Mix"


class CYCLES_IMAGE_PT_light_mix_options(CyclesLightMixOptionsPanel, Panel):
    bl_space_type = 'IMAGE_EDITOR'
    bl_region_type = 'UI'
    bl_category = "Light Mix"
    bl_parent_id = "CYCLES_IMAGE_PT_light_mix"


class CYCLES_IMAGE_PT_light_mix_popover(Panel):
    """Light Mix in the Image Editor header, which is also the header of the render window"""
    bl_label = "Light Mix"
    bl_space_type = 'IMAGE_EDITOR'
    bl_region_type = 'HEADER'
    bl_ui_units_x = 22

    @classmethod
    def poll(cls, context):
        return context.engine == 'CYCLES' and context.view_layer is not None

    def draw(self, context):
        layout = self.layout
        layout.prop(context.view_layer, "use_light_mix")
        draw_light_mix(layout, context)


class CYCLES_VIEW3D_PT_light_mix(CyclesLightMixPanel, Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Light Mix"


class CYCLES_VIEW3D_PT_light_mix_options(CyclesLightMixOptionsPanel, Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Light Mix"
    bl_parent_id = "CYCLES_VIEW3D_PT_light_mix"


class CYCLES_NODE_PT_light_mix(CyclesLightMixPanel, Panel):
    bl_space_type = 'NODE_EDITOR'
    bl_region_type = 'UI'
    bl_category = "Light Mix"

    @classmethod
    def poll(cls, context):
        return super().poll(context) and getattr(context.space_data, "tree_type", "") == "CompositorNodeTree"


def draw_image_header_button(self, context):
    if context.engine != 'CYCLES' or context.scene is None:
        return
    if not context.scene.cycles_light_mix.show_header_button:
        return
    if getattr(context.space_data, "mode", 'VIEW') != 'VIEW':
        return
    self.layout.popover("CYCLES_IMAGE_PT_light_mix_popover", text="Light Mix", icon='LIGHT')


classes = (
    CyclesLightMixSettings,
    CYCLES_OT_light_mix_scan,
    CYCLES_OT_light_mix_select,
    CYCLES_OT_light_mix_reset,
    CYCLES_OT_light_mix_bake,
    CYCLES_OT_light_mix_clear_all,
    CYCLES_OT_light_mix_import_addon,
    CYCLES_OT_light_mix_quick_render,
    CYCLES_UL_light_mix,
    CYCLES_VIEW_LAYER_PT_light_mix,
    CYCLES_VIEW_LAYER_PT_light_mix_options,
    CYCLES_IMAGE_PT_light_mix,
    CYCLES_IMAGE_PT_light_mix_options,
    CYCLES_IMAGE_PT_light_mix_popover,
    CYCLES_VIEW3D_PT_light_mix,
    CYCLES_VIEW3D_PT_light_mix_options,
    CYCLES_NODE_PT_light_mix,
)


def register():
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
    bpy.types.Scene.cycles_light_mix = PointerProperty(
        name="Cycles Light Mix Settings",
        description="Cycles Light Mix scan and workflow options",
        type=CyclesLightMixSettings,
    )
    bpy.types.IMAGE_HT_header.append(draw_image_header_button)
    for handlers in (bpy.app.handlers.depsgraph_update_post, bpy.app.handlers.load_post):
        if _member_info_invalidate not in handlers:
            handlers.append(_member_info_invalidate)


def unregister():
    from bpy.utils import unregister_class
    for handlers in (bpy.app.handlers.depsgraph_update_post, bpy.app.handlers.load_post):
        if _member_info_invalidate in handlers:
            handlers.remove(_member_info_invalidate)
    _quick_render_restore()
    bpy.types.IMAGE_HT_header.remove(draw_image_header_button)
    del bpy.types.Scene.cycles_light_mix
    for cls in reversed(classes):
        unregister_class(cls)
