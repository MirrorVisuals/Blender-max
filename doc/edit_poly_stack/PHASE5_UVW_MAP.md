# UVW Map modifier and selection passing

## UVW Map (`eModifierType_UVWMap`, `MOD_uvw_map.cc`)

Projects UVs from a gizmo box, as in 3ds Max.

- Mapping: Planar, Cylindrical (optional Cap), Spherical, Shrink Wrap, Box, Face.
- Alignment X / Y / Z picks the gizmo axis the projection looks along.
- Gizmo: location, rotation (XYZ Euler) and size in object space, or a Gizmo Object (its
  transform times its empty display size; the box spans -1..1 of that).
- Tile (U, V), Offset, Flip U / V.
- UV Map: the named one (created if missing), else the active one, else a new "UVMap".
- Selected Faces Only: maps the faces whose `.select_poly` is set when they arrive; the other
  faces keep their UVs, so several UVW Maps can map different parts.

Seams: cylindrical / spherical faces that cross the seam get their low side moved by +1. Corners
on a sphere pole take the average U of the face's other corners.

## Real-world size (pixel density)

"Real-World Size" sizes the texture by Pixel Density (texture pixels per meter, world space) and
Texture Size instead of by the gizmo; the gizmo then only places and turns the map. One tile is
Texture Size / Pixel Density meters. Cylindrical and spherical U run around the gizmo's
circumference, so they follow the same density.

## Gizmo drawing and editing (`scripts/startup/bl_operators/uvw_map.py`)

- The gizmo is drawn in the 3D Viewport while a UVW Map is the active modifier (plane with a
  green right edge and an up tick, box, cylinder or sphere with a green seam). Brighter while
  editing.
- Tab when the active (or hovered) modifier is a UVW Map, or its Edit Gizmo button: a helper
  point (tiny empty) is made at the gizmo and selected. It is NOT linked as Gizmo Object: the
  mesh would then depend on it and transform snapping excludes objects that depend on what is
  being moved. A depsgraph handler copies the helper's transform into the gizmo values instead,
  so G / R / S, the Move / Rotate / Scale tools and snapping all work.
- Tab on the helper (or Edit Gizmo again) stores it and removes it. A helper that is deselected,
  or whose modifier was deleted, is removed by a timer after the depsgraph update.
- A Gizmo Object the user sets by hand is just selected by Edit Gizmo, never removed.
- Fit / Center: bounds of the faces the modifier maps (the selected ones with Selected Faces
  Only), measured in the gizmo's rotation, from the mesh as it arrives at the modifier.
- View Align: turns the gizmo so its U, V and projection axes are the view's right, up, depth.

## Edit Poly passes its face selection (`EditPolyModifierData.sel_face_ids`)

At commit, the faces selected in the session are stored by stable ID (`patch_compute` now reports
the resulting ID of every edited face, so new faces from extrude etc. are stored too). The
modifier then writes `.select_poly`: a face is selected if its ID, or its parent's ID
(`.stable_parent_face`, e.g. a Subdivision added below later), is stored. Entering the level again
shows that selection. "Pass Selection" (`use_selection`) turns it off; then the selection that
arrives from below passes through.

Check: `check_uvw_map.py`.
