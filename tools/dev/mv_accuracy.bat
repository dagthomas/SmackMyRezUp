@echo off
REM Builds and runs the motion-vector accuracy check (tools\dev\mv_accuracy.cpp).
REM Needs only the compiler: TemporalGuides.cpp has no D3D or NGX dependency.
setlocal
set VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" set VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
    echo Could not find vcvars64.bat - run this from a VS x64 developer prompt instead.
    exit /b 1
)
call "%VCVARS%" >nul 2>&1
pushd "%~dp0"
cl /nologo /std:c++20 /O2 /EHsc /I "..\..\src" mv_accuracy.cpp "..\..\src\TemporalGuides.cpp" ^
   /Fe:mv_accuracy.exe > mv_accuracy_build.log 2>&1
if errorlevel 1 (
    type mv_accuracy_build.log
    popd
    exit /b 1
)
"%~dp0mv_accuracy.exe"
popd
