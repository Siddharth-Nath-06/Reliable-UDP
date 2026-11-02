param(
    [string]$BuildCommand = "",
    [string]$TestCommand = "",
    [switch]$SkipBuild,
    [switch]$SkipTests
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $Root

function Invoke-CheckedCommand([string]$Label, [string]$Command) {
    Write-Host "`n=== $Label ===" -ForegroundColor Cyan
    Write-Host "> $Command"
    # Run through PowerShell so commands may include arguments.
    & $env:ComSpec /d /s /c $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed with exit code $LASTEXITCODE. Experiments have been blocked."
    }
}

if (-not $SkipBuild) {
    if ([string]::IsNullOrWhiteSpace($BuildCommand)) {
        if (Test-Path "CMakeLists.txt") {
            if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw "CMakeLists.txt exists but cmake is not on PATH." }
            if (-not (Test-Path "build")) { New-Item -ItemType Directory -Path "build" | Out-Null }
            Invoke-CheckedCommand "Configure CMake" "cmake -S . -B build"
            Invoke-CheckedCommand "Build" "cmake --build build --config Release"
        } elseif (Test-Path "Makefile") {
            if (-not (Get-Command make -ErrorAction SilentlyContinue)) {
                if (Get-Command mingw32-make -ErrorAction SilentlyContinue) { $make = "mingw32-make" }
                else { throw "Makefile exists, but neither make nor mingw32-make is on PATH." }
            } else { $make = "make" }
            Invoke-CheckedCommand "Build" "$make"
        } else {
            throw "No CMakeLists.txt or Makefile found. Pass -BuildCommand with your exact build command; prechecker will not guess how to link the project."
        }
    } else {
        Invoke-CheckedCommand "Build" $BuildCommand
    }
} else {
    Write-Host "Build skipped by request." -ForegroundColor Yellow
}

if (-not $SkipTests) {
    if (-not [string]::IsNullOrWhiteSpace($TestCommand)) {
        Invoke-CheckedCommand "Tests" $TestCommand
    } elseif (Test-Path "CTestTestfile.cmake") {
        Invoke-CheckedCommand "Tests" "ctest --test-dir build --output-on-failure -C Release"
    } elseif (Test-Path "Makefile") {
        # Only run the conventional target if the Makefile advertises it.
        $makefile = Get-Content "Makefile" -Raw
        if ($makefile -match '(?m)^test\s*:') { Invoke-CheckedCommand "Tests" "make test" }
        elseif ($makefile -match '(?m)^check\s*:') { Invoke-CheckedCommand "Tests" "make check" }
        else { throw "No test/check target detected. Pass -TestCommand with the project's actual test command." }
    } else {
        throw "Could not detect a test runner. Pass -TestCommand with the project's actual test command."
    }
} else {
    Write-Host "Tests skipped by request." -ForegroundColor Yellow
}

Write-Host "`nPRECHECK PASSED: build and requested tests succeeded." -ForegroundColor Green
