# Edit Poly stack, phase 1: stable element IDs

Status: ID rule in a real Blender build; Subdivision writes IDs (step 2).

## Goal

Name every vertex, edge and face so an operation stored in an Edit Poly modifier can find its
target again after modifiers below it change, are reordered, or change resolution.

## What the survey found (checked on 5.4 dev; base branch is now v5.2.3 LTS, e13b728c)

- `CD_ORIGINDEX` already exists. Subdivision (`blenkernel/intern/subdiv_mesh.cc`), Array, Mirror,
  Bevel and others copy or fill it. It is a one-step link to the mesh the modifier received, and
  new elements get `ORIGINDEX_NONE`. It is not persistent across a stack, so it cannot be the ID.
  It is useful as the parent lookup inside a single modifier.
- There is no 64 bit attribute type. `AttrType::Int32_2D` (two int32) holds a 64 bit ID.
- Generic attribute interpolation blends integers, which is meaningless for IDs. Every
  geometry-creating modifier needs an explicit ID step rather than relying on propagation.
- A full build needs the prebuilt library bundle and hours on a small machine, so the core is a
  dependency-free header that is tested on its own.

## The ID rule

1. A surviving element keeps its ID.
2. A new element gets `derive(kind, parent IDs, small integers)`. Never from an index, thread
   count or iteration order.
3. A derived element also stores its primary parent ID, so `ParentIndex` can answer "which
   elements came from face F".

Implemented in `source/blender/blenkernel/BKE_stable_id.hh`:

| Piece | Purpose |
| --- | --- |
| `StableID` | 64 bit value stored as `Int32_2D`; `{0,0}` means none |
| `make_root` | IDs for a mesh with no history |
| `derive`, `derive_unordered` | child IDs; unordered for things like edges between two vertices |
| `Fraction` | positions in lowest terms, so 1/2 equals 2/4 equals 4/8 |
| `subd_edge_point`, `subd_face_point`, `subd_face_center` | subdivision rules |
| `canonical_start`, `canonical_direction` | a face frame that ignores the start corner and the winding |
| `resolve_collisions` | deterministic re-hash if two IDs ever match |
| `IDIndex` | ID to current index; `-1` means the element is gone, so the edit is paused |
| `ParentIndex` | parent ID to child indices |

Tested properties: the SubD midpoint has the same ID at levels 1, 2 and 3; edge direction,
face corner rotation and flipped normals do not change IDs; collisions resolve the same way
every run and never touch elements without an ID. A quad centre uses the (1/2, 1/2) face-point
rule, never `subd_face_center`, which is for n-gons only.

## Done in step 2: Subdivision writes IDs

- Attributes, one name per domain (names are unique within a mesh): `.stable_id_point`,
  `.stable_id_edge`, `.stable_id_face`, and `.stable_parent_*` for parents. Type `Int32_2D`.
- `BKE_stable_id_mesh.hh` / `stable_id_mesh.cc`: read IDs or make root IDs from indices
  (`read_or_make_root_ids`), write IDs and parents, `ensure_root_ids`.
- `subdiv_mesh.cc` (CPU subdivision, used by the modifier and the Subdivide node):
  - corner and loose vertices keep their ID;
  - edge vertices: `subd_edge_point`, position taken from the index layout (no float rounding);
  - quad interior: `quad_to_canonical` then `subd_face_point`; the quad centre is (1/2, 1/2);
  - n-gon: centre `subd_face_center`, points on the centre-to-midpoint lines
    `subd_ngon_spoke_point`, other patch points `subd_ngon_patch_point`;
  - edges and faces: derived from their vertex IDs; parent is the base edge or base face.
- ID attributes are no longer interpolated by subdivision (that gave meaningless values).
- 17 unit tests.

How to see it: Spreadsheet editor, enable "Show Internal Attributes". GPU subdivision skips the
CPU path when Subdivision is the last modifier, so turn it off in Preferences > Viewport while
testing. Collisions are not resolved inside subdivision (64 bit hashes; resolving would cost a
hash map per evaluation).

## Next steps

1. Store root IDs on the original mesh when it is created, imported or collapsed, so they survive
   index changes (needed once Edit Mode writes into an Edit Poly instead of the base mesh).
3. Mirror and Array: copies via `MirrorCopy` and `ArrayCopy`, original keeps its ID.
4. Bevel, Solidify, Weld, Decimate, Remesh: decide per modifier. Remesh and Decimate cannot
   preserve most IDs; they should clear IDs and mark the stack as "edits above are unmatched".
5. Save and load: `.stable_id` is a normal attribute, so it is stored with the mesh.
6. Then phase 2, the replay-only Edit Poly modifier, which stores offsets keyed by `StableID`.

## Open questions

- How to show the user that edits are paused, for example after lowering a SubD level.
- Whether Remesh and Decimate should be allowed below an Edit Poly at all.
- Geometry Nodes: IDs should pass through a node group untouched unless the nodes make new
  geometry, in which case the Extrude, Duplicate and Split nodes need the same rule.
