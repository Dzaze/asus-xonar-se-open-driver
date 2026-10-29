<#
.SYNOPSIS
    Exécute les tests unitaires du projet via ctest.

.DESCRIPTION
    Suppose que le projet a déjà été configuré et compilé avec scripts/build.ps1
    (ou cmake --preset msvc-ninja-<config>). N'a pas besoin de l'environnement vcvars.

.PARAMETER Config
    Debug (défaut) ou Release.

.EXAMPLE
    .\scripts\run-tests.ps1
    .\scripts\run-tests.ps1 -Config Release
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$preset = "msvc-ninja-$($Config.ToLowerInvariant())"
$buildDir = Join-Path $repoRoot "build\$preset"

if (-not (Test-Path (Join-Path $buildDir 'CMakeCache.txt'))) {
    Write-Host "ERREUR : le dossier de build $buildDir n'existe pas. Lancez d'abord .\scripts\build.ps1 -Config $Config" -ForegroundColor Red
    exit 1
}
if (-not (Get-Command ctest -ErrorAction SilentlyContinue)) {
    Write-Host "ERREUR : ctest introuvable dans le PATH (fourni avec CMake)." -ForegroundColor Red
    exit 1
}

Push-Location $repoRoot
try {
    Write-Host "==> ctest --preset $preset" -ForegroundColor Cyan
    ctest --preset $preset --output-on-failure
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        Write-Host "Des tests ont échoué (code $code)." -ForegroundColor Red
        exit $code
    }
    Write-Host "Tous les tests passent." -ForegroundColor Green
}
finally {
    Pop-Location
}
