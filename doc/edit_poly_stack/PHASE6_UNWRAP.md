# Unwrap modifier

`eModifierType_Unwrap`, `MOD_unwrap.cc`, `scripts/startup/bl_operators/unwrap.py`.

## Storage

UVs are stored per face by stable IDs (see the comment on `UnwrapModifierData`): face ID, parent
face ID, corner vertex IDs, corner UVs, and the subdivision layout of each corner
(`.stable_subd_uv`) if the face came from a subdivision.

## Evaluation: UVs for each face, in order

1. The face is stored: its corners by vertex ID.
2. Stored faces were split from it (Subdivision lowered): its corners by vertex ID in them.
3. It was split from a face (Subdivision added / raised): the stored face it lies in, in the
   subdivision layout (the stored face itself, laid out on the unit circle in canonical corner
   order, or the stored faces split from the same base face with their stored layout). The face
   center picks the stored face; the corners are interpolated with mean value coordinates, which
   is exact for UVs that are linear across a flat face.
4. It is a Mirror / Array copy: its source face, by the source vertex IDs.
5. Otherwise it is new.

New faces are flagged (`.unwrap_new`, and the modifier message counts them) and either
- Unwrap (default): LSCM over the new faces, with corners on vertices that already have UVs
  pinned to them, so they attach; islands with fewer than two such corners are scaled to the
  density of the stored UVs and laid out to the right of the 0..1 tile; or
- Keep: they keep the UVs that arrive from below.

## Sessions

Tab when an Unwrap is the active (or hovered) modifier, or Edit UVs: the object uses a copy of
the Unwrap's result in Edit Mode, the modifiers up to the Unwrap are hidden, and a UV Editor
window opens if none is visible. New faces are selected, with UV Sync Selection on. Tab stores
all UVs by ID (replacing the stored ones) and puts the original mesh back. Only UVs are kept.

Reset removes the stored UVs. Check: `check_unwrap.py`.
