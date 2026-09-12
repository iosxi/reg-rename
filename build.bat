@echo off
rem ---------------------------------------------------------------
rem Windows Device Name Manager - MSVC build driver
rem
rem   build.bat            release build
rem   build.bat uitest     UI check build (no UAC)
rem   build.bat test       non-GUI smoke test
rem   build.bat clean
rem
rem Finds and calls vcvars64.bat automatically, so this works from a
rem plain command prompt.
rem
rem Two cmd.exe pitfalls this file deliberately avoids:
rem
rem  1) ASCII only. cmd parses batch files in the OEM code page (932 on
rem     Japanese Windows), so UTF-8 Japanese text here breaks parsing.
rem     Japanese comments belong in Makefile.msvc and the C sources.
rem
rem  2) No parenthesised if-blocks around paths. Expanding a variable
rem     whose value contains "(x86)" inside "if ... ( ... )" closes the
rem     block early ("\Microsoft was unexpected at this time"). Labels
rem     and goto are used instead.
rem ---------------------------------------------------------------
setlocal

if defined VCINSTALLDIR goto :build

set "VSINSTALLER=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%VSINSTALLER%\vswhere.exe" goto :err_vswhere

rem pushd into the installer directory so vswhere can be invoked unquoted.
rem Quoting an exe path inside a for /f backtick command is fragile when
rem the path contains spaces and parentheses.
set "VSPATH="
pushd "%VSINSTALLER%"
for /f "tokens=*" %%i in ('.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath') do set "VSPATH=%%i"
if not defined VSPATH for /f "tokens=*" %%i in ('.\vswhere.exe -latest -products * -property installationPath') do set "VSPATH=%%i"
popd

if defined VSPATH goto :got_vs
goto :err_novs

:got_vs
set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" goto :err_vcvars
rem stderr is redirected too: vcvars64.bat itself prints
rem "'vswhere.exe' is not recognized as an internal or external command"
rem on stderr on this machine. That comes from Microsoft's script, not
rem from here, and it does not stop vcvars from succeeding. Failures are
rem caught by the errorlevel check below instead.
call "%VCVARS%" >nul 2>&1
if errorlevel 1 goto :err_vcvars_run
goto :build

:build
cd /d "%~dp0"
nmake /nologo /f Makefile.msvc %*
exit /b %ERRORLEVEL%

:err_vswhere
echo [error] vswhere.exe not found. Install Visual Studio Build Tools.
exit /b 1

:err_novs
echo [error] No Visual Studio installation found.
exit /b 1

:err_vcvars
echo [error] vcvars64.bat not found under "%VSPATH%".
exit /b 1

:err_vcvars_run
echo [error] vcvars64.bat failed.
exit /b 1
