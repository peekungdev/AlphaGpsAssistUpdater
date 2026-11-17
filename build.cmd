@echo off
rem Builds one self-contained exe:  build.cmd <x64|x86|arm64>
rem Output: dist\GpsAssistUpdater-<arch>.exe (static CRT, only Windows system DLLs).
rem Needs Visual Studio 2022 or later with the C++ desktop workload (plus the ARM64 tools for arm64).
setlocal
set ROOT=%~dp0
set ARCH=%1
if /i not "%ARCH%"=="x64" if /i not "%ARCH%"=="x86" if /i not "%ARCH%"=="arm64" (
  echo usage: build.cmd x64^|x86^|arm64
  exit /b 2
)

rem Find Visual Studio
set VS=
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.Tools.ARM64 -property installationPath`) do set VS=%%i
if not defined VS if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set VS=%%i
if not defined VS (echo Visual Studio with C++ tools not found& exit /b 1)

rem Pick the compiler for this host (ARM64 or x64) and the target
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" (set HOST=arm64) else (set HOST=amd64)
if /i "%ARCH%"=="x64" set TARGET=amd64
if /i "%ARCH%"=="x86" set TARGET=x86
if /i "%ARCH%"=="arm64" set TARGET=arm64
if "%HOST%"=="%TARGET%" (set VCVARS=vcvars%HOST:amd64=64%.bat) else (set VCVARS=vcvars%HOST%_%TARGET%.bat)
call "%VS%\VC\Auxiliary\Build\%VCVARS%" >nul || exit /b 1

set OBJ=%ROOT%build\%ARCH%
if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%ROOT%dist" mkdir "%ROOT%dist"
rc /nologo /fo "%OBJ%\app.res" "%ROOT%src\app.rc" || exit /b 1
cl /nologo /c /utf-8 /std:c++17 /O2 /MT /W4 /WX /EHsc /GS /guard:cf /DUNICODE /D_UNICODE ^
   /Fo"%OBJ%\GpsAssistUpdater.obj" "%ROOT%src\GpsAssistUpdater.cpp" || exit /b 1
link /nologo /OUT:"%ROOT%dist\GpsAssistUpdater-%ARCH%.exe" /SUBSYSTEM:WINDOWS /MANIFEST:NO ^
   /OPT:REF /OPT:ICF /DYNAMICBASE /NXCOMPAT /GUARD:CF /Brepro ^
   "%OBJ%\GpsAssistUpdater.obj" "%OBJ%\app.res" ^
   user32.lib gdi32.lib comctl32.lib winhttp.lib bcrypt.lib advapi32.lib || exit /b 1
copy /y "%ROOT%README.txt" "%ROOT%dist\README.txt" >nul
echo built dist\GpsAssistUpdater-%ARCH%.exe
