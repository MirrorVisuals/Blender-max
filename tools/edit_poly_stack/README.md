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
- `RUN_ALL_CHECKS.bat`: unit tests plus every `check_*.py` in background Blender; full logs go to
  `check_logs\`.
- `CHECK_*.bat` / `check_*.py`: single checks (stable IDs, Edit Poly, Tab sessions, topology,
  operations, Collapse to Here, Mirror / Array, UVW Map, Unwrap, Cycles light surface).

Feature documentation: `doc/edit_poly_stack/`.
