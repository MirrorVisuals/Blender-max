@echo off
rem Build the Edit Poly fork (Blender 5.2.3 + edit-poly-stack) and run the stable-ID tests.
rem Needs: Visual Studio 2022 with "Desktop development with C++", Git for Windows, Git LFS.
setlocal
cd /d "%~dp0blender" || goto :fail

where git >nul 2>nul || (echo Git for Windows is not installed: https://git-scm.com/download/win & goto :fail)
git lfs version >nul 2>nul || (echo Git LFS is not installed: run "git lfs install" or get it from https://git-lfs.com & goto :fail)

echo.
echo === 1/3  Download the prebuilt libraries (several GB, first time only) ===
rem Fetch only lib\windows_x64; your edit-poly-stack branch is not touched.
git lfs install --skip-repo || goto :fail
git config --local "submodule.lib/windows_x64.update" checkout || goto :fail
git submodule update --progress --init lib/windows_x64 || goto :fail
rem Large binary source files (startup.blend, icons, fonts) live in Git LFS on projects.blender.org.
rem Test data is skipped (several GB, not needed to build or run the stable-ID tests).
git config lfs.url https://projects.blender.org/blender/blender.git/info/lfs || goto :fail
git lfs pull --exclude="tests/**" || goto :fail

echo.
echo === 2/3  Build Blender with tests (developer config; first build takes a long time) ===
rem Use the CMake that ships with Visual Studio 2022 if none is on PATH.
where cmake >nul 2>nul || set "PATH=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%"
where cmake >nul 2>nul || (echo CMake not found. Install it from https://cmake.org/download/ and tick "Add CMake to the PATH". & goto :fail)
cmake --version | findstr /b "cmake"
rem GPU rendering: build the Cycles CUDA kernels for the RTX 5090 (sm_120, needs CUDA 12.8+),
rem so Cycles does not try to compile them when you render (that needs cl.exe and crashes).
set "BUILD_DIR=%~dp0build_windows_x64_vc17_Release"
set "CUDA_OK="
where nvcc >nul 2>nul && (nvcc --version | findstr /c:"release 12.8" /c:"release 12.9" /c:"release 13." >nul && set "CUDA_OK=1")
if defined CUDA_OK (
  echo CUDA 12.8+ found: GPU kernels for sm_120 will be built.
  if exist "%BUILD_DIR%\CMakeCache.txt" cmake -S . -B "%BUILD_DIR%" -DWITH_CYCLES_CUDA_BINARIES=ON -DCYCLES_CUDA_BINARIES_ARCH=sm_120 >nul || goto :fail
) else (
  echo NOTE: CUDA Toolkit 12.8 or newer not found - GPU kernels are not built.
  echo       GPU rendering needs it: https://developer.nvidia.com/cuda-downloads
)
call make.bat developer 2022 || goto :fail

echo.
echo === 3/3  Run the stable-ID tests ===
call "%~dp0RUN_TESTS.bat" || goto :fail

echo.
echo Done. Blender: %~dp0build_windows_x64_vc17_Release\bin\Release\blender.exe
pause
exit /b 0

:fail
echo.
echo Build stopped with an error. Copy the last lines above and send them to Claude.
pause
exit /b 1
