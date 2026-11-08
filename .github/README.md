<!--
Keep this document short & concise,
linking to external resources instead of including content in-line.
See 'release/text/readme.html' for the end user read-me.
-->

Blender-max
===========

An experimental fork of **Blender 5.2.3 LTS** that brings a 3ds Max style modifier workflow to
Blender: editable modeling levels in the modifier stack, UVW Map and Unwrap modifiers, and a
few rendering extras. Branch: `edit-poly-stack`. Experimental and unofficial: not affiliated
with the Blender Foundation.

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
  nearest Edit Poly below the active modifier, or adds one at the top of the stack. While you
  edit, the modifiers above stay visible. Tab again stores the edits in the modifier, and the
  base mesh is never changed.
- **Any modeling tool can be used.** Vertex moves are stored as moves. Topology changes
  (delete, extrude, inset, bevel and so on) are stored as a patch anchored to the mesh below.
- **Re-runnable operations.** Extrude, inset and bevel are also logged. If the mesh below
  changes (for example a Subdivision level), they are re-applied to the matching faces.
- **Several levels.** You can stack several Edit Poly levels and edit any of them. Click a level
  (or hover its panel) and press Tab.
- **Selection passing.** The faces selected when you leave a level are passed up the stack,
  as with Edit Poly in 3ds Max (see UVW Map: Selected Faces Only).
- **Collapse to Here** (modifier menu) bakes everything up to a modifier into the mesh while
  keeping the stable IDs.

### UVW Map modifier

- **Projections:** Planar, Box, Cylindrical (with optional Cap), Spherical, Shrink Wrap and Face.
- **Map controls:** X / Y / Z alignment, tile, offset and flip.
- **Real-World Size:** sets the map size from pixel density (px/m) and texture size.
- **Selected Faces Only:** maps only the faces passed up by the Edit Poly level below.
- **Gizmo:** drawn in the viewport. Tab (or Edit Gizmo) lets you move, rotate and scale it,
  with snapping. Fit, Center and View Align are available.

### Unwrap modifier

- **Stored UVs:** Tab opens a UV session in the UV Editor, where all of Blender's UV tools work.
  Tab again stores the UVs by stable ID.
- **Follows the stack:** the stored UVs follow the mesh below. Subdivision level up or down,
  Mirror and Array copies all keep their UVs.
- **New faces:** faces added below are either auto-unwrapped and attached to the stored UVs
  around them, or keep the UVs that arrive from below. Either way they are flagged and
  pre-selected in the next session.
- **Quick buttons:** Smart Project, Unwrap and Pack, without a session.

### Cycles: Full Visible Surface for area lights

When an area light is seen directly by the camera (or in sharp mirrors and clear glass), this
option shows its whole surface at the brightness it has without Spread. The light it casts
still uses Spread. It is in the light's Beam Shape panel and under Object > Visibility > Ray
Visibility.

### Build

`BUILD_WINDOWS.bat` builds the Cycles CUDA kernels for RTX 50 series cards (sm_120) when
CUDA 12.8 or newer is installed.

Documentation and tests
-----------------------

- **Design notes:** [`doc/edit_poly_stack/`](../doc/edit_poly_stack/).
- **Build and check scripts (Windows):** [`tools/edit_poly_stack/`](../tools/edit_poly_stack/).
  `BUILD_WINDOWS.bat` builds Blender. `RUN_ALL_CHECKS.bat` runs the unit tests and one check
  per feature in background Blender.

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
