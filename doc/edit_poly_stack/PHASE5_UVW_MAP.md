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

## Gizmo editing (`scripts/startup/bl_operators/uvw_map.py`)

- Tab when the active (or hovered) modifier is a UVW Map, or its Edit Gizmo button: an empty
  (cube display, parented to the object, identity parent inverse) is made from the gizmo values
  and set as Gizmo Object, then selected. G / R / S move the map live.
- Tab on that empty (or Edit Gizmo again): its transform is stored back in the modifier and the
  empty is removed. A Gizmo Object the user set by hand is never removed by Edit Gizmo.
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
