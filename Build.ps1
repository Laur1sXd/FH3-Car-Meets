param(
    [string]$ToolchainBin = "",
    [string]$OutDir = "$PSScriptRoot\build",
    [switch]$SmokeTest
)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($ToolchainBin)) {
    $compiler = (Get-Command x86_64-w64-mingw32-clang++.exe -ErrorAction Stop).Source
} else {
    $compiler = Join-Path $ToolchainBin 'x86_64-w64-mingw32-clang++.exe'
}
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$common = @('-std=c++20','-O2','-Wall','-Wextra','-Werror','-static',"-I$PSScriptRoot\sdk")
& $compiler @common -shared "$PSScriptRoot\src\plugin.cpp" -o "$OutDir\CarMeets.dll"
if ($LASTEXITCODE) { throw 'CarMeets build failed.' }
if ($SmokeTest) {
    & $compiler @common -municode "$PSScriptRoot\tests\smoke.cpp" -o "$OutDir\smoke.exe"
    if ($LASTEXITCODE) { throw 'Smoke harness build failed.' }
    $scratch = Join-Path $OutDir ('smoke-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $scratch | Out-Null
    & "$OutDir\smoke.exe" "$OutDir\CarMeets.dll" $scratch
    if ($LASTEXITCODE) { throw 'Standalone DLL smoke test failed.' }
}
