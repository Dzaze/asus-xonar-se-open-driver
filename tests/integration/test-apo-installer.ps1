#requires -Version 7.0
# Tests isoles : fichiers temporaires, services/COM/registre/signature remplaces dans le module.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Import-Module (Join-Path $repo 'scripts\install\ApoInstallTransaction.psm1') -Force
$testRoot=Join-Path $env:TEMP ('xonar-installer-tests-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot|Out-Null
try {
    & (Get-Module ApoInstallTransaction) {
        param($testRoot)
        function script:Get-AuthenticodeSignature {param($LiteralPath)
            [pscustomobject]@{Status=if($script:tx.InvalidSignature){'NotTrusted'}else{'Valid'};SignerCertificate=[pscustomobject]@{Thumbprint='unit-signer'}}
        }
        function script:Get-XonarSpeakerTarget { [pscustomobject]@{EndpointGuid='{00000000-0000-0000-0000-000000000001}';Devnode='unit-hardware'} }
        function script:Get-XonarDriverState { [pscustomobject]@{DriverVersion='unchanged';InfName='unit.inf'} }
        function script:Assert-ApoComDestination {param($Target)}
        function script:Get-XonarApoProtectionSnapshot {param([switch]$AllowUnattached) [pscustomobject]@{Subkey='unit';Sddl=$script:tx.Protection}}
        function script:Protect-XonarApoEffects {param($InstallDir) $script:tx.Protection='protected'}
        function script:Complete-XonarApoEffectsProtection {param($InstallDir) if($script:tx.Protection -ne 'protected'){throw 'Protection not applied'}}
        function script:Restore-XonarApoEffectsProtection {param($InstallDir) $script:tx.Protection='unprotected'}
        function script:Restore-XonarApoProtectionSnapshot {param($Snapshot) $script:tx.Protection=$Snapshot[0].Sddl}
        function script:Read-ApoRegistryTree {param($Path)
            [pscustomobject]@{Path=$Path;Exists=$true;Values=@([pscustomobject]@{Name='';Kind='String';Value=$script:tx.Com});Children=@()}
        }
        function script:Restore-ApoCom {param($Trees)
            if($script:tx.FailRollback){throw 'Simulated rollback failure'}
            $script:tx.Com=$Trees[0].Values[0].Value
        }
        function script:Invoke-ApoAttachment {param($BackupDirectory,[switch]$Preview)
            if($Preview){return}
            [pscustomobject]@{Original=$script:tx.Fx}|Export-Clixml (Join-Path $BackupDirectory 'fx-unit.clixml')
            $script:tx.Fx='new-effects'
        }
        function script:Restore-ApoSnapshot {param($Path)
            $script:tx.Fx=(Import-Clixml -LiteralPath $Path).Original
        }
        function script:Stop-ApoAudio {$script:tx.Service='Stopped';++$script:tx.StopCount}
        function script:Start-Service {param($Name)
            if($Name -eq 'AudioEndpointBuilder'){$script:tx.Builder='Running';return}
            if($script:tx.FailStart -and -not $script:tx.StartFailureSeen){$script:tx.StartFailureSeen=$true;throw 'Simulated startup failure'}
            $script:tx.Service='Running'
        }
        function script:Get-Service {param($Name)
            $service=[pscustomobject]@{Status=if($Name -eq 'AudioEndpointBuilder'){$script:tx.Builder}else{$script:tx.Service}}
            $service|Add-Member -MemberType ScriptMethod -Name WaitForStatus -Value {param($state,$timeout)}
            $service
        }
        function script:Start-Process {param($FilePath,$ArgumentList,[switch]$PassThru,[switch]$Wait,$WindowStyle)
            $script:tx.Com='new-com'
            [pscustomobject]@{ExitCode=if($script:tx.FailRegistration){1}else{0}}
        }
        function script:Set-Acl {param($LiteralPath,$AclObject)}
        function script:Stop-Service {param($Name,[switch]$Force) if($Name -ne 'AudioEndpointBuilder'){throw 'Unexpected service mutation'};$script:tx.Builder='Stopped'}
        $env:XONAR_INSTALL_TEST='1'
        $passed=0
        foreach($case in 'AfterCopy','AfterRegistration','AfterAttachment','AfterProtection','StartupFailure','RegistrationFailure','SuccessAndRestore','BadSignature','RollbackFailure'){
            $script:tx=@{Com='old-com';Fx='old-effects';Protection='previous-permissions';Builder='Running';Service='Running';StopCount=0;InvalidSignature=$case -eq 'BadSignature';FailStart=$case -eq 'StartupFailure';StartFailureSeen=$false;FailRegistration=$case -eq 'RegistrationFailure';FailRollback=$case -eq 'RollbackFailure'}
            $folder=Join-Path $testRoot $case
            $install=Join-Path $folder 'Install';$config=Join-Path $folder 'Config';$backup=Join-Path $folder 'Backup'
            New-Item -ItemType Directory -Path $install,$config -Force|Out-Null
            $dll=Join-Path $install 'XonarApo.dll';$source=Join-Path $folder 'candidate.dll';$cfg=Join-Path $config 'apo-efx.cfg'
            Set-Content -LiteralPath $dll -Value 'previous DLL';Set-Content -LiteralPath $source -Value 'different candidate DLL';Set-Content -LiteralPath $cfg -Value 'personal profile'
            $before=(Get-FileHash -LiteralPath $dll).Hash
            $failure=if($case -in 'AfterCopy','AfterRegistration','AfterAttachment','AfterProtection'){$case}elseif($case -eq 'RollbackFailure'){'AfterAttachment'}else{'None'}
            $failed=$false;$message=''
            try {Install-ApoTransactional -SourceDll $source -InstallDir $install -ConfigDir $config -BackupDirectory $backup -TrustedSignerThumbprint 'unit-signer' -FailurePoint $failure|Out-Null}
            catch{$failed=$true;$message=$_.Exception.Message}
            if($case -eq 'SuccessAndRestore'){
                if($failed -or (Get-FileHash -LiteralPath $dll).Hash -eq $before){throw "Success failed : $message"}
                Restore-ApoInstallation -BackupDirectory $backup
            }elseif(-not $failed){throw "Failure not reported : $case"}
            if($case -eq 'RollbackFailure'){
                if($message -notmatch 'ROLLBACK INCOMPLET'){throw 'Rollback failure hidden'}
                $script:tx.FailRollback=$false
                Restore-ApoInstallation -BackupDirectory $backup
            }
            if((Get-FileHash -LiteralPath $dll).Hash -ne $before){throw "Wrong binary after $case"}
            if($script:tx.Com -ne 'old-com' -or $script:tx.Fx -ne 'old-effects' -or $script:tx.Service -ne 'Running'){throw "State not restored : $case"}
            if($script:tx.Protection -ne 'previous-permissions' -or $script:tx.Builder -ne 'Running'){throw "Effects permissions or endpoint service not restored : $case"}
            if((Get-Content -LiteralPath $cfg -Raw).Trim() -ne 'personal profile'){throw "Profile changed : $case"}
            if($case -eq 'BadSignature' -and $script:tx.StopCount -ne 0){throw 'Signature refusal modified service'}
            ++$passed
            Write-Host "PASS : $case (different binary restored; profile, COM, effects, service checked)"
        }
        Write-Host "$passed installer tests passed."
    } $testRoot
}finally{
    $absolute=[IO.Path]::GetFullPath($testRoot)
    $temp=[IO.Path]::GetFullPath($env:TEMP).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($temp,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($absolute) -notlike 'xonar-installer-tests-*'){throw 'Unsafe cleanup path'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
