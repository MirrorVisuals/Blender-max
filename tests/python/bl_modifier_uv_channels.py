# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: Apache-2.0

# UV map channels check: Map Channel on UVW Map and Unwrap (1 = default UV map, N = "UVChannel_N";
# setting it does not change the mesh, the modifier writes the UV map to its result, Add UV Map
# adds it to the mesh), two UVW Maps on different channels with different sizes, and Connect to
# Texture (a UV Map node in front of the selected texture node, the UV map added to the mesh).
# ./blender.bin --background --factory-startup --python tests/python/bl_modifier_uv_channels.py
import sys
import bpy

failures = []


def check(cond, msg):
    print(("  ok    " if cond else "  FAIL  ") + msg)
    if not cond:
        failures.append(msg)


bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_cube_add(size=2.0)
ob = bpy.context.active_object
me = ob.data
for layer in list(me.uv_layers):
    me.uv_layers.remove(layer)

print("Map Channel")
base = ob.modifiers.new("Base", 'UVW_MAP')
base.map_type = 'BOX'
check(base.map_channel == 1, f"a new UVW Map is on channel 1, the default UV map ({base.map_channel})")
detail = ob.modifiers.new("Detail", 'UVW_MAP')
detail.map_type = 'BOX'
detail.map_channel = 2
check(detail.uv_layer == "UVChannel_2" and detail.map_channel == 2,
      f"channel 2 writes the UV map \"UVChannel_2\" ({detail.uv_layer!r})")
check(len(me.uv_layers) == 0, "setting the channel does not change the mesh")
dg = bpy.context.evaluated_depsgraph_get()
ev_names = list(ob.evaluated_get(dg).data.uv_layers.keys())
check("UVMap" in ev_names and "UVChannel_2" in ev_names,
      f"the modifiers write both UV maps to their result ({ev_names})")
bpy.ops.object.uv_map_add_for_modifier(modifier=detail.name)
names = [layer.name for layer in me.uv_layers]
check(names == ["UVMap", "UVChannel_2"],
      f"Add UV Map: a mesh without UV maps gets \"UVMap\" (channel 1) first, then \"UVChannel_2\" ({names})")
default = next((l.name for l in me.uv_layers if l.active_render), None)
check(default == "UVMap", f"the default UV map stays \"UVMap\" ({default})")

# Different sizes: the detail map tiles 4 times as often.
base.size_mode = 'GIZMO'
detail.size_mode = 'GIZMO'
detail.gizmo_size = [s * 0.25 for s in base.gizmo_size]
dg = bpy.context.evaluated_depsgraph_get()
ev = ob.evaluated_get(dg).data


def uv_extent(name):
    data = ev.attributes[name].data
    us = [d.vector[0] for d in data]
    return max(us) - min(us)


ext_base, ext_detail = uv_extent("UVMap"), uv_extent("UVChannel_2")
check(abs(ext_detail - 4 * ext_base) < 1e-3,
      f"the two maps have their own sizes (U range {ext_base:.3f} and {ext_detail:.3f})")

detail.map_channel = 1
check(detail.uv_layer == "" and detail.map_channel == 1, "back to channel 1: the default UV map")
detail.map_channel = 2
detail.uv_layer = "UVMap"
check(detail.map_channel == 1, "picking the default UV map by name reads as channel 1")
detail.map_channel = 2

unwrap = ob.modifiers.new("Unwrap", 'UNWRAP')
unwrap.map_channel = 3
check(unwrap.uv_layer == "UVChannel_3" and "UVChannel_3" not in me.uv_layers,
      "Unwrap has the channel too (channel 3 is \"UVChannel_3\", not on the mesh yet)")

print("Connect to Texture")
mat = bpy.data.materials.new("Mat")
if mat.node_tree is None:
    mat.use_nodes = True
ob.data.materials.append(mat)
tree = mat.node_tree
image_node = tree.nodes.new('ShaderNodeTexImage')
image_node.location = (-400, 0)
tree.nodes.active = image_node
result = bpy.ops.object.uv_map_connect_texture(modifier=detail.name)
vec = image_node.inputs["Vector"]
uv_node = vec.links[0].from_node if vec.is_linked else None
check(result == {'FINISHED'} and uv_node is not None and uv_node.type == 'UVMAP' and
      uv_node.uv_map == "UVChannel_2",
      "a UV Map node reading \"UVChannel_2\" feeds the texture")
count = sum(1 for n in tree.nodes if n.type == 'UVMAP')
bpy.ops.object.uv_map_connect_texture(modifier=base.name)
check(sum(1 for n in tree.nodes if n.type == 'UVMAP') == count and uv_node.uv_map == "UVMap",
      "pressing it for another modifier changes the same node (no new one)")

# Texture Coordinate -> Mapping -> Image: the UV map goes into the Mapping node.
image2 = tree.nodes.new('ShaderNodeTexImage')
mapping = tree.nodes.new('ShaderNodeMapping')
coords = tree.nodes.new('ShaderNodeTexCoord')
tree.links.new(coords.outputs["UV"], mapping.inputs["Vector"])
tree.links.new(mapping.outputs["Vector"], image2.inputs["Vector"])
tree.nodes.active = image2
bpy.ops.object.uv_map_connect_texture(modifier=detail.name)
src = mapping.inputs["Vector"].links[0].from_node if mapping.inputs["Vector"].is_linked else None
check(src is not None and src.type == 'UVMAP' and src.uv_map == "UVChannel_2" and
      image2.inputs["Vector"].links[0].from_node == mapping,
      "through a Mapping node, the UV Map node feeds the Mapping node (which still feeds the texture)")

image3 = tree.nodes.new('ShaderNodeTexImage')
tree.nodes.active = image3
bpy.ops.object.uv_map_connect_texture(modifier=unwrap.name)
check("UVChannel_3" in me.uv_layers and image3.inputs["Vector"].is_linked and
      image3.inputs["Vector"].links[0].from_node.uv_map == "UVChannel_3",
      "connecting a texture adds the UV map to the mesh when it is not there")

tree.nodes.active = tree.nodes.new('ShaderNodeMath')
try:
    result = bpy.ops.object.uv_map_connect_texture(modifier=detail.name)
except RuntimeError:
    result = {'CANCELLED'}
check(result == {'CANCELLED'}, "a selected node without a Vector input is refused")

print()
print("RESULT:", "ALL PASSED" if not failures else f"{len(failures)} FAILED")
# A non-zero exit code makes the test fail when run by CTest.
sys.exit(1 if failures else 0)
