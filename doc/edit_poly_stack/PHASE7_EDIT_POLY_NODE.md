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

Elements are found by stable IDs. Nodes before the Edit Poly node that do not keep stable IDs give
the mesh new ones: the edits then pause, as an Edit Poly level does when the mesh below changes.

## Sessions

1. The node is asked to keep a copy of the mesh that arrives at it and of its result (capture), and
   the object is evaluated.
2. The result is edited in Edit Mode as for a modifier level (session IDs, operation log); the
   modifiers before the Geometry Nodes modifier are hidden.
3. While editing, the node outputs the edited mesh (the input of the modifier), so the nodes after
   it and the modifiers above show the edit live.
4. Tab records the edits into the node's entry, as `session_commit` does for a level.

Check: `tests/python/bl_node_edit_poly.py`.
