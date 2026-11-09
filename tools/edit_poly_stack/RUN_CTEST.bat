@echo off
rem Run the Edit Poly stack tests with CTest, the way Blender's own tests run.
rem Needs a build made with BUILD_WINDOWS.bat after the tests were added (that registers them).
setlocal
rem Use the CTest that ships with Visual Studio 2022 if none is on PATH.
where ctest >nul 2>nul || set "PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
cd /d "%~dp0build_windows_x64_vc17_Release"
ctest -C Release --output-on-failure -R "stable_ids_subdivision|modifier_edit_poly|modifier_mirror_array_stable_ids|modifier_uvw_map|modifier_unwrap|modifier_uv_channels|mesh_extrude_manifold_solid|cycles_area_light_full_visible_surface|cycles_camera_tilt_correction|node_edit_poly|node_uvw_map"
pause
