echo on
setlocal EnableExtensions

set "ARCH=%GODOT_ARCH%"
if not defined ARCH set "ARCH=x86_64"

set "JOBS=%GODOT_JOBS%"
if not defined JOBS set "JOBS=%NUMBER_OF_PROCESSORS%"
if not defined JOBS set "JOBS=1"

set "SCONS_CMD="

where /Q scons
if not errorlevel 1 (
	scons --version >nul 2>nul
	if not errorlevel 1 set "SCONS_CMD=scons"
)

if not defined SCONS_CMD (
	where /Q python
	if not errorlevel 1 (
		python -c "import sys" >nul 2>nul
		if not errorlevel 1 (
			python -m SCons --version >nul 2>nul
			if not errorlevel 1 set "SCONS_CMD=python -m SCons"
		)
	)
)

if not defined SCONS_CMD (
	where /Q py
	if not errorlevel 1 (
		py -3 -c "import sys" >nul 2>nul
		if not errorlevel 1 (
			py -3 -m SCons --version >nul 2>nul
			if not errorlevel 1 set "SCONS_CMD=py -3 -m SCons"
		)
	)
)

if not defined SCONS_CMD (
	echo [ERROR] No working Python + SCons runtime found.
	echo         Install Python 3 and SCons, then re-run.
	echo         Example:
	echo           winget install -e --id Python.Python.3.12
	echo           py -3 -m pip install --user scons
	echo         If Python aliases point to Microsoft Store stubs:
	echo           disable "python.exe/python3.exe" App execution aliases in Windows Settings.
	exit /b 1
)

set "OPTIMIZE=%GODOT_OPTIMIZE%"
if not defined OPTIMIZE set "OPTIMIZE=speed_trace"

set "TOOLCHAIN_FLAG="
if /I "%GODOT_USE_MINGW%"=="yes" set "TOOLCHAIN_FLAG=use_mingw=yes"

echo [Godot] Building Windows editor
echo [Godot] arch=%ARCH% jobs=%JOBS%
echo [Godot] optimize=%OPTIMIZE%
if defined TOOLCHAIN_FLAG echo [Godot] toolchain=%TOOLCHAIN_FLAG%

%SCONS_CMD% platform=windows target=editor arch=%ARCH% dev_build=yes optimize=%OPTIMIZE% debug_symbols=yes angle=no accesskit=no %TOOLCHAIN_FLAG% -j%JOBS% %*
set "EXIT_CODE=%ERRORLEVEL%"

if not "%EXIT_CODE%"=="0" (
	echo [Godot] Build failed with exit code %EXIT_CODE%.
)

exit /b %EXIT_CODE%
