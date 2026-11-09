@echo off
rem Run bl_mesh_extrude_manifold_solid.py (in blender\tests\python) inside the built Blender (no window).
"%~dp0build_windows_x64_vc17_Release\bin\Release\blender.exe" --background --factory-startup --python "%~dp0blender\tests\python\bl_mesh_extrude_manifold_solid.py"
pause
