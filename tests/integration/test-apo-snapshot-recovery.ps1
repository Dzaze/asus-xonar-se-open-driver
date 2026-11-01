#requires -Version 7.0
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Import-Module (Join-Path $repo 'scripts\apo\ApoRegistry.psm1') -Force
$testRoot=Join-Path $env:TEMP ('xonar-snapshot-tests-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot|Out-Null
try{
    & (Get-Module ApoRegistry) {
        param($testRoot)
        $script:oldGuid='{00000000-0000-0000-0000-000000000001}'
        $script:newGuid='{00000000-0000-0000-0000-000000000002}'
        function script:Get-XonarSpeakerTarget {param($EndpointGuid)
            if($script:case -ne 'OriginalEndpoint' -and $EndpointGuid){throw 'Old endpoint inactive'}
            $guid=if($script:case -eq 'OriginalEndpoint'){$script:oldGuid}else{$script:newGuid}
            [pscustomobject]@{EndpointGuid=$guid;Devnode=if($script:case -eq 'ChangedHardware'){'other-device'}else{'same-device'};Subkey="$script:RenderSubkey\$guid\FxProperties"}
        }
        function script:Open-ApoEffectsKey {param($Subkey,[switch]$Writable)
            $script:opened=$Subkey
            $key=[pscustomobject]@{}
            $key|Add-Member ScriptMethod Flush {}
            $key|Add-Member ScriptMethod Dispose {}
            $key
        }
        function script:Set-ApoValue {param($Key,$Value) ++$script:writes}
        foreach($case in 'OriginalEndpoint','RecreatedEndpoint','ChangedHardware','TamperedPath'){
            $script:case=$case;$script:writes=0;$script:opened=$null
            $path=Join-Path $testRoot ($case+'.clixml')
            $snapshot=[pscustomobject]@{Version=1;EndpointGuid=$script:oldGuid;Devnode='same-device';Subkey="$script:RenderSubkey\$script:oldGuid\FxProperties";Values=@([pscustomobject]@{Name='effect';Exists=$true;Kind='String';Value='ours'})}
            if($case -eq 'TamperedPath'){$snapshot.Subkey='SOFTWARE\Other'}
            $snapshot|Export-Clixml -LiteralPath $path
            $failed=$false;try{Restore-ApoSnapshot $path}catch{$failed=$true}
            if($case -in 'ChangedHardware','TamperedPath'){
                if(-not $failed -or $script:writes -ne 0){throw ('Invalid snapshot accepted: '+$case)}
            }else{
                $guid=if($case -eq 'OriginalEndpoint'){$script:oldGuid}else{$script:newGuid}
                if($failed -or $script:writes -ne 1 -or $script:opened -ne "$script:RenderSubkey\$guid\FxProperties"){throw ('Wrong endpoint restored: '+$case)}
            }
            Write-Host ('PASS : '+$case)
        }
        Write-Host '4 snapshot recovery scenarios passed.'
    } $testRoot
}finally{
    $absolute=[IO.Path]::GetFullPath($testRoot);$prefix=[IO.Path]::GetFullPath($env:TEMP).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($absolute) -notlike 'xonar-snapshot-tests-*'){throw 'Unsafe cleanup'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
