# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([ValidateSet('Protect','Restore','Status')][string]$Action='Protect',[string]$InstallDir='C:\Program Files\XonarSE',[switch]$Preview)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot '..\install\ApoEffectsProtection.psm1') -Force
Import-Module (Join-Path $PSScriptRoot '..\install\XonarApoCommon.psm1') -Force
if($Action -eq 'Status'){Test-XonarApoEffectsProtected;return}
if(-not(Invoke-XonarSelfElevation -ScriptPath $PSCommandPath -BoundParameters $PSBoundParameters -SkipIfNotAdmin:$Preview)){exit 0}
$log=Join-Path $env:TEMP ('xonar-effects-protection-'+(Get-Date -Format yyyyMMdd-HHmmss)+'.log')
Start-Transcript -LiteralPath $log|Out-Null
$running=(Get-Service Audiosrv).Status -eq 'Running'
$builder=(Get-Service AudioEndpointBuilder).Status -eq 'Running'
$before=$null;$code=0
try{
    if($Preview){
        if($Action -eq 'Protect'){Protect-XonarApoEffects -InstallDir $InstallDir -Preview}
        else{Restore-XonarApoEffectsProtection -InstallDir $InstallDir -Preview}
    }else{
        try{$before=@(Get-XonarApoProtectionSnapshot -AllowUnattached)}catch{if($Action -ne 'Restore'){throw}}
        Stop-Service Audiosrv -Force
        Stop-Service AudioEndpointBuilder -Force
        if($Action -eq 'Protect'){Protect-XonarApoEffects -InstallDir $InstallDir}
        else{Restore-XonarApoEffectsProtection -InstallDir $InstallDir}
        if($builder){Start-Service AudioEndpointBuilder}
        if($running){Start-Service Audiosrv}
        if($running){(Get-Service Audiosrv).WaitForStatus('Running',[TimeSpan]::FromSeconds(15))}
    }
}catch{
    $code=1;Write-Host $_.Exception.Message -ForegroundColor Red
    if($before){try{Restore-XonarApoProtectionSnapshot $before}catch{Write-Host ('Restauration permissions incomplete : '+$_.Exception.Message) -ForegroundColor Red}}
}finally{
    if(-not $Preview){if($builder){Start-Service AudioEndpointBuilder};if($running){Start-Service Audiosrv}}
    Stop-Transcript|Out-Null
}
exit $code
