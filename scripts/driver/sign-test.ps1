<#
.SYNOPSIS
    Produit le catalogue et signe le paquet pilote avec un certificat de TEST.
.DESCRIPTION
    À utiliser uniquement pour la machine virtuelle de test (test-signing activé dans
    la VM). Crée, si besoin, un certificat auto-signé "Xonar SE Open Driver TEST" dans
    le magasin personnel de l'utilisateur courant (CurrentUser\My) et l'exporte en
    .cer pour l'installer dans la VM. Ne modifie ni Secure Boot ni la politique de
    signature de la machine hôte.
.PARAMETER PackageDir
    Dossier contenant xonarse.sys et xonarse.inf (défaut : build\driver-debug\driver-package).
#>
[CmdletBinding()]
param(
    [string] $PackageDir = ''
)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
if (-not $PackageDir) { $PackageDir = Join-Path $root 'build\driver-debug\driver-package' }
if (-not (Test-Path (Join-Path $PackageDir 'xonarse.sys'))) { throw "xonarse.sys absent de $PackageDir : lancez build-driver.ps1 d'abord." }

$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$binVer = Get-ChildItem "$kits\bin" -Directory | Where-Object { Test-Path "$($_.FullName)\x64\signtool.exe" } |
    Sort-Object Name -Descending | Select-Object -First 1
if (-not $binVer) { throw "signtool introuvable dans le WDK." }
$signtool = Join-Path $binVer.FullName 'x64\signtool.exe'
$inf2cat = Join-Path $binVer.FullName 'x86\Inf2Cat.exe'
if (-not (Test-Path $inf2cat)) { $inf2cat = Join-Path $binVer.FullName 'x64\Inf2Cat.exe' }
if (-not (Test-Path $inf2cat)) { throw "Inf2Cat introuvable dans le WDK." }

$subject = 'CN=Xonar SE Open Driver TEST'
$cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $subject -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
if (-not $cert) {
    Write-Host "==> Création du certificat de test $subject (CurrentUser\My)"
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\CurrentUser\My `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(2)
}
$cerPath = Join-Path $PackageDir 'XonarSeOpenDriver-TEST.cer'
Export-Certificate -Cert $cert -FilePath $cerPath -Force | Out-Null
Write-Host "    Certificat exporté : $cerPath (à importer dans la VM, magasins Root et TrustedPublisher)"

Write-Host "==> Inf2Cat (catalogue xonarse.cat, Windows 10/11 x64)"
& $inf2cat /driver:$PackageDir /os:10_X64 /verbose
if ($LASTEXITCODE -ne 0) { throw "Inf2Cat en échec." }

Write-Host "==> Signature de xonarse.sys et xonarse.cat"
foreach ($f in 'xonarse.sys', 'xonarse.cat') {
    & $signtool sign /v /fd SHA256 /sha1 $cert.Thumbprint /t http://timestamp.digicert.com (Join-Path $PackageDir $f)
    if ($LASTEXITCODE -ne 0) {
        Write-Warning "Horodatage indisponible, nouvelle tentative sans horodatage pour $f"
        & $signtool sign /v /fd SHA256 /sha1 $cert.Thumbprint (Join-Path $PackageDir $f)
        if ($LASTEXITCODE -ne 0) { throw "Signature en échec pour $f." }
    }
}
Write-Host ""
Write-Host "Paquet signé en TEST : $PackageDir"
Write-Host "Installation : uniquement dans la VM (docs/driver-vm-setup.md). Jamais sur le PC principal."
