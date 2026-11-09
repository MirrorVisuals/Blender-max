@echo off
rem Edit Poly node in typical Geometry Nodes setups: what happens to an edit when something earlier
rem in the node tree changes, plus timings on a large mesh. The report is also saved in
rem check_logs\benchmark_node_edit_poly.txt.
setlocal
set "LOGS=%~dp0check_logs"
if not exist "%LOGS%" mkdir "%LOGS%"
"%~dp0build_windows_x64_vc17_Release\bin\Release\blender.exe" --background --factory-startup --python "%~dp0blender\tools\edit_poly_stack\benchmark_node_edit_poly.py" > "%LOGS%\benchmark_node_edit_poly.txt" 2>&1
type "%LOGS%\benchmark_node_edit_poly.txt"
pause
