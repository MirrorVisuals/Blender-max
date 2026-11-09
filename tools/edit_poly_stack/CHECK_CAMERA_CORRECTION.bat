@echo off
rem Run bl_cycles_camera_tilt_correction.py (in blender\tests\python) inside the built Blender (no window).
"%~dp0build_windows_x64_vc17_Release\bin\Release\blender.exe" --background --factory-startup --python "%~dp0blender\tests\python\bl_cycles_camera_tilt_correction.py"
pause
