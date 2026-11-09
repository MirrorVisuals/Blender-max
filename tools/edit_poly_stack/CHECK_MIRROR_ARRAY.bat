@echo off
rem Run bl_modifier_mirror_array_stable_ids.py (in blender\tests\python) inside the built Blender (no window).
"%~dp0build_windows_x64_vc17_Release\bin\Release\blender.exe" --background --factory-startup --python "%~dp0blender\tests\python\bl_modifier_mirror_array_stable_ids.py"
pause
