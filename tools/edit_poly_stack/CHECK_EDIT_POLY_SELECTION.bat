@echo off
rem Run check_edit_poly_selection.py inside the built Blender (no window).
"%~dp0build_windows_x64_vc17_Release\bin\Release\blender.exe" --background --factory-startup --python "%~dp0check_edit_poly_selection.py"
pause
