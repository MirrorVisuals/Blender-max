@echo off
rem Run every Edit Poly check in the built Blender and show a summary.
rem Full output of each check is saved in the "check_logs" folder.
setlocal
set "EXE=%~dp0build_windows_x64_vc17_Release\bin\Release\blender.exe"
set "LOGS=%~dp0check_logs"
if not exist "%LOGS%" mkdir "%LOGS%"

echo === Unit tests (stable ID rule) ===
call "%~dp0RUN_TESTS.bat" < nul > "%LOGS%\unit_tests.txt" 2>&1
findstr /c:"PASSED" /c:"FAILED" "%LOGS%\unit_tests.txt"

for %%C in (check_stable_ids check_edit_poly check_edit_poly_tab check_edit_poly_topology check_edit_poly_ops check_edit_poly_collapse check_edit_poly_attrs check_edit_poly_selection check_mirror_array check_uvw_map check_unwrap check_light_surface) do (
  echo.
  echo === %%C ===
  "%EXE%" --background --factory-startup --python "%~dp0%%C.py" > "%LOGS%\%%C.txt" 2>&1
  findstr /c:"FAIL " /c:"RESULT" /c:"BLI_assert" /c:"Error" "%LOGS%\%%C.txt"
)

echo.
echo Done. If a check shows no RESULT line, Blender stopped inside it: send me its file from check_logs.
pause
