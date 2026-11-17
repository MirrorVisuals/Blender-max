<!--
Keep this document short & concise,
linking to external resources instead of including content in-line.
See 'release/text/readme.html' for the end user read-me.
-->

Blender-max
===========

An experimental fork of **Blender 5.2.3 LTS** that brings a 3ds Max style modifier workflow to
Blender: editable modeling levels in the modifier stack, UVW Map and Unwrap modifiers, a solid
Extrude Manifold, a Slice Plane, camera tilt correction, a V-Ray style frame buffer with
interactive render and Light Mix, and a few more rendering extras. Branch: `edit-poly-stack`.
Experimental and unofficial: not affiliated with the Blender Foundation.

What is different from Blender 5.2.3
------------------------------------

### Stable element IDs

Vertices, edges and faces carry stable IDs (hidden attributes such as `.stable_id_point`).
Elements that survive an operation keep their ID; new elements get an ID derived from where
they came from. Subdivision Surface, Mirror and Array follow this rule. Edits stored by ID stay
on the same elements when the modifiers below them change, which is what makes the features
below possible.

### Edit Poly modifier

- **Tab edits a modifier level, not the base mesh.** It edits the active Edit Poly, or the
  nearest Edit Poly below the active modifier. Objects without an Edit Poly modifier or node
  get Blender's usual Edit Mode, so nothing changes until you place one. While you
  edit, the modifiers above stay visible. Tab again stores the edits in the modifier, and the
  base mesh is never changed.
- **Any modeling tool can be used.** Vertex moves are stored as moves. Topology changes
  (delete, extrude, inset, bevel and so on) are stored as a patch anchored to the mesh below.
- **Re-runnable operations.** Extrude, inset and bevel are also logged. If the mesh below
  changes (for example a Subdivision level), they are re-applied to the matching faces.
- **Several levels.** You can stack several Edit Poly levels and edit any of them. Click a level
  (or hover its panel) and press Tab.
- **Element data.** Bevel weights, creases, sharp edges, seams, sharp faces and material
  indices set in a level are stored in it by stable ID, like the moves.
- **Selection passing.** The vertices, edges and faces selected when you leave a level are
  passed up the stack, as with Edit Poly in 3ds Max (see UVW Map: Selected Faces Only and Bevel:
  Selection). They are also in a vertex group, "Edit Poly Selection", so any modifier with a
  vertex group field (Solidify, Displace, Smooth...) can work on the selection.
- **Collapse to Here** (modifier menu) bakes everything up to a modifier into the mesh while
  keeping the stable IDs.

### Edit Poly node (Geometry Nodes)

- **The same edits inside a node tree.** Add it with Shift+A (Mesh > Operations > Edit Poly) at
  any point of a Geometry Nodes tree. It is never added automatically.
- **Tab edits the node.** With the Geometry Nodes modifier active, Tab edits the mesh as it
  arrives at the active Edit Poly node, or else the one closest to the output, with any tool.
  The nodes after it update live. Without Edit Poly nodes, Tab works as before.
- **Stored per object.** The edits are stored in each object's Geometry Nodes modifier (like
  baked data), so objects sharing a node group keep their own edits.
- **Panel.** The Geometry Nodes modifier lists its Edit Poly nodes with what is stored, an
  Edit button and Reset.
- **Selection outputs.** Selected Points / Edges / Faces pass the node's selection on, e.g.
  Selected Edges into a Bevel node's Selection bevels only those edges (live while editing).
- **Tab in the node editor** edits the active Edit Poly node; on other nodes it enters groups
  as before.
- **Mute to switch off.** M on the node turns its edits off (they are kept); Tab skips muted
  nodes.

### UVW Map node (Geometry Nodes)

- **The UVW Map modifier as a node.** Shift+A > Mesh > UV > UVW Map. All the modifier's settings
  (projection, alignment, map channel, material, size modes, tile, offset, flip, gizmo or gizmo
  object) are in a UVW Map section of the Geometry Nodes modifier, stored per object.
- **Selection input** instead of Selected Faces Only, e.g. Selected Faces of an Edit Poly node.
- **Gizmo.** Drawn while the modifier is active; Edit Gizmo, Fit, Center and View Align work as
  for the modifier, and Tab edits the gizmo when the UVW Map node is the active node.
- **Never pauses.** The projection uses positions, so it keeps working whatever the nodes before
  it do.

### UVW Map modifier

- **Projections:** Planar, Box, Cylindrical (with optional Cap), Spherical, Shrink Wrap and Face.
- **Map controls:** X / Y / Z alignment, tile, offset and flip.
- **Size:** from the gizmo, as a Real-World Size (metres per texture tile), or as a Pixel
  Density (px/m for a texture size), with presets for common texture sizes and densities.
- **Material ID:** maps only the faces of one material slot of the object.
- **Map Channel,** as in 3ds Max: 1 is the default UV map, 2 and up are UV maps
  "UVChannel_2" and so on, added when needed. Several UVW Maps on different channels give the
  textures of one material their own sizes. **Connect to Texture** makes the selected texture
  node of the material read the modifier's UV map. Unwrap has the same channel and button.
- **Selected Faces Only:** maps only the faces passed up by the Edit Poly level below.
- **Gizmo:** drawn in the viewport. Tab (or Edit Gizmo) lets you move, rotate and scale it,
  with snapping, and the texture follows live. Fit, Center and View Align are available, and
  the alignment buttons reset the gizmo rotation. With several UVW Maps, Tab over another
  map's panel switches to its gizmo.

### Unwrap modifier

- **Stored UVs:** Tab opens a UV session in the UV Editor, where all of Blender's UV tools work.
  Tab again stores the UVs by stable ID.
- **Follows the stack:** the stored UVs follow the mesh below. Subdivision level up or down,
  Mirror and Array copies all keep their UVs.
- **New faces:** faces added below are either auto-unwrapped and attached to the stored UVs
  around them, or keep the UVs that arrive from below. Either way they are flagged and
  pre-selected in the next session.
- **Quick buttons:** Smart Project, Unwrap and Pack, without a session.

### Bevel modifier: Limit Method Selection

Bevels only the edges (or vertices) selected in the Edit Poly level below.

### Extrude Manifold: solid extrude, as in 3ds Max

On a closed mesh, Extrude Manifold now works on the volume:

- **Pushing faces in** cuts away the volume they pass through. Side walls that end up against
  existing faces disappear, at any depth, and pushing through the mesh opens a hole.
- **Pulling faces out** adds the volume, merging with any part of the mesh it runs into.
- **Open meshes,** or the **Solid** option turned off (in the operator panel), work exactly as
  Blender's Extrude Manifold always did.

It uses the fast Manifold Boolean solver (with the exact solver as a fallback) on the
connected part being edited only. With 50,000 faces it takes about 0.15 s; other parts of the
mesh add almost nothing.

### Add Plane tool

The Add tool in the toolbar (Object and Edit Mode) can draw a plane, like the cube and the
other primitives: drag its base and it is added with the size drawn.

### Slice Plane, as in 3ds Max

- **Plane empty:** Add > Empty > Plane, or the **Add Slice Plane** tool in the Object Mode
  toolbar (drag it on a surface like the Add Plane tool). It is a helper for an infinite plane:
  its size is only for display, and the line from its center is the normal.
- **Mesh > Slice** (also in the right-click menu) in Edit Mode cuts the selected geometry of
  every object in Edit Mode with the selected Plane empties, or with every visible Plane empty
  when none is selected: **Slice** (add edges), **Split** (two parts), **Remove Above** /
  **Remove Below** (Above is the side the normal points to), and the same with **Fill**. The
  cut edges are selected afterwards, as with Bisect. Mode, Fill and threshold can be changed
  in the Adjust Last Operation panel.

### Camera: Tilt Correction

Keeps lines parallel when the camera is tilted (two-point perspective), like a shift lens or
the tilt correction of 3ds Max's Physical Camera. It is in the camera's Lens panel.

- **Automatic:** corrects the camera's own tilt up or down, so vertical lines stay vertical.
  It follows the camera as it moves or is animated. **Amount** sets how much is corrected.
- **Manual:** corrects by the angles given, **Tilt Vertical** and **Horizontal** (for a wall
  seen from the side), whatever the camera's direction.

The point the camera aims at stays in the middle of the frame. The camera's rotation and its
own Shift are not changed. It works in the camera view, Cycles, EEVEE and Workbench, and the
camera can still be tilted with Fly/Walk navigation or Lock Camera to View.

### Cycles: Map Range and Light Falloff on lights

- **Distance limits as light settings**, instead of a Light Path > Map Range node setup. Point,
  spot and area lights get two Cycles panels, both off by default.
- **Map Range:** up to Min Distance there is no falloff (the light is as bright as at Min
  Distance, as Decay Start in 3ds Max); from there it fades to zero at Max Distance, with the
  interpolation types of the Map Range node. No shadow ray is traced beyond Max Distance.
- **To Min / To Max:** the brightness at Min and at Max Distance (1 and 0 by default). Lower
  To Min, or swap them, to change or reverse the fade.
- **Light Falloff:** the Quadratic, Linear and Constant outputs and Smooth of the Light Falloff
  node.
- **Full Visible Surface** keeps the surface of an area light as seen unchanged by them.

### Cycles: Full Visible Surface for area lights

When an area light is seen directly by the camera (or in sharp mirrors and clear glass), this
option shows its whole surface at the brightness it has without Spread. The light it casts
still uses Spread. It is in the light's Beam Shape panel and under Object > Visibility > Ray
Visibility.

### Frame buffer: interactive render (IPR)

The Image Editor header (also the render window) gets the controls of the V-Ray frame buffer:

- **Interactive Render:** renders the Render Result again whenever something in the render
  changes (objects, materials, lights, the camera, the frame, the resolution or the region),
  with the final render settings, so the result can be saved like any render. Persistent Data
  is on meanwhile and Lock Interface off; both are put back when it stops.
- **Pause** holds the render in progress, changes made meanwhile render on resume. **Stop**
  ends it.
- **Resolution %** menu, **Region** render (draw the region on the image: only it renders, the
  rest keeps the previous render) and a **Render** button.
- No black frame between one render and the next, and the display refreshes as in the viewport.

### Cycles: Light Mix, as V-Ray LightMix

Re-balances the light groups of a view layer without rendering again. It is in View Layer
Properties > Light Mix, in a Light Mix tab of the Image Editor, 3D Viewport and Compositor
sidebars, and in a Light Mix button of the Image Editor header.

- **Scan Lights** builds the light groups and assigns them: linked duplicates and collection
  instances share a group, real copies get their own. Options: grouping by object data, name
  prefix or regex, a group prefix, the World, lights inside collection instances, emissive
  objects (per material, per object or one group), and removing unused groups.
- **Per light group:** On/Off, Solo, Value, Exposure and Tint, and Select Objects. A
  **Master** Value, Exposure and Tint applies to all of them.
- **Live everywhere:** in the rendered viewport, in the frame buffer while rendering, and on a
  finished render: changing the mix afterwards mixes the Render Result again in milliseconds
  (from the Combined pass as rendered, which is kept), and runs the compositor again.
- **Denoise Light Groups** (off by default) denoises every light group pass of a finished
  render once, guided by albedo and normal, so even large changes stay clean.
- **Setups:** save the mix under a name in the scene, load it (light groups are matched by
  name), update or remove it, and export or import it as a `.json` file.
- **Bake Mix into Scene** writes the mix into the lights, World and emissive materials.
  **Reset Mix**, **Clear All Light Groups** and **Quick Render** (low samples and resolution,
  just to have something to mix) are there too.
- **From the Light Mix add-on:** Import Light Mix Add-on Mix takes over its settings and
  removes its compositor nodes. Disable the add-on afterwards.

### Preferences: Scene Selector Position

Interface > Editors > **Scene Selector Position** puts the Scene and View Layer selectors at
the left of the Top Bar, before the menus, for layouts with the Outliner on the left. Right
is the default, as in Blender.

### Build

`BUILD_WINDOWS.bat` builds the Cycles CUDA kernels for RTX 50 series cards (sm_120) when
CUDA 12.8 or newer is installed.

### Fixes

- Vulkan: no crash when snapping with Face Midpoint (the snap cursor's midpoint dot).
- No crash in builds with asserts when a move switches to scale or rotate (G then S or R) while
  an add-on holds the running operator: Blender warns and carries on, as release builds do.

Documentation and tests
-----------------------

- **Design notes:** [`doc/edit_poly_stack/`](../doc/edit_poly_stack/).
- **Build and check scripts (Windows):** [`tools/edit_poly_stack/`](../tools/edit_poly_stack/).
  `BUILD_WINDOWS.bat` builds Blender. `RUN_ALL_CHECKS.bat` runs the unit tests and one check
  per feature in background Blender. `BENCHMARK.bat` times the features on big scenes (meshes
  of up to a million faces, 400 objects with full stacks, Extrude Manifold on up to 200,000
  faces) and appends the results to `benchmark_results.txt`.

Status
------

Experimental. Files saved with the new modifiers only open in this build. Known limits are
listed in the design notes.

---

> [!IMPORTANT]
> Cloning from this [GitHub mirror](https://github.com/blender/blender) may cause Git LFS errors. To avoid this, use `GIT_LFS_SKIP_SMUDGE=1` when doing your initial clone.  
> See [the documentation](https://developer.blender.org/docs/handbook/contributing/using_git/#github-mirror) for full instructions.

Blender
=======

Blender is the free and open source 3D creation suite.
It supports the entirety of the 3D pipeline—modeling, rigging, animation, simulation, rendering, compositing,
motion tracking and video editing.

![Blender screenshot](https://code.blender.org/wp-content/uploads/2018/12/springrg.jpg "Blender screenshot")

Project Pages
-------------

- [Main Website](http://www.blender.org)
- [Reference Manual](https://docs.blender.org/manual/en/latest/index.html)
- [User Community](https://www.blender.org/community/)

Development
-----------

- [Build Instructions](https://developer.blender.org/docs/handbook/building_blender/)
- [Code Review & Bug Tracker](https://projects.blender.org)
- [Developer Forum](https://devtalk.blender.org)
- [Developer Documentation](https://developer.blender.org/docs/)


License
-------

Blender as a whole is licensed under the GNU General Public License, Version 3.
Individual files may have a different but compatible license.

See [blender.org/about/license](https://www.blender.org/about/license) for details.
The changes in this fork are under the same license.
