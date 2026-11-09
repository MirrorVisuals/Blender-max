@echo off
rem Run check_camera_correction.py inside the built Blender (no window).
"%~dp0build_windows_x64_vc17_Release\bin\Release\blender.exe" --background --factory-startup --python "%~dp0check_camera_correction.py"
pause
