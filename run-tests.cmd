@echo off
setlocal

set "ROOT=%~dp0"
set "EXE=%ROOT%build\Release\nSkinzCS2.Tests.exe"
set "RUN_ID=%RANDOM%_%RANDOM%"
set "STDOUT_LOG=%ROOT%build\Release\nSkinzCS2.Tests.%RUN_ID%.stdout.log"
set "STDERR_LOG=%ROOT%build\Release\nSkinzCS2.Tests.%RUN_ID%.stderr.log"

if not exist "%EXE%" (
    echo Test binary not found: "%EXE%"
    exit /b 1
)

"%EXE%" > "%STDOUT_LOG%" 2> "%STDERR_LOG%"
set "TEST_EXIT=%ERRORLEVEL%"

if exist "%STDOUT_LOG%" type "%STDOUT_LOG%"
if exist "%STDERR_LOG%" for %%A in ("%STDERR_LOG%") do if %%~zA gtr 0 type "%STDERR_LOG%"

findstr /c:"nSkinzCS2 tests passed" "%STDOUT_LOG%" >nul 2>nul
if not errorlevel 1 (
    echo EXITCODE:0
    exit /b 0
)

echo EXITCODE:%TEST_EXIT%
exit /b %TEST_EXIT%
