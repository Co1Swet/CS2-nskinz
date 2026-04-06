@echo off
setlocal

set "ROOT=%~dp0"
set "SOLUTION=%ROOT%nSkinzCS2.sln"
set "CONFIGURATION=Release"
set "PLATFORM=x64"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "MSBUILD="
set "RUN_ID=%RANDOM%_%RANDOM%"
set "BUILD_STDOUT_LOG=%ROOT%build\Release\build.%RUN_ID%.stdout.log"
set "BUILD_STDERR_LOG=%ROOT%build\Release\build.%RUN_ID%.stderr.log"

if not exist "%SOLUTION%" (
    echo Solution not found: "%SOLUTION%"
    exit /b 1
)

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
        set "MSBUILD=%%I"
        goto found_msbuild
    )
)

if exist "D:\Visual studio 2022\MSBuild\Current\Bin\MSBuild.exe" (
    set "MSBUILD=D:\Visual studio 2022\MSBuild\Current\Bin\MSBuild.exe"
    goto found_msbuild
)

echo MSBuild.exe was not found. Install Visual Studio Build Tools or update this script.
exit /b 1

:found_msbuild
echo Using MSBuild: "%MSBUILD%"
echo Building "%SOLUTION%" with Configuration=%CONFIGURATION% Platform=%PLATFORM%
echo BUILD_STDOUT_LOG:%BUILD_STDOUT_LOG%
echo BUILD_STDERR_LOG:%BUILD_STDERR_LOG%

"%MSBUILD%" "%SOLUTION%" /p:Configuration=%CONFIGURATION% /p:Platform=%PLATFORM% /m:1 /nologo > "%BUILD_STDOUT_LOG%" 2> "%BUILD_STDERR_LOG%"
set "BUILD_EXIT=%ERRORLEVEL%"
echo BUILD_EXIT:%BUILD_EXIT%
if not "%BUILD_EXIT%"=="0" (
    echo Build failed with exit code %BUILD_EXIT%.
    exit /b %BUILD_EXIT%
)

echo Build succeeded. Running tests...
call "%ROOT%run-tests.cmd"
set "TEST_EXIT=%ERRORLEVEL%"
if not "%TEST_EXIT%"=="0" (
    echo Tests failed with exit code %TEST_EXIT%.
    exit /b %TEST_EXIT%
)

echo Build and tests completed successfully.
exit /b 0
