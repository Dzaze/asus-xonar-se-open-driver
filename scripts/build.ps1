<#
.SYNOPSIS
    Configure et compile le projet Xonar SE Open Driver avec MSVC + Ninja.

.DESCRIPTION
    Localise Visual Studio (Build Tools ou édition complète) via vswhere, charge
    l'environnement vcvars64 dans la session PowerShell courante, puis exécute
    cmake --preset / cmake --build. Fonctionne depuis un PowerShell ordinaire :
    il n'est pas nécessaire d'ouvrir un « Developer PowerShell ».

.PARAMETER Config
    Debug (défaut) ou Release. Sélectionne le preset msvc-ninja-<config>.

.PARAMETER Test
    Lance ctest après la compilation.

.PARAMETER Clean
    Supprime le dossier de build du preset avant de configurer.

.EXAMPLE
    .\scripts\build.ps1
    .\scripts\build.ps1 -Config Release -Test
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Config = 'Debug',
    [switch]$Test,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$preset = "msvc-ninja-$($Config.ToLowerInvariant())"

function Write-Step([string]$Message) { Write-Host "==> $Message" -ForegroundColor Cyan }
function Fail([string]$Message) { Write-Host "ERREUR : $Message" -ForegroundColor Red; exit 1 }

# ---------------------------------------------------------------------------
# 1. Localiser Visual Studio et vcvars64.bat
# ---------------------------------------------------------------------------
Write-Step "Recherche de Visual Studio (vswhere)"
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Fail "vswhere introuvable ($vswhere). Installez Visual Studio Build Tools avec la charge de travail C++."
}

$vsPath = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    Fail "Aucune installation Visual Studio avec les outils C++ (VC.Tools.x86.x64) n'a été trouvée."
}
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    Fail "vcvars64.bat introuvable sous $vsPath"
}
Write-Host "    Visual Studio : $vsPath"

# ---------------------------------------------------------------------------
# 2. Importer l'environnement vcvars64 dans la session courante
# ---------------------------------------------------------------------------
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Step "Chargement de l'environnement vcvars64"
    $envDump = cmd /c "`"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0 -or -not $envDump) {
        Fail "L'exécution de vcvars64.bat a échoué."
    }
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') {
            $name = $Matches[1]
            $value = $Matches[2]
            # Ne pas écraser les variables internes de PowerShell
            if ($name -notin @('PROMPT', 'PWD')) {
                Set-Item -Path "Env:$name" -Value $value
            }
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        Fail "cl.exe reste introuvable après le chargement de vcvars64."
    }
}
Write-Host "    cl.exe : $((Get-Command cl.exe).Source)"

# ---------------------------------------------------------------------------
# 3. Vérifier cmake et ninja
# ---------------------------------------------------------------------------
foreach ($tool in 'cmake', 'ninja') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "$tool introuvable dans le PATH. Installez-le (winget install Kitware.CMake / Ninja-build.Ninja)."
    }
}

# ---------------------------------------------------------------------------
# 4. Configurer, compiler, tester
# ---------------------------------------------------------------------------
Push-Location $repoRoot
try {
    $buildDir = Join-Path $repoRoot "build\$preset"
    if ($Clean -and (Test-Path $buildDir)) {
        Write-Step "Nettoyage de $buildDir"
        Remove-Item -Recurse -Force $buildDir
    }

    Write-Step "Configuration CMake (preset $preset)"
    cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { Fail "La configuration CMake a échoué (code $LASTEXITCODE)." }

    Write-Step "Compilation (preset $preset)"
    cmake --build --preset $preset
    if ($LASTEXITCODE -ne 0) { Fail "La compilation a échoué (code $LASTEXITCODE)." }

    if ($Test) {
        Write-Step "Tests (ctest, preset $preset)"
        ctest --preset $preset --output-on-failure
        if ($LASTEXITCODE -ne 0) { Fail "Des tests ont échoué (code $LASTEXITCODE)." }
    }

    Write-Host ""
    Write-Host "Build $Config terminé avec succès. Sortie : $buildDir" -ForegroundColor Green
}
finally {
    Pop-Location
}
