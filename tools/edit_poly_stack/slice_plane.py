# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Slice Plane, as in 3ds Max.

A Plane empty (Add > Empty > Plane, or the Add Slice Plane tool in Object Mode) is a helper for
an infinite plane. In Edit Mode, Mesh > Slice cuts the selected geometry of every object in Edit
Mode with it. The empty's location is a point on the plane and its local Z axis (the line drawn
from its center) is the plane's normal: "Above" is the side the line points to.

The planes used are the selected Plane empties, or every visible Plane empty when none is
selected. The plane is infinite: the size of the empty is only for display.
"""

import bpy
import bmesh
from bpy.types import Operator
from bpy.props import BoolProperty, EnumProperty, FloatProperty

# Temporary layer that marks the geometry to cut, so that a second plane cuts the pieces that the
# first plane made from the selection (they are not all selected after a cut).
LAYER_NAME = "__slice_plane_region"


def is_slice_plane(ob):
    return ob.type == 'EMPTY' and ob.empty_display_type == 'PLANE'


def slice_planes(context):
    """The Plane empties to cut with, as (name, world point, world normal)."""
    planes = [ob for ob in context.selected_objects if is_slice_plane(ob)]
    if not planes:
        planes = [ob for ob in context.visible_objects if is_slice_plane(ob)]
    result = []
    for ob in sorted(planes, key=lambda ob: ob.name):
        matrix = ob.matrix_world
        normal = matrix.to_3x3().col[2]
        if normal.length < 1e-8:
            continue
        result.append((ob.name, matrix.translation.copy(), normal.normalized()))
    return result


def _region_layers(bm):
    return (
        bm.verts.layers.int.get(LAYER_NAME) or bm.verts.layers.int.new(LAYER_NAME),
        bm.edges.layers.int.get(LAYER_NAME) or bm.edges.layers.int.new(LAYER_NAME),
        bm.faces.layers.int.get(LAYER_NAME) or bm.faces.layers.int.new(LAYER_NAME),
    )


def _remove_region_layers(bm):
    for seq in (bm.verts, bm.edges, bm.faces):
        layer = seq.layers.int.get(LAYER_NAME)
        if layer is not None:
            seq.layers.int.remove(layer)


def slice_bmesh(bm, matrix_world, planes, *, mode='SLICE', use_fill=False, threshold=0.0001):
    """
    Cut the selected geometry of ``bm`` with world space ``planes`` (point, normal).
    Afterwards the cut edges (and filled faces) are selected, as with Bisect.
    Returns the number of cut edges.
    """
    vert_layer, edge_layer, face_layer = _region_layers(bm)
    for seq, layer in ((bm.verts, vert_layer), (bm.edges, edge_layer), (bm.faces, face_layer)):
        for ele in seq:
            ele[layer] = 1 if ele.select else 0

    matrix_inv = matrix_world.inverted_safe()
    # Normals go from world to local space with the transpose of the 3x3 matrix.
    matrix_normal = matrix_world.to_3x3().transposed()
    clear_inner = mode == 'REMOVE_BELOW'
    clear_outer = mode == 'REMOVE_ABOVE'

    cut_edges = []
    for co, no in planes:
        co_local = matrix_inv @ co
        no_local = matrix_normal @ no
        if no_local.length < 1e-12:
            continue
        no_local.normalize()

        geom = (
            [v for v in bm.verts if v[vert_layer]] +
            [e for e in bm.edges if e[edge_layer]] +
            [f for f in bm.faces if f[face_layer]]
        )
        if not geom:
            break
        result = bmesh.ops.bisect_plane(
            bm,
            geom=geom,
            dist=threshold,
            plane_co=co_local,
            plane_no=no_local,
            clear_inner=clear_inner,
            clear_outer=clear_outer,
        )
        new_cut_edges = [ele for ele in result["geom_cut"] if isinstance(ele, bmesh.types.BMEdge)]
        for ele in result["geom_cut"]:
            if isinstance(ele, bmesh.types.BMVert):
                # 2 marks the vertices of this cut, to find the fill faces below.
                ele[vert_layer] = 2
            else:
                ele[edge_layer] = 1
        cut_edges += new_cut_edges

        if use_fill and new_cut_edges:
            filled = bmesh.ops.triangle_fill(
                bm, use_beauty=True, use_dissolve=True, edges=new_cut_edges, normal=no_local,
            )["geom"]
            # With "use_dissolve" some of the returned faces have been dissolved already.
            faces = [ele for ele in filled if isinstance(ele, bmesh.types.BMFace) and ele.is_valid]
            if faces:
                # This invalidates the Python references to the faces (they can be flipped).
                bmesh.ops.face_attribute_fill(bm, faces=faces, use_normals=True, use_data=True)
                for face in bm.faces:
                    if all(v[vert_layer] == 2 for v in face.verts):
                        # 2 marks the fill faces, to select them at the end.
                        face[face_layer] = 2
        for ele in result["geom_cut"]:
            if isinstance(ele, bmesh.types.BMVert) and ele.is_valid:
                ele[vert_layer] = 1

    cut_edges = [e for e in cut_edges if e.is_valid]

    if mode == 'SPLIT' and cut_edges:
        result = bmesh.ops.split_edges(bm, edges=cut_edges)
        cut_edges = [e for e in result["edges"] if e.is_valid]

    for seq in (bm.verts, bm.edges, bm.faces):
        for ele in seq:
            ele.select = False
    for edge in cut_edges:
        edge.select = True
    for face in bm.faces:
        if face[face_layer] == 2:
            face.select = True
    bm.select_flush_mode()

    # Last: removing a layer invalidates the Python references to the elements.
    _remove_region_layers(bm)
    return len(cut_edges)


class MESH_OT_slice_plane(Operator):
    """Cut the selected geometry with the Plane empties (the selected ones, or all visible ones """ \
        """when none is selected). The plane is infinite, its normal points to the "Above" side"""
    bl_idname = "mesh.slice_plane"
    bl_label = "Slice"
    bl_options = {'REGISTER', 'UNDO'}

    mode: EnumProperty(
        name="Mode",
        items=(
            ('SLICE', "Slice", "Add edges where the plane cuts the geometry"),
            ('SPLIT', "Split", "Cut and split the geometry into two parts along the plane"),
            ('REMOVE_ABOVE', "Remove Above", "Cut and remove the geometry on the side the plane's normal points to"),
            ('REMOVE_BELOW', "Remove Below", "Cut and remove the geometry behind the plane"),
        ),
        default='SLICE',
    )
    use_fill: BoolProperty(
        name="Fill",
        description="Fill in the cut",
    )
    threshold: FloatProperty(
        name="Axis Threshold",
        description="Preserves the existing geometry along the cut plane",
        min=0.00001,
        max=1.0,
        default=0.0001,
        precision=5,
        step=0.1,
        subtype='DISTANCE',
    )

    @classmethod
    def poll(cls, context):
        return context.mode == 'EDIT_MESH'

    def execute(self, context):
        planes = slice_planes(context)
        if not planes:
            self.report({'ERROR'}, "No Plane empty to slice with, add one with Add > Empty > Plane")
            return {'CANCELLED'}

        plane_geometry = [(co, no) for _name, co, no in planes]
        total = 0
        for ob in context.objects_in_mode_unique_data:
            if ob.type != 'MESH':
                continue
            mesh = ob.data
            bm = bmesh.from_edit_mesh(mesh)
            if not any(v.select for v in bm.verts):
                continue
            total += slice_bmesh(
                bm,
                ob.matrix_world,
                plane_geometry,
                mode=self.mode,
                use_fill=self.use_fill,
                threshold=self.threshold,
            )
            bmesh.update_edit_mesh(mesh, loop_triangles=True, destructive=True)

        if total == 0 and self.mode in {'SLICE', 'SPLIT'}:
            self.report({'WARNING'}, "The plane does not cut the selected geometry")
        return {'FINISHED'}


classes = (
    MESH_OT_slice_plane,
)
