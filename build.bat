@echo off
setlocal EnableExtensions

set "ARCH=%GODOT_ARCH%"
if not defined ARCH set "ARCH=x86_64"

set "JOBS=%GODOT_JOBS%"
if not defined JOBS set "JOBS=%NUMBER_OF_PROCESSORS%"
if not defined JOBS set "JOBS=1"

set "TOOLCHAIN_FLAG="
if /I "%GODOT_USE_MINGW%"=="yes" set "TOOLCHAIN_FLAG=use_mingw=yes"

set "D3D12=%GODOT_D3D12%"
if not defined D3D12 set "D3D12=yes"

set "OPTIMIZE=%GODOT_OPTIMIZE%"
if not defined OPTIMIZE set "OPTIMIZE=speed_trace"

set "SCONS_CMD="

where /Q scons
if %ERRORLEVEL% EQU 0 (
	set "SCONS_CMD=scons"
	goto :build
)

python -c "import sys" >nul 2>nul
if not %ERRORLEVEL% EQU 0 goto :no_python

python -m SCons --version >nul 2>nul
if not %ERRORLEVEL% EQU 0 goto :no_scons

set "SCONS_CMD=python -m SCons"
goto :build

:no_python
echo [ERROR] Python is not available on PATH.
echo         Install Python and retry.
set "EXIT_CODE=1"
goto :finish

:no_scons
echo [ERROR] SCons is not installed for this Python.
echo         Run: python -m pip install --user scons
set "EXIT_CODE=1"
goto :finish

:build
echo [Godot] Building Windows editor
echo [Godot] arch=%ARCH% jobs=%JOBS%
echo [Godot] d3d12=%D3D12%
echo [Godot] optimize=%OPTIMIZE%
if defined TOOLCHAIN_FLAG echo [Godot] toolchain=%TOOLCHAIN_FLAG%

%SCONS_CMD% platform=windows target=editor arch=%ARCH% dev_build=yes optimize=%OPTIMIZE% debug_symbols=yes angle=no accesskit=no disable_xr=yes d3d12=%D3D12% %TOOLCHAIN_FLAG% -j%JOBS% %*
set "EXIT_CODE=%ERRORLEVEL%"

if not "%EXIT_CODE%"=="0" echo [Godot] Build failed with exit code %EXIT_CODE%.

:finish
if /I "%0"=="%~f0" pause
exit /b %EXIT_CODE%
