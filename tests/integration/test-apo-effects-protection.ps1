#requires -Version 7.0
# Tests ACL reels en memoire ; les acces natifs au registre sont injectes.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Import-Module (Join-Path $repo 'scripts\install\ApoEffectsProtection.psm1') -Force
$testRoot=Join-Path $env:TEMP ('xonar-protection-tests-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot|Out-Null
try{
    & (Get-Module ApoEffectsProtection) {
        param($testRoot)
        $original='O:SYG:SYD:AI(A;CI;KA;;;SY)(A;CI;KR;;;BU)(A;CI;KA;;;BA)'
        $prefix='SYSTEM\CurrentControlSet\Control\DeviceClasses\{6994ad04-93ef-11d0-a3cc-00a0c9223196}\##?#USB#VID_0B05&PID_189D&MI_00#unit'
        $script:paths=@(foreach($index in 1..3){$prefix+$index+'\#TopologySpeaker\Device Parameters\FX\0'})
        $script:descriptors=@{};foreach($path in $script:paths){$script:descriptors[$path]=$original}
        $script:writes=0;$script:failAt=0
        function script:Get-XonarProtectionTargets {$script:paths}
        function script:Read-XonarRegistrySddl {param($Subkey) $script:descriptors[$Subkey]}
        function script:Test-XonarRegistryKeyExists {param($Subkey) $script:descriptors.ContainsKey($Subkey)}
        function script:Write-XonarRegistryDacl {param($Subkey,$Sddl)
            Assert-XonarProtectionSubkey $Subkey
            ++$script:writes
            if($script:writes -eq $script:failAt){throw 'Injected ACL failure'}
            $script:descriptors[$Subkey]=$Sddl
        }
        $install=Join-Path $testRoot 'Install'
        Protect-XonarApoEffects -InstallDir $install -Preview
        if($script:writes -ne 0 -or (Test-Path -LiteralPath $install)){throw 'Preview modified state'}
        Write-Host 'PASS : preview without mutation'
        Protect-XonarApoEffects -InstallDir $install
        if(-not(Test-XonarApoEffectsProtected) -or $script:writes -ne 3){throw 'Protection missing'}
        $state=Join-Path $install 'State\apo-effects-protection.clixml'
        $hash=(Get-FileHash -LiteralPath $state).Hash
        Protect-XonarApoEffects -InstallDir $install
        if($script:writes -ne 3 -or (Get-FileHash -LiteralPath $state).Hash -ne $hash){throw 'Second protection changed baseline'}
        Write-Host 'PASS : idempotence and original baseline preserved'
        $acl=[Security.AccessControl.RegistrySecurity]::new();$acl.SetSecurityDescriptorSddlForm($script:descriptors[$script:paths[0]])
        $rules=@($acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier]))
        foreach($sid in 'S-1-5-18','S-1-5-19','S-1-5-20'){
            $rule=@($rules|Where-Object {$_.IdentityReference.Value -eq $sid -and $_.AccessControlType -eq 'Deny'})
            if($rule.Count -ne 1 -or ($rule[0].RegistryRights -band [Security.AccessControl.RegistryRights]::Delete) -eq 0 -or ($rule[0].RegistryRights -band [Security.AccessControl.RegistryRights]::SetValue) -eq 0){throw 'SYSTEM/service write or delete not denied'}
        }
        Write-Host 'PASS : explicit write/delete/change-permissions denial for SYSTEM and services'
        Restore-XonarApoEffectsProtection -InstallDir $install
        foreach($value in $script:descriptors.Values){if($value -ne $original){throw 'Original ACL not restored'}}
        Write-Host 'PASS : exact original permissions restored'
        $script:failAt=$script:writes+2
        $failed=$false
        try{Protect-XonarApoEffects -InstallDir $install}catch{$failed=$true}
        if(-not $failed){throw 'Injected failure hidden'}
        foreach($value in $script:descriptors.Values){if($value -ne $original){throw 'Partial ACL lock left after failure'}}
        Write-Host 'PASS : failure halfway restores every ACL'
        $failed=$false
        try{Assert-XonarProtectionSubkey 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\{00000000-0000-0000-0000-000000000000}\Properties'}catch{$failed=$true}
        if(-not $failed){throw 'Unrelated registry path accepted'}
        Write-Host 'PASS : unrelated registry keys rejected'
        $script:descriptors.Remove($script:paths[2])
        Restore-XonarApoEffectsProtection -InstallDir $install
        if($script:descriptors.ContainsKey($script:paths[2])){throw 'Removed device key recreated during restoration'}
        Write-Host 'PASS : removed endpoint/interface keys skipped during uninstall'
        Write-Host '7 effects protection scenarios passed.'
    } $testRoot
}finally{
    $absolute=[IO.Path]::GetFullPath($testRoot);$prefix=[IO.Path]::GetFullPath($env:TEMP).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($absolute) -notlike 'xonar-protection-tests-*'){throw 'Unsafe cleanup'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
