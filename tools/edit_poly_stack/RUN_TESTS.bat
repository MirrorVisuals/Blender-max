@echo off
rem Run only the stable-ID tests (no rebuild). Same DLL search path as Blender's own CTest setup.
setlocal
set "BUILD=%~dp0build_windows_x64_vc17_Release"
set "LIB=%~dp0blender\lib\windows_x64"
set "PATH=%BUILD%\bin\Release\blender.shared;%BUILD%\bin\Release;%LIB%\epoxy\bin;%LIB%\tbb\bin;%LIB%\OpenImageIO\bin;%LIB%\boost\lib;%LIB%\openexr\bin;%LIB%\imath\bin;%LIB%\shaderc\bin;%LIB%\opencolorio\bin;%LIB%\aom\bin;%LIB%\openjph\bin;%LIB%\ceres\bin;%PATH%"
"%BUILD%\bin\tests\Release\blender_test.exe" --gtest_filter=stable_id.* --test-release-dir "%BUILD%\bin\Release\5.2"
echo.
echo Exit code: %ERRORLEVEL%
pause
