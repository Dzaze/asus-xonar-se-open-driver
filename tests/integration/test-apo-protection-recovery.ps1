#requires -Version 7.0
# Exercise endpoint replacement after service restart without mutating Windows.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Import-Module (Join-Path $repo 'scripts\install\ApoEffectsProtection.psm1') -Force
& (Get-Module ApoEffectsProtection) {
    function script:Get-XonarEffectsEndpointKey {$script:current}
    function script:Test-XonarApoEffectsProtected {$script:protected.ContainsKey($script:current)}
    function script:Protect-XonarApoEffects {param($InstallDir) $script:protected[$script:current]=$true;++$script:locks}
    function script:Start-Sleep {param($Milliseconds)}
    function script:Get-Service {param($Name) [pscustomobject]@{Status=$script:services[$Name]}}
    function script:Stop-Service {param($Name,[switch]$Force) $script:services[$Name]='Stopped';++$script:stops}
    function script:Start-Service {param($Name)
        $script:services[$Name]='Running'
        if($Name -eq 'AudioEndpointBuilder'){
            ++$script:restarts
            if($script:scenario -eq 'NewGuid' -and $script:restarts -eq 1){$script:current='new-guid'}
            if($script:scenario -eq 'Unstable'){$script:current='new-'+$script:restarts}
        }
    }
    foreach($case in 'Stable','NewGuid','Unstable','ServicesStopped'){
        $script:scenario=$case;$script:current='old-guid';$script:protected=@{};$script:locks=0;$script:stops=0;$script:restarts=0
        $initial=if($case -eq 'ServicesStopped'){'Stopped'}else{'Running'}
        $script:services=@{Audiosrv=$initial;AudioEndpointBuilder=$initial}
        if($case -eq 'Stable'){$script:protected[$script:current]=$true}
        $failed=$false;try{Complete-XonarApoEffectsProtection}catch{$failed=$true}
        if($case -eq 'Unstable'){
            if(-not $failed -or $script:locks -ne 4){throw 'Unstable endpoint was accepted or retries were unbounded'}
        }else{
            if($failed -or -not(Test-XonarApoEffectsProtected)){throw ('Protected endpoint missing: '+$case)}
        }
        if($case -eq 'Stable' -and $script:stops -ne 0){throw 'Stable protected endpoint unnecessarily restarted'}
        if($case -eq 'NewGuid' -and ($script:locks -ne 2 -or -not $script:protected.ContainsKey('new-guid'))){throw 'Only old GUID was protected'}
        if($case -eq 'ServicesStopped' -and $script:stops -ne 0){throw 'Inactive services modified'}
        if($script:services.Audiosrv -ne $initial -or $script:services.AudioEndpointBuilder -ne $initial){throw 'Original service state not restored'}
        Write-Host ('PASS : '+$case)
    }
    Write-Host '4 endpoint protection recovery scenarios passed.'
}
