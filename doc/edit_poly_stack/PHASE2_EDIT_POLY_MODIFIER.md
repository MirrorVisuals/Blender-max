# Edit Poly stack, phase 2: replay-only Edit Poly modifier

Status: first version. Stores vertex moves only; no Tab / Edit Mode integration yet.

## What it does

- New modifier type `EDIT_POLY` (`eModifierType_EditPoly = 87`, `EditPolyModifierData`),
  in Add Modifier > Edit.
- Stores moves as pairs (stable vertex ID, offset). Two plain arrays: `move_ids` (2 ints per
  move) and `move_offsets` (3 floats per move), saved in the .blend file.
- On evaluation it reads `.stable_id_point` of the incoming mesh (root IDs from indices if there
  are none), finds each move's point by ID and adds the offset.
- A move whose point is not in the incoming mesh is paused, not deleted, and the modifier shows
  "N of M moves paused". It applies again when the point comes back (e.g. SubD level raised).

## Python API (for testing until Edit Mode writes into the modifier)

    mod = obj.modifiers.new("Edit Poly", 'EDIT_POLY')
    mod.move_set(stable_id=(lo, hi), offset=(x, y, z))   # replaces an earlier move of that ID
    mod.moves_clear()
    mod.moves_num                                         # read-only

## Files

- `makesdna/DNA_modifier_types.h`: enum value and struct
- `modifiers/intern/MOD_edit_poly.cc`: the modifier
- `modifiers/MOD_modifiertypes.hh`, `modifiers/intern/MOD_util.cc`, `modifiers/CMakeLists.txt`
- `makesrna/intern/rna_modifier.cc`: enum item, `EditPolyModifier`, `move_set`, `moves_clear`
- `scripts/startup/bl_ui/properties_data_modifier.py`: menu entry

## Check

`CHECK_EDIT_POLY.bat` runs `check_edit_poly.py`: a move on the top face centre of a subdivided
cube stays on that point at SubD levels 1, 2 and 3, pauses at level 0, comes back at level 1,
survives save and reload, and duplicates get their own copy.

## Next

- Offsets are in object space. When the shape below changes a lot, a move in the local frame of
  the point (normal / tangent) may follow the surface better. Decide after trying it.
- Phase 3: Tab enters the Edit Poly level; moving vertices in Edit Mode records moves here.
