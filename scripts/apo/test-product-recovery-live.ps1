#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if(Test-Path -LiteralPath $OutputDirectory){throw 'Utiliser un dossier neuf.'}
New-Item -ItemType Directory -Path $OutputDirectory|Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
Import-Module (Join-Path $PSScriptRoot 'ApoRegistry.psm1') -Force
$target=Get-XonarSpeakerTarget
$pipe=[IO.Pipes.NamedPipeClientStream]::new('.','xonar-cc-test',[IO.Pipes.PipeDirection]::InOut)
$pipe.Connect(5000)
$writer=[IO.StreamWriter]::new($pipe,[Text.UTF8Encoding]::new($false),1024,$true);$writer.AutoFlush=$true
$reader=[IO.StreamReader]::new($pipe,[Text.Encoding]::UTF8,$false,1024,$true)
function Send([string]$command){
    $writer.WriteLine($command);$reply=$reader.ReadLine()
    if($null -eq $reply -or $reply.StartsWith('err:')){throw "Pipe : $reply"};$reply
}
function Read-Diagnostic([string]$Name){
    $json=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String((Send 'diagnostic')))
    $json|Set-Content -LiteralPath (Join-Path $OutputDirectory ($Name+'.json'))
    $json|ConvertFrom-Json
}
function Start-Source([string]$Name){
    Start-Process -FilePath (Join-Path $root 'build\apo-live-probe\apo-live-probe.exe') -ArgumentList @('--render','--endpoint',$target.EndpointGuid,'--audible','--source-stereo','--seconds','60','--frequency','60','--channel','stereo') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $OutputDirectory ($Name+'.txt')) -RedirectStandardError (Join-Path $OutputDirectory ($Name+'.err.txt'))
}
function Stop-Source($Player){if($Player -and -not $Player.HasExited){Stop-Process -Id $Player.Id}}
$snapshot=$null;$player=$null
Start-Transcript -LiteralPath (Join-Path $OutputDirectory 'recovery.txt')|Out-Null
try {
    $snapshot=Send 'snapshot'
    $player=Start-Source 'before'
    Start-Sleep -Milliseconds 2500
    $before=Read-Diagnostic 'diagnostic-before'
    if(-not @($before.signal.sources|Where-Object channels -eq 2).Count){throw 'Flux stereo absent du diagnostic reel.'}
    Stop-Source $player;$player=$null
    $helper=Join-Path $OutputDirectory 'rollback-helper.ps1'
    @'
param([string]$Repository,[string]$OutputDirectory)
$ErrorActionPreference='Stop'
$env:XONAR_INSTALL_TEST='1'
$backup=Join-Path $OutputDirectory 'failed-update'
$dll='C:\Program Files\XonarSE\XonarApo.dll'
$before=(Get-FileHash -LiteralPath $dll).Hash
$cfgBefore=(Get-FileHash 'C:\ProgramData\XonarSE\apo-efx.cfg').Hash
& (Join-Path $Repository 'scripts\install\install-xonar-apo.ps1') -SourceDll (Join-Path $Repository 'build-apo\src\windows-apo\XonarApo.dll') -BackupDirectory $backup -FailurePoint AfterAttachment *> (Join-Path $OutputDirectory 'failed-update.txt')
if($LASTEXITCODE -ne 1){throw 'La panne injectee doit etre signalee comme un echec.'}
$manifest=Import-Clixml -LiteralPath (Join-Path $backup 'install-manifest.clixml')
if($manifest.Outcome -ne 'RolledBack'){throw 'Rollback automatique non confirme.'}
if((Get-FileHash -LiteralPath $dll).Hash -ne $before){throw 'DLL non restauree.'}
if((Get-FileHash 'C:\ProgramData\XonarSE\apo-efx.cfg').Hash -ne $cfgBefore){throw 'Profil modifie par le rollback.'}
Import-Module (Join-Path $Repository 'scripts\apo\ApoRegistry.psm1') -Force
foreach($file in Get-ChildItem -LiteralPath $backup -Filter 'fx-*.clixml'){
    $snapshot=Import-Clixml -LiteralPath $file.FullName
    $key=Open-ApoEffectsKey -Subkey $snapshot.Subkey
    try{foreach($value in $snapshot.Values){
        $current=Get-ApoValue -Key $key -Name $value.Name
        if(($current|ConvertTo-Json -Depth 10 -Compress) -cne ($value|ConvertTo-Json -Depth 10 -Compress)){throw "FxProperties non restaure : $($value.Name)"}
    }}finally{$key.Dispose()}
}
Import-Module (Join-Path $Repository 'scripts\install\ApoInstallTransaction.psm1') -Force
$com=& (Get-Module ApoInstallTransaction) { @(foreach($root in Get-ApoComRoots){Read-ApoRegistryTree $root}) }
if(($com|ConvertTo-Json -Depth 20 -Compress) -cne ($manifest.ComTrees|ConvertTo-Json -Depth 20 -Compress)){throw 'COM non restaure.'}
if((Get-Service Audiosrv).Status -ne 'Running'){throw 'Service audio non redemarre.'}
[pscustomobject]@{Passed=$true;Outcome=$manifest.Outcome;DllHash=$before;FxRestored=$true;ComRestored=$true;ProfilePreserved=$true;AudioRunning=$true}|ConvertTo-Json|Set-Content (Join-Path $OutputDirectory 'rollback-proof.json')
'PASS : panne injectee, ancienne DLL/COM/FxProperties restaures, profil conserve.'|Set-Content (Join-Path $OutputDirectory 'helper-result.txt')
'@|Set-Content -LiteralPath $helper
    $pwsh=(Get-Process -Id $PID).Path
    $admin=Start-Process $pwsh -Verb RunAs -WindowStyle Hidden -ArgumentList @('-NoProfile','-File',('"'+$helper+'"'),'-Repository',('"'+$root+'"'),'-OutputDirectory',('"'+$OutputDirectory+'"')) -PassThru
    if(-not $admin.WaitForExit(45000)){throw 'Timeout du banc installateur.'}
    if($admin.ExitCode -ne 0){throw "Banc rollback echoue : $($admin.ExitCode)"}
    Get-Content (Join-Path $OutputDirectory 'helper-result.txt')
    $player=Start-Source 'after'
    Start-Sleep -Milliseconds 3000
    $after=Read-Diagnostic 'diagnostic-after'
    $ui=Send 'get';$ui
    if($after.state.revision -le $before.state.revision){throw 'Le profil ne semble pas avoir ete reapplique automatiquement.'}
    if($after.state.partiallyApplied -or $ui -notmatch 'state=Applied'){throw 'DSP non acquitte apres reprise.'}
    if(-not @($after.signal.sources|Where-Object channels -eq 2).Count){throw 'Flux stereo absent apres reprise.'}
    Send 'view diagnostic'
    [pscustomobject]@{Passed=$true;BeforeRevision=$before.state.revision;AfterRevision=$after.state.revision;BeforeInstances=$before.state.instances;AfterInstances=$after.state.instances;Ui=$ui}|ConvertTo-Json -Depth 15|Set-Content (Join-Path $OutputDirectory 'recovery-proof.json')
    "PASS : diagnostic reel, restauration automatique et reprise sur nouvel audiodg sans relancer l'application."
}finally{
    Stop-Source $player
    if($snapshot){try{[void](Send "restore $snapshot")}catch{Write-Warning $_}}
    $writer.Dispose();$reader.Dispose();$pipe.Dispose();Stop-Transcript|Out-Null
}
