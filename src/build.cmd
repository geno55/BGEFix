@echo off
setlocal enabledelayedexpansion
rem ---------------------------------------------------------------------------
rem Builds d3d9.dll (32-bit) from d3d9_windowed.cpp.
rem
rem The output MUST be 32-bit: BGE.exe is a 32-bit process and will not load a
rem 64-bit DLL. No DirectX headers are needed - only windows.h and kernel32/user32.
rem ---------------------------------------------------------------------------

set OUTDIR=%~dp0..\dist
set SRC=%~dp0d3d9_windowed.cpp
set DEF=%~dp0d3d9_windowed.def

rem --- locate the x86 MSVC environment -------------------------------------
where cl.exe >nul 2>&1
if %ERRORLEVEL%==0 goto :have_cl

rem Quoted set form: %ProgramFiles(x86)% contains parentheses that otherwise
rem terminate an enclosing block early.
set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%VSDIR%\vswhere.exe" (
    echo ERROR: vswhere.exe not found. Install Visual Studio Build Tools with the
    echo        "Desktop development with C++" workload, or run this from a
    echo        "x86 Native Tools Command Prompt".
    exit /b 1
)

rem Use the fully quoted path: some environments set NoDefaultCurrentDirectoryInExePath,
rem which stops cmd finding vswhere.exe even when it is the working directory.
for /f "usebackq delims=" %%i in (`"%VSDIR%\vswhere.exe" -latest -products * -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
    echo ERROR: no Visual Studio installation found.
    exit /b 1
)
if not exist "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo ERROR: vcvarsall.bat missing under "%VSPATH%".
    exit /b 1
)

echo Configuring x86 toolchain from "%VSPATH%"
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 (
    echo ERROR: vcvarsall x86 failed. Is the x86 toolset installed?
    exit /b 1
)

:have_cl
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

rem --- confirm we are really targeting x86 ----------------------------------
cl 2>&1 | findstr /i "x86" >nul
if errorlevel 1 (
    echo ERROR: the active cl.exe is not targeting x86. Use the x86 Native Tools prompt.
    exit /b 1
)

pushd "%OUTDIR%"

rem /MT links the CRT statically so the DLLs have no redistributable dependency.
echo Building d3d9.dll ...
cl /nologo /LD /MT /O1 /W4 /WX /DNDEBUG ^
   /Fe:d3d9.dll "%SRC%" ^
   /link /DEF:"%DEF%" /MACHINE:X86 kernel32.lib user32.lib
set RC=%ERRORLEVEL%
if not "%RC%"=="0" goto :done

echo Building dinput8.dll ...
cl /nologo /LD /MT /O1 /W4 /WX /DNDEBUG ^
   /Fe:dinput8.dll "%~dp0dinput8_xinput.cpp" ^
   /link /DEF:"%~dp0dinput8_xinput.def" /MACHINE:X86 kernel32.lib user32.lib
set RC=%ERRORLEVEL%

:done
popd

del /q "%OUTDIR%\*.obj" 2>nul
del /q "%OUTDIR%\*.exp" "%OUTDIR%\*.lib" 2>nul

if not "%RC%"=="0" (
    echo BUILD FAILED ^(exit %RC%^)
    exit /b %RC%
)

echo.
echo Built: %OUTDIR%\d3d9.dll
echo Built: %OUTDIR%\dinput8.dll
exit /b 0
