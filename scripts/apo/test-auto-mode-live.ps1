#Requires -Version 7.0
<#
.SYNOPSIS
    Prouve Auto par formats declares 2/6, canaux isoles, flux simultanes et bascule live.
.DESCRIPTION
    Signal -40 dBFS. Aucun changement du format Windows ou du pilote. Le profil
    courant est sauvegarde et restaure dans finally. XONAR_TEST_PIPE=1 requis.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'ApoRegistry.psm1') -Force
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if(Test-Path -LiteralPath $OutputDirectory){throw 'Dossier de preuve deja present.'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path $OutputDirectory).Path
$target=Get-XonarSpeakerTarget
$pipe=[IO.Pipes.NamedPipeClientStream]::new('.','xonar-cc-test',[IO.Pipes.PipeDirection]::InOut)
$snapshot=$null;$players=[Collections.Generic.List[Diagnostics.Process]]::new();$rows=[Collections.Generic.List[object]]::new()
function Send-AutoCommand([string]$Command) {
    $writer.WriteLine($Command);$read=$reader.ReadLineAsync()
    if(-not $read.Wait(20000)){throw "Timeout UI : $Command"}
    $reply=$read.Result
    if($null -eq $reply -or $reply.StartsWith('err:')){throw "UI refuse : $Command / $reply"}
    $reply
}
function Start-AutoSource([string]$Label,[string[]]$Extra) {
    $arguments=@('--render','--endpoint',$target.EndpointGuid,'--audible','--seconds','120','--frequency','60','--channel','stereo')+$Extra
    $p=Start-Process -FilePath (Join-Path $root 'build\apo-live-probe\apo-live-probe.exe') -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $OutputDirectory "$Label.txt") -RedirectStandardError (Join-Path $OutputDirectory "$Label.err.txt")
    $players.Add($p);$p
}
function Stop-AutoSource([Diagnostics.Process]$Player) {
    $Player.Refresh();if(-not $Player.HasExited){Stop-Process -Id $Player.Id}
}
function Assert-AutoMeasure([string]$Name,[scriptblock]$Check) {
    [void](Send-AutoCommand 'apply');Start-Sleep -Milliseconds 1100
    $expected=[IO.File]::GetLastWriteTimeUtc('C:\ProgramData\XonarSE\apo-efx.cfg').ToFileTimeUtc()
    $s=$null
    for($retry=0;$retry -lt 40;$retry++) {
        try {$candidate=Get-Content 'C:\ProgramData\XonarSE\apo-live-status.json' -Raw|ConvertFrom-Json
            if($candidate.active -and $candidate.configFileTime -eq $expected){$s=$candidate;break}
        }catch{}
        Start-Sleep -Milliseconds 100
    }
    if(-not $s){throw "Aucun acquittement DSP : $Name"}
    $passed=[bool](& $Check $s)
    $ui=Send-AutoCommand 'get'
    $rows.Add([pscustomobject]@{Name=$Name;Passed=$passed;Status=$s;Ui=$ui})
    $rows|ConvertTo-Json -Depth 12|Set-Content (Join-Path $OutputDirectory 'results.json')
    Write-Host "$Name : $passed / RMS $($s.outputRms -join ', ')"
    if(-not $passed){throw "Resultat incorrect : $Name"}
    $s
}
Start-Transcript (Join-Path $OutputDirectory 'live.txt')|Out-Null
try {
    $pipe.Connect(5000);$writer=[IO.StreamWriter]::new($pipe,[Text.UTF8Encoding]::new($false),1024,$true);$writer.AutoFlush=$true
    $reader=[IO.StreamReader]::new($pipe,[Text.Encoding]::UTF8,$false,1024,$true)
    $snapshot=Send-AutoCommand 'snapshot';$snapshot|Set-Content (Join-Path $OutputDirectory 'profile-before.base64')
    $profile=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($snapshot))|ConvertFrom-Json -Depth 100
    $profile.ListeningMode=0;$profile.SourcePolicy=1;$profile.Output.MasterGainDb=0
    $profile.Output.ChannelGainDb=@(0,0,0,0,0,0,0,0);$profile.Output.ChannelDelayMs=@(0,0,0,0,0,0,0,0)
    $profile.Output.ChannelMute=@($false,$false,$false,$false,$false,$false,$false,$false)
    foreach($channel in $profile.Output.Eq){foreach($band in $channel){$band.Enabled=$false;$band.GainDb=0}}
    $profile.Output.BassManagement.Enabled=$false;$profile.Output.Limiter.Enabled=$true;$profile.Output.Limiter.CeilingDb=-0.3
    $baseline=[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes(($profile|ConvertTo-Json -Depth 100 -Compress)))
    [void](Send-AutoCommand "restore $baseline")
    $logBefore=(Get-Content 'C:\ProgramData\XonarSE\apo-log.txt').Count

    $p=Start-AutoSource 'stereo' @('--source-stereo')
    $null=Assert-AutoMeasure 'Auto : vraie source stereo vers six sorties' {param($s) $s.auto -and @($s.outputRms|Where-Object {$_ -gt .0001}).Count -eq 6}
    [void](Send-AutoCommand 'set mode 5.1')
    $null=Assert-AutoMeasure 'Meme flux stereo : Auto vers 5.1 manuel' {param($s) -not $s.auto -and @($s.outputRms[2..5]|Where-Object {$_ -ne 0}).Count -eq 0}
    [void](Send-AutoCommand 'set mode auto')
    $null=Assert-AutoMeasure 'Meme flux stereo : 5.1 manuel vers Auto' {param($s) $s.auto -and @($s.outputRms|Where-Object {$_ -gt .0001}).Count -eq 6}
    [void](Send-AutoCommand 'set masterGainDb -6')
    $null=Assert-AutoMeasure 'Auto : calibration -6 dB une seule fois' {param($s) [Math]::Abs($s.inputRms[0]-.00707107) -lt .0001 -and [Math]::Abs($s.outputRms[0]/$s.inputRms[0]-.501187) -lt .02}
    [void](Send-AutoCommand 'set masterGainDb 0');Stop-AutoSource $p

    $p=Start-AutoSource 'native-front-pair' @()
    $null=Assert-AutoMeasure 'Auto : vrai flux six canaux aux surrounds silencieux reste intact' {param($s) $s.auto -and $s.outputRms[0] -gt .001 -and @($s.outputRms[2..5]|Where-Object {$_ -ne 0}).Count -eq 0}
    Stop-AutoSource $p
    foreach($channel in 0..5) {
        $p=Start-AutoSource "native-$channel" @('--channel',[string]$channel)
        $null=Assert-AutoMeasure "Auto : canal natif $channel sans duplication" {param($s)
            $valid=$s.outputRms[$channel] -gt .001
            foreach($other in 0..5){if($other -ne $channel -and $s.outputRms[$other] -ne 0){$valid=$false}}
            $valid
        }
        Stop-AutoSource $p
    }
    $p=Start-AutoSource 'mono' @('--source-mono')
    $null=Assert-AutoMeasure 'Auto : source mono negociee sans perte audio' {param($s) @($s.outputRms|Where-Object {$_ -gt .0001}).Count -eq 6}
    Stop-AutoSource $p
    $p=Start-AutoSource 'stereo-44100' @('--source-stereo','--source-rate','44100')
    $null=Assert-AutoMeasure 'Auto : source stereo 44.1 kHz, mix Windows toujours 48 kHz' {param($s) $s.auto -and $s.sampleRate -eq 48000 -and @($s.outputRms|Where-Object {$_ -gt .0001}).Count -eq 6}
    Stop-AutoSource $p

    $p=Start-AutoSource 'stereo-pcm16' @('--source-stereo','--source-pcm16')
    $null=Assert-AutoMeasure 'Auto : source stereo PCM 16 bits' {param($s) $s.auto -and @($s.outputRms|Where-Object {$_ -gt .0001}).Count -eq 6}
    Stop-AutoSource $p
    $p=Start-AutoSource 'stereo-media' @('--source-stereo','--media')
    $null=Assert-AutoMeasure 'Auto : categorie Media stereo' {param($s) $s.auto -and @($s.outputRms|Where-Object {$_ -gt .0001}).Count -eq 6}
    Stop-AutoSource $p
    $p=Start-AutoSource 'native-media' @('--media')
    $null=Assert-AutoMeasure 'Auto : categorie Media 5.1 aux surrounds silencieux' {param($s) $s.auto -and $s.outputRms[0] -gt .001 -and @($s.outputRms[2..5]|Where-Object {$_ -ne 0}).Count -eq 0}
    Stop-AutoSource $p

    $stereo=Start-AutoSource 'mixed-stereo' @('--source-stereo')
    $native=Start-AutoSource 'mixed-native-center' @('--channel','2','--frequency','1200')
    $null=Assert-AutoMeasure 'Auto : stereo et 5.1 simultanes, centre natif conserve' {param($s) $s.auto -and @($s.outputRms|Where-Object {$_ -gt .0001}).Count -eq 6 -and $s.outputRms[2] -gt .0075}
    Stop-AutoSource $stereo;Stop-AutoSource $native
    Get-Content 'C:\ProgramData\XonarSE\apo-log.txt'|Select-Object -Skip $logBefore|Set-Content (Join-Path $OutputDirectory 'apo-formats.txt')
    Write-Host 'PASS : distinction par format declare, preservation native et routage simultane verifies.'
}finally {
    try {if($snapshot){[void](Send-AutoCommand "restore $snapshot")}}finally{
        foreach($p in $players){Stop-AutoSource $p}
        $pipe.Dispose();Stop-Transcript|Out-Null
    }
}
