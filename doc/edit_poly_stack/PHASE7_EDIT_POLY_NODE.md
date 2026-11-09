# Edit Poly node (Geometry Nodes)

`GeometryNodeEditPoly`, `node_geo_edit_poly.cc`; storage and evaluation in `MOD_nodes.cc`; sessions
in `scripts/startup/bl_operators/edit_poly.py`.

The same edits as an Edit Poly modifier level (moves, topology patch, operation log, element data,
selection), made at a point inside a node tree.

## Rules

- The node is only ever added by hand (Shift+A in the node editor). Nothing adds it automatically.
- Tab on an object whose active modifier is a Geometry Nodes modifier with Edit Poly nodes enters
  a session on one of them:
  1. the active node of the node tree, if it is an Edit Poly node;
  2. otherwise the Edit Poly node closest to the output of the node tree.
- Without Edit Poly nodes, or when another modifier is active, Tab does exactly what it did before
  (including placing an Edit Poly modifier).
- Tab again stores the edits and leaves the session, as for a modifier level.

## Panel

The Geometry Nodes modifier shows an Edit Poly panel when its node tree has Edit Poly nodes: one
row per node (in a node group: "Group > Node") with what is stored, an Edit button (a session on
that node) and Reset (removes its edits). Muted nodes (M) are switched off: their edits are kept
but not applied, Tab skips them and Edit is disabled.

## Storage

Node groups are shared between objects, so the edits are not stored in the node. Like baked data
of Bake nodes, they are stored per object in the Geometry Nodes modifier
(`NodesModifierData::edit_polys`), one entry per nested node ID (`bNestedNodeRef`), so an Edit Poly
node inside a node group used by several objects keeps separate edits for each of them. Each entry
holds an `EditPolyModifierData`, so the modifier's code applies, records and saves them.

Entries are created on the first stored session and removed when their node no longer exists in the
node tree (kept while a linked node group is missing, like bakes).

## Evaluation

The node applies its entry to the incoming mesh exactly like an Edit Poly modifier level
(`edit_poly::apply`): topology patch (or re-run operations), moves, element data, selection. Other
geometry (curves, instances...) passes through unchanged. Without stored edits the node passes the
geometry through.

Elements are found by stable IDs. Most nodes do not write them; without them, elements are found by
index. That is only right while the number of points and faces is the one the edits were made on, so
each level stores those numbers when it is edited (`input_verts_num`, `input_faces_num`): if they
change and the mesh has no stable IDs, the edits pause instead of landing on other elements, and the
level cannot be edited until the numbers match again or its edits are reset. This also applies to
Edit Poly modifiers. A node that changes the order of elements without changing their number is not
detected. `tools/edit_poly_stack/benchmark_node_edit_poly.py` reports what happens in typical setups.

## Selection outputs

Selected Points, Selected Edges and Selected Faces output the selection stored when the node was
last edited (the live Edit Mode selection while it is edited), like an Edit Poly level passes its
selection up the stack. For example Selected Edges into the Selection of a Bevel node bevels only
the edges selected in the node. Without stored edits, or while the edits are paused, nothing is
selected.

## Sessions

1. The node is asked to keep a copy of the mesh that arrives at it and of its result (capture), and
   the object is evaluated.
2. The result is edited in Edit Mode as for a modifier level (session IDs, operation log); the
   modifiers before the Geometry Nodes modifier are hidden.
3. While editing, the node outputs the edited mesh (the input of the modifier), so the nodes after
   it and the modifiers above show the edit live.
4. Tab records the edits into the node's entry, as `session_commit` does for a level.

Check: `tests/python/bl_node_edit_poly.py`.
