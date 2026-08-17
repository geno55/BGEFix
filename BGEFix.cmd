@echo off
setlocal
rem ---------------------------------------------------------------------------
rem BGEFix - double-click this.
rem
rem It opens the installer, which asks which of the three fixes you want, shows
rem what is already installed, and can also report the current state or remove
rem everything again. There is nothing to type and no options to learn.
rem
rem Anything you pass here goes straight through to Fix-BGE.ps1, and passing
rem anything skips the menu - the script then does exactly what you asked for:
rem
rem     BGEFix.cmd -Status
rem     BGEFix.cmd -Component All -Force
rem     BGEFix.cmd -Revert
rem
rem See README.md for the full list.
rem ---------------------------------------------------------------------------

rem Windows tags every file extracted from a downloaded zip with a mark-of-the-web
rem stream. -ExecutionPolicy Bypass is enough to run the script itself, but the
rem stream also rides along on the DLLs when they are copied into the game folder,
rem so clear it here rather than installing game files Windows treats as downloads.
set "BGEFIX_HOME=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-ChildItem -LiteralPath $env:BGEFIX_HOME -Recurse -File | Unblock-File" >nul 2>&1

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0Fix-BGE.ps1" %*
set "RC=%ERRORLEVEL%"

rem The script elevates itself, and the elevated window holds itself open so the
rem summary stays readable. Pause here only on failure: a failure BEFORE elevation
rem prints in this window, which would otherwise close and take the message with it.
if not "%RC%"=="0" (
    echo.
    echo   BGEFix did not finish - exit code %RC%.
    echo   Nothing is left half-applied. Run this file again, or choose U to remove
    echo   everything and start over.
    echo.
    pause
)
exit /b %RC%
