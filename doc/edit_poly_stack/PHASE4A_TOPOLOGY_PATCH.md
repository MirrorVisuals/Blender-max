# Edit Poly stack, phase 4a: topology patches (any tool)

Status: first version. Any Edit Mode tool can be used inside an Edit Poly level.

## What is stored

When a session ends, the edited mesh is compared with the mesh as it arrives at the level
(`patch_compute` in `modifiers/intern/MOD_edit_poly_patch.cc`):

- vertices that kept their input ID become moves (as before);
- removed input vertices and faces, by ID;
- new vertices: ID, anchor (an input vertex ID) and offset from the anchor. Copies made by tools
  such as Extrude are anchored to the vertex they were copied from; vertices without a usable ID
  (e.g. merges) to the nearest input vertex. New vertices from earlier sessions keep their IDs;
- new faces: ring of vertex IDs, ID (a rebuilt face keeps its input ID), source face for
  copying face and corner attributes (materials, UVs).

The patch is stored as two flat arrays (`patch_ints`, `patch_floats`, versioned) in
`EditPolyModifierData` and saved in the .blend file.

## Replay (`patch_apply`)

Kept input vertices and faces in their order, then new vertices (anchor position + offset) and new
faces; edges rebuilt from faces, loose input edges kept; generic attributes copied from the input
element, anchor, source face or matching source corner; stable IDs written for all domains. Moves
are applied after the patch.

If anything referenced is missing in the input (the topology below changed, e.g. a Subdivision
level), the whole patch is paused with a warning. A paused level cannot be edited (Tab reports
why), because editing it would replace the paused edits.

## Python API

`patch_new_vertices`, `patch_new_faces`, `patch_removed_faces` (read-only), `patch_clear()`,
`session_check(input)`, `session_prepare(input, mesh)`, `session_commit(input, mesh)`.

## Check

`CHECK_EDIT_POLY_TOPOLOGY.bat`: extrude on a subdivided cube, edit again, deform the base
(extrusion follows), Subdivision level change (patch pauses, editing refused, comes back), delete
faces, inset + bevel in one session, save/reload, clear.

## Limits / next (phase 4b)

- New loose edges (not part of a face) are not stored.
- UVs of new faces come from the nearest corner of the source face (approximate).
- Re-runnable operations for extrude / inset / bevel, so they survive a Subdivision level change
  via the parent lookup.
