# Edit Poly stack, phase 4b: re-runnable operations

Status: first version. Extrude (region, along the extrusion direction), Inset (region and
individual) and Bevel (edges, superellipse profile) survive a Subdivision level change below.

## Capture (Python, `bl_operators/edit_poly.py`)

While a level is edited, a depsgraph handler watches `WindowManager.operators` (the operators
behind Adjust Last Operation) and logs extrude / inset / bevel with their final settings and the
session IDs of the faces or edges they used. The selection is snapshotted after each change,
except while a modal tool runs (cheap, and the snapshot is the selection the next tool uses).
The extrusion offset is measured from the geometry (copies of each vertex share its session ID).

Session IDs are now pairs of plain integer attributes (`edit_poly_*_id_lo/hi`) so Python can read
them from a BMesh.

## Validation (C++, `session_commit`)

The level's log plus the session's operations is replayed on the level's input
(`ops_apply`, BMesh operators `extrude_face_region`, `inset_region` / `inset_individual`,
`bevel`). The log is kept only if the replay has the same vertex, edge and face counts as the
edited mesh; otherwise (undo, other tools, chains on new geometry) it is dropped and the level
behaves as in phase 4a. Footprints are stored with each element.

## Footprints and re-targeting

Subdivision writes `.stable_subd_uv` (corner domain): each corner's position inside its base face,
in a layout independent of corner order and winding. A logged element's footprint is its parent
base face and these positions. On another level, a face is covered by a face footprint if its
centroid lies inside it (finer) or the footprint's centroid lies inside the face (coarser); an
edge is covered if both ends lie on the footprint segment.

## Replay in the modifier

The exact patch is used when it applies. When it is paused and the log can be re-targeted, the
operations are re-run on the covering elements; moves of points that still exist are applied; the
modifier says so in its warning. A level whose patch is paused still cannot be edited.

## Limits

- Operations on geometry created by earlier operations (chains), other tools, custom bevel
  profiles and hardened normals are not logged; such levels pause as before.
- Moves made on new geometry after an operation are not carried to other levels.
- Only Subdivision provides footprints so far.

## Check

`CHECK_EDIT_POLY_OPS.bat`.
