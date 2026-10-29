<#
.SYNOPSIS
    Compile le pilote noyau xonarse.sys avec le WDK installé (sans MSBuild).
.DESCRIPTION
    Charge l'environnement vcvars64 des Build Tools, configure CMake avec
    XONAR_BUILD_DRIVER=ON dans build/driver-<Config> et assemble le paquet non signé
    dans build/driver-<Config>/driver-package. Rien n'est installé sur la machine.
.PARAMETER Config
    Debug (défaut) ou Release.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')] [string] $Config = 'Debug'
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$buildDir = Join-Path $root "build\driver-$($Config.ToLower())"

Write-Host "==> Recherche de Visual Studio (vswhere)"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere introuvable : installez Visual Studio Build Tools." }
$vsPath = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "Aucune installation Visual Studio / Build Tools trouvée." }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat introuvable dans $vsPath : installez les outils C++ x64." }

Write-Host "==> Chargement de vcvars64"
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}

Write-Host "==> Configuration CMake ($buildDir)"
# Les arguments commençant par un tiret ne sont pas développés par PowerShell : on les cite.
cmake -S "$root" -B "$buildDir" -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DCMAKE_CXX_COMPILER=cl" `
    "-DXONAR_BUILD_DRIVER=ON" "-DXONAR_BUILD_TESTS=OFF" "-DXONAR_BUILD_TOOLS=OFF"
if ($LASTEXITCODE -ne 0) { throw "Configuration CMake en échec." }

Write-Host "==> Compilation de xonarse.sys"
cmake --build $buildDir --target xonarse_package
if ($LASTEXITCODE -ne 0) { throw "Compilation en échec." }

$pkg = Join-Path $buildDir 'driver-package'
Write-Host ""
Write-Host "Paquet pilote NON SIGNÉ assemblé dans : $pkg"
Get-ChildItem $pkg | Format-Table Name, Length, LastWriteTime -AutoSize
Write-Host "Étape suivante : scripts\driver\sign-test.ps1 (signature de test, pour la VM uniquement)."
