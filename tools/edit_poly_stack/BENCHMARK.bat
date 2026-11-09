@echo off
rem Performance benchmark of the Edit Poly stack in big scenes (takes a few minutes).
rem "BENCHMARK.bat quick" runs smaller scenes. Results are added to benchmark_results.txt.
set "QUICK="
if /i "%~1"=="quick" set "QUICK=--quick"
"%~dp0build_windows_x64_vc17_Release\bin\Release\blender.exe" --background --factory-startup --log "mesh.extrude" --log-level debug --python "%~dp0benchmark.py" -- %QUICK%
pause
