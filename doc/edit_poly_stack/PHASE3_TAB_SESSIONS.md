# Edit Poly stack, phase 3: Tab edits an Edit Poly level

Status: first version. Records moves (G/R/S, proportional editing, any tool that only moves
vertices). Topology tools are refused when leaving the level.

## Tab rules (`object.edit_poly_tab`, bound to Tab in the default keymap)

1. Active modifier is an Edit Poly: edit it.
2. Active modifier is something else: edit the nearest Edit Poly below it.
3. Otherwise the topmost Edit Poly.
4. No Edit Poly: add one at the top and edit it (`AUTO_ADD` in `bl_operators/edit_poly.py`).

Non-mesh objects and other modes keep the normal Tab behaviour. The mode dropdown still offers
classic Edit Mode on the base mesh as an escape hatch.

## How a session works

- Enter: evaluate the stack below the level (`new_from_object`), stamp each vertex with its
  stable ID and start position (`EditPolyModifier.session_prepare`, attributes
  `edit_poly_input_id`, `edit_poly_input_position`), apply the level's current moves, and give
  the object this mesh. Modifiers up to and including the level are hidden (their result is in
  the mesh); modifiers above stay visible = end result. Session state is in custom properties
  `_edit_poly_*` on the object, so it survives save, reload and undo.
- Leave (Tab again): `EditPolyModifier.session_commit` stores position - start as moves by ID,
  removes moves of vertices back at their start, keeps paused moves. Then the original mesh and
  modifier visibility are restored and the temporary mesh is removed.
- Topology changed (counts differ or duplicate/missing IDs): refused, stays in Edit Mode. Undo
  the change, or `object.edit_poly_session_discard` (F3: "Discard Edit Poly Session").

## Files

- `scripts/startup/bl_operators/edit_poly.py`: Tab, Discard, Edit Level operators
- `scripts/presets/keyconfig/keymap_data/blender_default.py`: Tab -> `object.edit_poly_tab`
- `modifiers/MOD_edit_poly.hh`, `modifiers/intern/MOD_edit_poly.cc`: session prepare / commit
- `makesrna/intern/rna_modifier.cc`: `session_prepare`, `session_commit`

## Known limits

- Moves are in object space of the level's input.
- Other modes (UV, weight paint, sculpt) still work on the base mesh.
- Topology tools: next phase (record operations, map them through stable IDs).
