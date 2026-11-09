# Edit Poly stack: build and check scripts

Scripts used to build this branch on Windows and to check it.

Put them **next to** the `blender` source folder (not inside it), e.g.

```
D:\Blender_Experimental\
    blender\            <- this repository, branch edit-poly-stack
    BUILD_WINDOWS.bat   <- copied from blender\tools\edit_poly_stack
    RUN_ALL_CHECKS.bat
    ...
```

- `BUILD_WINDOWS.bat`: downloads the prebuilt libraries, builds Blender (developer config, Visual
  Studio 2022) with Cycles CUDA kernels for sm_120 (RTX 50 series) if CUDA 12.8+ is installed,
  then runs the unit tests.
- `RUN_ALL_CHECKS.bat`: unit tests plus every test in `tests\python` that belongs to this
  branch (`bl_stable_ids_subdivision.py`, `bl_modifier_*.py`, `bl_mesh_extrude_manifold_solid.py`,
  `bl_cycles_area_light_full_visible_surface.py`, `bl_cycles_camera_tilt_correction.py`) in
  background Blender; full logs go to `check_logs\`.
- `CHECK_*.bat`: run one of those tests (stable IDs, Edit Poly, Tab sessions, topology,
  operations, Collapse to Here, Mirror / Array, UVW Map, Unwrap, UV channels, Extrude Manifold
  solid, Cycles light surface, camera tilt correction).
- `RUN_CTEST.bat`: runs the same tests through CTest, as registered in
  `tests/python/CMakeLists.txt`.

Feature documentation: `doc/edit_poly_stack/`.
