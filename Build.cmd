@echo off
setlocal
where cl.exe >nul 2>&1
if errorlevel 1 (
    echo Open an x64 Native Tools Command Prompt for Visual Studio and run Build.cmd.
    exit /b 1
)
pushd "%~dp0"
cl /nologo /std:c++20 /O2 /MT /EHsc /W4 /WX /LD /I sdk src\plugin.cpp /link /OUT:CarMeets.dll
set "cml_result=%errorlevel%"
popd
exit /b %cml_result%
