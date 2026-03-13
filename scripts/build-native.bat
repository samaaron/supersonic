@echo off
setlocal enabledelayedexpansion

:: Build script for SuperSonic native (JUCE) backend
:: Uses CMake to build the SuperSonicJuce executable

set SCRIPT_DIR=%~dp0
set PROJECT_ROOT=%SCRIPT_DIR%..
set BUILD_DIR=%PROJECT_ROOT%\build\native

:: Defaults
set BUILD_TYPE=Release
set BUILD_TESTS=OFF
set CLEAN=false
set JOBS=

:: Parse arguments
:parse_args
if "%~1"=="" goto done_args
if /i "%~1"=="--debug" (set BUILD_TYPE=Debug & shift & goto parse_args)
if /i "%~1"=="--tests" (set BUILD_TESTS=ON & shift & goto parse_args)
if /i "%~1"=="--clean" (set CLEAN=true & shift & goto parse_args)
if /i "%~1"=="--help" goto show_help
if /i "%~1"=="-h" goto show_help
echo Unknown option: %~1
exit /b 1

:show_help
echo Usage: %~nx0 [options]
echo   --debug     Build in Debug mode (default: Release)
echo   --tests     Build native test suite
echo   --clean     Remove build dir and reconfigure
exit /b 0

:done_args

echo Building SuperSonic native (%BUILD_TYPE%)

if "%CLEAN%"=="true" (
    if exist "%BUILD_DIR%" (
        echo Cleaning build directory...
        rmdir /s /q "%BUILD_DIR%"
    )
)

:: Configure if needed
if not exist "%BUILD_DIR%\CMakeCache.txt" (
    echo Configuring CMake...
    cmake -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DBUILD_TESTS=%BUILD_TESTS% "%PROJECT_ROOT%"
    if errorlevel 1 exit /b 1
)

:: Build
cmake --build "%BUILD_DIR%" --config %BUILD_TYPE%
if errorlevel 1 exit /b 1

echo.
echo Build complete. Check %BUILD_DIR% for output.
