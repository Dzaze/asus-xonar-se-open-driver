# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
Installe ou met a jour la DLL APO signee, avec sauvegarde et restauration automatique sur erreur.
.DESCRIPTION
Ne modifie ni pilote, INF, format Windows, volume ou BCD. -WhatIf ne modifie pas le systeme audio.
Le certificat prive n'est pas distribue : fournir une DLL deja signee et son empreinte publique.
#>
[CmdletBinding()]
param(
    [ValidateSet('Install','Restore')][string]$Action='Install',
    [string]$SourceDll,
    [string]$InstallDir='C:\Program Files\XonarSE',
    [string]$ConfigDir='C:\ProgramData\XonarSE',
    [string]$BackupDirectory,
    [string]$TrustedSignerThumbprint='FF68A5C11C614F5E45AD64594D5A978BD9E5D321',
    [ValidateSet('None','AfterCopy','AfterRegistration','AfterAttachment','AfterProtection')][string]$FailurePoint='None',
    [switch]$WhatIf,[switch]$Pause
)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'XonarApoCommon.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'ApoInstallTransaction.psm1') -Force
if(-not $SourceDll){
    $candidates=@((Join-Path $PSScriptRoot '..\..\build-apo\src\windows-apo\XonarApo.dll'),(Join-Path $PSScriptRoot 'XonarApo.dll'),(Join-Path $PSScriptRoot '..\..\bin\XonarApo.dll'))
    $SourceDll=$candidates|Where-Object {Test-Path -LiteralPath $_}|Select-Object -First 1
    if(-not $SourceDll){$SourceDll=$candidates[0]}
}
if(-not(Invoke-XonarSelfElevation -ScriptPath $PSCommandPath -BoundParameters $PSBoundParameters -SkipIfNotAdmin:$WhatIf)){exit 0}
$log=Join-Path $env:TEMP ('xonar-apo-install-'+(Get-Date -Format yyyyMMdd-HHmmss)+'.log')
Start-Transcript -LiteralPath $log | Out-Null
$code=0
try {
    if($Action -eq 'Restore'){
        if(-not $BackupDirectory){throw 'Restore requiert -BackupDirectory.'}
        if($WhatIf){Write-Host "PLAN : restaurer DLL, COM et FxProperties depuis $BackupDirectory, avec redemarrage audio."}
        else{Restore-ApoInstallation -BackupDirectory $BackupDirectory}
    }else{
        Install-ApoTransactional -SourceDll $SourceDll -InstallDir $InstallDir -ConfigDir $ConfigDir -BackupDirectory $BackupDirectory -TrustedSignerThumbprint $TrustedSignerThumbprint -FailurePoint $FailurePoint -Preview:$WhatIf
    }
}catch{Write-Host $_.Exception.Message -ForegroundColor Red; $code=1}
finally{Stop-Transcript|Out-Null;if($Pause){Read-Host 'Entree pour fermer'|Out-Null}}
exit $code
