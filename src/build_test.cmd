@echo off
setlocal
rem Builds and runs the functional test against the DLL in ..\dist.

set OUTDIR=%~dp0..\dist

where cl.exe >nul 2>&1
if %ERRORLEVEL%==0 goto :have_cl
set "VSDIR=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%VSDIR%\vswhere.exe" ( echo ERROR: vswhere.exe not found & exit /b 1 )
for /f "usebackq delims=" %%i in (`"%VSDIR%\vswhere.exe" -latest -products * -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH ( echo ERROR: Visual Studio not found & exit /b 1 )
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 exit /b 1

:have_cl
pushd "%OUTDIR%"
cl /nologo /MT /O1 /W4 /DNDEBUG /Fe:test_proxy.exe "%~dp0test_proxy.cpp" ^
   /link /MACHINE:X86 kernel32.lib user32.lib
set RC=%ERRORLEVEL%
del /q *.obj 2>nul
if not "%RC%"=="0" ( echo BUILD FAILED & popd & exit /b %RC% )

echo.
echo Running functional test...
echo.
rem Absolute path: cmd will not resolve an exe from the working directory when
rem NoDefaultCurrentDirectoryInExePath is set.
"%OUTDIR%\test_proxy.exe"
set RC=%ERRORLEVEL%
popd
exit /b %RC%
