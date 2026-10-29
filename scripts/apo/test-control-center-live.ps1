#Requires -Version 7.0
<#
.SYNOPSIS
    Banc UI -> pipe de test -> fichier -> APO audiodg sur un seul flux continu.
.DESCRIPTION
    L'application doit être lancée avec XONAR_TEST_PIPE=1. Signal stéréo 60 Hz
    à -40 dBFS. Sauvegarde le profil, teste les quatre modes et les réglages,
    restaure le profil dans finally puis arrête sa seule sonde. Aucun pilote,
    format Windows, volume matériel, service ou registre n'est modifié.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'ApoRegistry.psm1') -Force
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if(Test-Path -LiteralPath $OutputDirectory){throw 'Utiliser un dossier neuf.'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$pipe=[IO.Pipes.NamedPipeClientStream]::new('.','xonar-cc-test',[IO.Pipes.PipeDirection]::InOut)
$tone=$null;$snapshot=$null;$rows=[Collections.Generic.List[object]]::new()
function Send([string]$Command){
    $writer.WriteLine($Command)
    $read=$reader.ReadLineAsync()
    if(-not $read.Wait(20000)){throw "Timeout de la commande $Command"}
    $reply=$read.Result
    if($null -eq $reply -or $reply.StartsWith('err:')){throw "Commande refusée : $Command / $reply"}
    return $reply
}
function Read-LiveMeasurement([string]$Name,[scriptblock]$Check){
    [void](Send 'apply')
    Start-Sleep -Milliseconds 900
    $expected=[IO.File]::GetLastWriteTimeUtc('C:\ProgramData\XonarSE\apo-efx.cfg').ToFileTimeUtc()
    $status=$null
    for($retry=0;$retry -lt 40;$retry++){
        try {$candidate=Get-Content 'C:\ProgramData\XonarSE\apo-live-status.json' -Raw|ConvertFrom-Json
            if($candidate.active -and $candidate.configFileTime -eq $expected){$status=$candidate;break}
        }catch{}
        Start-Sleep -Milliseconds 100
    }
    if(-not $status){throw "Pas d'acquittement audiodg : $Name"}
    if($script:audioPid -and $script:audioPid -ne $status.pid){throw 'Le graphe a redémarré : test de bascule continue invalide.'}
    $script:audioPid=$status.pid
    $success=[bool](& $Check $status)
    $rows.Add([pscustomobject]@{Name=$Name;Passed=$success;Status=$status})
    $rows|ConvertTo-Json -Depth 12|Set-Content (Join-Path $OutputDirectory 'results.json')
    Write-Host "$Name : $success / audiodg $($status.pid) / RMS $($status.outputRms -join ', ')"
    if(-not $success){throw "Mesure DSP non conforme : $Name"}
    return $status
}
Start-Transcript (Join-Path $OutputDirectory 'live.txt')|Out-Null
try {
    $pipe.Connect(5000)
    $writer=[IO.StreamWriter]::new($pipe,[Text.UTF8Encoding]::new($false),1024,$true);$writer.AutoFlush=$true
    $reader=[IO.StreamReader]::new($pipe,[Text.Encoding]::UTF8,$false,1024,$true)
    $snapshot=Send 'snapshot';$snapshot|Set-Content (Join-Path $OutputDirectory 'profile-before.base64')
    $profile=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($snapshot))|ConvertFrom-Json -Depth 100
    $profile.ListeningMode=0;$profile.Output.MasterGainDb=0
    $profile.Output.ChannelGainDb=@(0,0,0,0,0,0,0,0)
    $profile.Output.ChannelDelayMs=@(0,0,0,0,0,0,0,0)
    $profile.Output.ChannelMute=@($false,$false,$false,$false,$false,$false,$false,$false)
    foreach($channel in $profile.Output.Eq){foreach($band in $channel){$band.Enabled=$false;$band.GainDb=0}}
    $profile.Output.BassManagement.Enabled=$false
    $profile.Output.BassManagement.CrossoverHz=80
    $profile.Output.BassManagement.HighPassSatellites=$true
    $profile.Output.BassManagement.SumToLfe=$true
    $profile.Output.Limiter.Enabled=$true;$profile.Output.Limiter.CeilingDb=-0.3
    $baseline=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($profile|ConvertTo-Json -Depth 100 -Compress)))
    [void](Send "restore $baseline")
    $target=Get-XonarSpeakerTarget
    $tone=Start-Process -FilePath (Join-Path $root 'build\apo-live-probe\apo-live-probe.exe') -WindowStyle Hidden -ArgumentList @('--render','--endpoint',$target.EndpointGuid,'--audible','--seconds','180','--frequency','60','--channel','stereo') -PassThru -RedirectStandardOutput (Join-Path $OutputDirectory 'render.txt') -RedirectStandardError (Join-Path $OutputDirectory 'render.err.txt')
    $script:audioPid=0
    $base=Read-LiveMeasurement '5.1 : FL/FR seulement pour une source stéréo' {param($s) $s.mode -eq 0 -and $s.outputRms[0] -gt .001 -and $s.outputRms[2] -eq 0 -and $s.outputRms[3] -eq 0 -and $s.outputRms[4] -eq 0 -and $s.outputRms[5] -eq 0}
    [void](Send 'set mode upmix')
    $null=Read-LiveMeasurement 'Upmix : six sorties alimentées' {param($s) $s.mode -eq 5 -and @($s.outputRms|Where-Object {$_ -gt .0001}).Count -eq 6}
    [void](Send 'set mode 2.0')
    $null=Read-LiveMeasurement '2.0 : FL/FR, quatre sorties silencieuses' {param($s) $s.mode -eq 6 -and $s.outputRms[0] -gt .001 -and @($s.outputRms[2..5]|Where-Object {$_ -ne 0}).Count -eq 0}
    [void](Send 'set mode 2.1')
    $null=Read-LiveMeasurement '2.1 : FL/FR et caisson, centre/surrounds silencieux' {param($s) $s.mode -eq 3 -and $s.outputRms[3] -gt .0001 -and $s.outputRms[2] -eq 0 -and $s.outputRms[4] -eq 0 -and $s.outputRms[5] -eq 0}
    [void](Send 'set mode 5.1');[void](Send 'set masterGainDb -12')
    $null=Read-LiveMeasurement 'Gain maître -12 dB réellement appliqué' {param($s) [Math]::Abs($s.outputRms[0]/$base.outputRms[0]-.251189) -lt .02}
    [void](Send 'set masterGainDb 0');[void](Send 'set chgain FL -6')
    $null=Read-LiveMeasurement 'Gain FL -6 dB, FR conservé' {param($s) [Math]::Abs($s.outputRms[0]/$s.outputRms[1]-.501187) -lt .02}
    [void](Send 'set chgain FL 0');[void](Send 'set mute FR 1')
    $null=Read-LiveMeasurement 'Mute FR isolé' {param($s) $s.outputRms[1] -eq 0 -and $s.outputRms[0] -gt .001}
    [void](Send 'set mute FR 0');[void](Send 'set delay FL 5')
    $null=Read-LiveMeasurement 'Délai FL 5 ms : phase de 108 degrés à 60 Hz' {param($s) [Math]::Abs($s.correlation[0]+.309017) -lt .03 -and $s.correlation[1] -gt .99}
    [void](Send 'set delay FL 0');[void](Send 'set eq FL 0 60 1 -12')
    $null=Read-LiveMeasurement 'EQ FL -12 dB à 60 Hz, FR conservé' {param($s) [Math]::Abs($s.outputRms[0]/$s.outputRms[1]-.251189) -lt .03}
    [void](Send 'set eq FL 0 60 1 0');[void](Send 'set bass 1')
    $bass=Read-LiveMeasurement 'Bass management : passe-haut et dérivation du caisson' {param($s) $s.outputRms[0] -lt $base.outputRms[0]*.4 -and $s.outputRms[3] -gt .001}
    [void](Send 'set crossover 200')
    $null=Read-LiveMeasurement 'Crossover 80 vers 200 Hz réellement modifié' {param($s) $s.outputRms[0] -lt $bass.outputRms[0]*.1 -and $s.outputRms[3] -gt .001}
    [void](Send 'set bass 0');[void](Send 'set crossover 80')
    $null=Read-LiveMeasurement 'Retour neutre après les filtres' {param($s) [Math]::Abs($s.outputRms[0]/$base.outputRms[0]-1) -lt .02 -and $s.outputRms[3] -eq 0}
    [void](Send 'set ceiling -6');[void](Send 'set limiter 1')
    $null=Read-LiveMeasurement 'Paramètres limiteur acquittés sur le flux actif' {param($s) $s.active -and $s.outputRms[0] -gt .001}
    $speakers=Send 'speakers';Write-Host "Bouton enceintes : $speakers"
    if($speakers -notlike 'Test terminé*'){throw 'Test des enceintes via la page en échec.'}
    Write-Host 'PASS : modes et réglages sur le même audiodg, test des six enceintes exécuté par la page.'
}finally {
    try {if($snapshot){[void](Send "restore $snapshot");Write-Host "Profil original restauré dans l'interface et le DSP."}}finally{
        if($tone){$tone.Refresh();if(-not $tone.HasExited){Stop-Process -Id $tone.Id}}
        $pipe.Dispose();Stop-Transcript|Out-Null
    }
}
