<#
.SYNOPSIS
    Restaure une sauvegarde typée exacte, ou retire seulement nos CLSID.
.DESCRIPTION
    .NET SetValue/QueryValues ; pas de reg import/KEY_WRITE, désenregistrement COM,
    suppression de DLL/dossier, changement d'ACL ou de pilote.
#>
[CmdletBinding()]
param([string]$EndpointGuid,[string]$RestoreBackup,[switch]$DryRun,[switch]$RestartAudio)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'ApoRegistry.psm1') -Force
if($RestoreBackup){
    if($DryRun){Write-Host "Restauration prévue : $RestoreBackup";return}
    Restore-ApoSnapshot -Path $RestoreBackup
}else{
    $target=Get-XonarSpeakerTarget -EndpointGuid $EndpointGuid
    $key=Open-ApoEffectsKey -Subkey $target.Subkey -Writable:(-not $DryRun)
    try {
        $ours=@('{C243A8CB-D535-4BA0-BCC5-CC4338833E7A}','{D2FBC737-00C3-4482-B3DD-02D92C06003E}')
        foreach($pidValue in 1,2,5,6,7,13,14,15){
            $value=Get-ApoValue -Key $key -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},$pidValue"
            if(-not $value.Exists){continue}
            $remaining=@($value.Value|Where-Object {$_ -notin $ours})
            if($remaining.Count -eq @($value.Value).Count){continue}
            $value.Exists=$remaining.Count -gt 0
            $value.Value=if($value.Kind -eq 'MultiString'){,[string[]]$remaining}else{if($remaining.Count){$remaining[0]}else{$null}}
            if($DryRun){Write-Host "Retrait de nos CLSID : $($value.Name)"}else{Set-ApoValue -Key $key -Value $value}
        }
        # Sans sauvegarde, association et modes préexistants sont conservés.
    }finally{$key.Dispose()}
}
if($RestartAudio -and -not $DryRun){Restart-Service Audiosrv -Force}
