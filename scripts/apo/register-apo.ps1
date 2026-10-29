<#
.SYNOPSIS
    Associe les APO au seul endpoint haut-parleurs Xonar via SetValue/QueryValues.
.DESCRIPTION
    Prototype réversible. Sauvegarde CLIXML typée AVANT toute écriture ; chaque
    SetValue/DeleteValue est relu. Aucun changement d'ACL, pilote, volume, format
    ou protection. EFX 6->6 via GFX par défaut (pilote sans modes KS modernes).
    Legacy = SFX/EFX mono ; Composite = SFX/EFX multiples. Une seule famille.
#>
[CmdletBinding()]
param([string]$EndpointGuid,[ValidateSet('Efx','Sfx','Both')][string]$Stage='Efx',
      [ValidateSet('Gfx','LfxGfx','Legacy','Composite')][string]$Attachment='LfxGfx',
      [switch]$DryRun,[switch]$RestartAudio,[string]$BackupDirectory)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'ApoRegistry.psm1') -Force
if($Attachment -eq 'LfxGfx' -and -not $PSBoundParameters.ContainsKey('Stage')){$Stage='Both'}
if($Attachment -eq 'Gfx' -and $Stage -ne 'Efx'){throw 'GFX accepte uniquement notre EFX 6->6 ; utiliser -Stage Efx.'}
if($Attachment -eq 'LfxGfx' -and $Stage -ne 'Both'){throw 'LFX/GFX requiert les deux classes : -Stage Both.'}
$target=Get-XonarSpeakerTarget -EndpointGuid $EndpointGuid
if($target.DeviceState -ne 1){throw 'Endpoint non actif : aucune écriture.'}
$ours=@('{C243A8CB-D535-4BA0-BCC5-CC4338833E7A}','{D2FBC737-00C3-4482-B3DD-02D92C06003E}')
$fmt='{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}';$modes='{d3993a3f-99c2-4402-b5ec-a92a0367664b}'
$default='{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}'
$names=@("$fmt,0","$fmt,1","$fmt,2","$fmt,5","$fmt,6","$fmt,7","$fmt,13","$fmt,14","$fmt,15","$modes,5","$modes,7")
$key=Open-ApoEffectsKey -Subkey $target.Subkey
try {
    $before=@(foreach($name in $names){Get-ApoValue -Key $key -Name $name})
    foreach($name in @("$fmt,1","$fmt,2","$fmt,5","$fmt,6","$fmt,7","$fmt,13","$fmt,14","$fmt,15")){
        $value=Get-ApoValue -Key $key -Name $name
        if($value.Exists){foreach($clsid in @($value.Value)){if($clsid -notin $ours){throw "Effet tiers préexistant dans $name : refus de l'écraser."}}}
    }
    $disabled=Get-ApoValue -Key $key -Name '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5'
    if($disabled.Exists -and $disabled.Value -ne 0){throw 'Améliorations désactivées : aucune réactivation implicite.'}
}finally{$key.Dispose()}
$planned=@{}
foreach($name in $names){$planned[$name]=[pscustomobject]@{Name=$name;Exists=$false;Kind=$null;Value=$null}}
$planned["$fmt,0"]=[pscustomobject]@{Name="$fmt,0";Exists=$true;Kind='String';Value='{00000000-0000-0000-0000-000000000000}'}
foreach($effect in @(@{Name='Sfx';Legacy=5;Composite=13;Mode=5;Clsid=$ours[0]},@{Name='Efx';Legacy=7;Composite=15;Mode=7;Clsid=$ours[1]})){
    if($Attachment -in 'Gfx','LfxGfx'){continue}
    if($Stage -ne 'Both' -and $Stage -ne $effect.Name){continue}
    $pidValue=if($Attachment -eq 'Legacy'){$effect.Legacy}else{$effect.Composite}
    $kind=if($Attachment -eq 'Legacy'){'String'}else{'MultiString'}
    $data=if($Attachment -eq 'Legacy'){$effect.Clsid}else{,[string[]]@($effect.Clsid)}
    $name="$fmt,$pidValue";$planned[$name]=[pscustomobject]@{Name=$name;Exists=$true;Kind=$kind;Value=$data}
    $name="$modes,$($effect.Mode)";$planned[$name]=[pscustomobject]@{Name=$name;Exists=$true;Kind='MultiString';Value=[string[]]@($default)}
}
if($Attachment -in 'Gfx','LfxGfx'){
    $planned["$fmt,2"]=[pscustomobject]@{Name="$fmt,2";Exists=$true;Kind='String';Value=$ours[1]}
}
if($Attachment -eq 'LfxGfx'){
    $planned["$fmt,1"]=[pscustomobject]@{Name="$fmt,1";Exists=$true;Kind='String';Value=$ours[0]}
}
Write-Host "Cible validée : $($target.EndpointGuid) / $($target.Devnode) / $Stage / $Attachment"
if($DryRun){$planned.Values|Sort-Object Name|Format-Table Name,Exists,Kind,Value;return}
if(-not $BackupDirectory){$BackupDirectory=Join-Path $env:LOCALAPPDATA 'XonarSE\apo-backups'}
New-Item -ItemType Directory -Path $BackupDirectory -Force|Out-Null
$snapshot=Join-Path $BackupDirectory ('fx-'+[Guid]::NewGuid().ToString('N')+'.clixml')
[pscustomobject]@{Version=1;EndpointGuid=$target.EndpointGuid;Devnode=$target.Devnode;Subkey=$target.Subkey;Values=$before}|Export-Clixml -LiteralPath $snapshot -Encoding utf8
Write-Host "Sauvegarde typée : $snapshot"
$key=Open-ApoEffectsKey -Subkey $target.Subkey -Writable
try {
    foreach($name in $names){Set-ApoValue -Key $key -Value $planned[$name];Write-Host "Relu : $name"}
    $key.Flush()
}catch {
    foreach($value in $before){Set-ApoValue -Key $key -Value $value};$key.Flush();throw
}finally{$key.Dispose()}
if($RestartAudio){Restart-Service Audiosrv -Force}
Write-Host "Retour arrière : scripts\apo\unregister-apo.ps1 -RestoreBackup `"$snapshot`" -RestartAudio"
Write-Host 'Journal : %ProgramData%\XonarSE\apo-log.txt (vérifier le PID audiodg).'
