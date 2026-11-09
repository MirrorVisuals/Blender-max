@echo off
rem Run every Edit Poly stack test in the built Blender and show a summary.
rem The tests are in blender\tests\python (the same scripts CTest runs, see RUN_CTEST.bat).
rem Full output of each test is saved in the "check_logs" folder.
setlocal
set "EXE=%~dp0build_windows_x64_vc17_Release\bin\Release\blender.exe"
set "TESTS=%~dp0blender\tests\python"
set "LOGS=%~dp0check_logs"
if not exist "%LOGS%" mkdir "%LOGS%"

echo === Unit tests (stable ID rule) ===
call "%~dp0RUN_TESTS.bat" < nul > "%LOGS%\unit_tests.txt" 2>&1
findstr /c:"PASSED" /c:"FAILED" "%LOGS%\unit_tests.txt"

for %%C in (bl_stable_ids_subdivision bl_modifier_edit_poly bl_modifier_edit_poly_tab bl_modifier_edit_poly_topology bl_modifier_edit_poly_operations bl_modifier_edit_poly_collapse bl_modifier_edit_poly_attributes bl_modifier_edit_poly_selection bl_modifier_mirror_array_stable_ids bl_modifier_uvw_map bl_modifier_unwrap bl_modifier_uv_channels bl_mesh_extrude_manifold_solid bl_cycles_area_light_full_visible_surface bl_cycles_camera_tilt_correction bl_node_edit_poly) do (
  echo.
  echo === %%C ===
  "%EXE%" --background --factory-startup --python "%TESTS%\%%C.py" > "%LOGS%\%%C.txt" 2>&1
  findstr /c:"FAIL " /c:"RESULT" /c:"BLI_assert" /c:"Error" "%LOGS%\%%C.txt"
)

echo.
echo Done. If a test shows no RESULT line, Blender stopped inside it: send me its file from check_logs.
pause
