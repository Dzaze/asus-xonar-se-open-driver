#requires -Version 7.0
# Fichiers reels temporaires, dependances privilegiees injectees : aucun pilote ou certificat reel change.
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Import-Module (Join-Path $repo 'scripts\install\XonarProduct.psm1') -Force
$testRoot=Join-Path $env:TEMP ('xonar-product-tests-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot|Out-Null
try {
    & (Get-Module XonarProduct) {
        param($testRoot)
        function script:Assert-XonarProductAdministrator {}
        function script:Test-XonarProductPrerequisites {[pscustomobject]@{InstanceId='unit-device'}}
        function script:Get-XonarProductDriver {param($InstanceId) $script:tx.Driver}
        function script:Get-XonarProductCertificate {param($Path) [pscustomobject]@{Thumbprint=$script:ProductSigner;HasPrivateKey=$false}}
        function script:Get-AuthenticodeSignature {param($LiteralPath) [pscustomobject]@{Status='Valid';SignerCertificate=[pscustomobject]@{Thumbprint=$script:ProductSigner}}}
        function script:Test-Path {param($LiteralPath,$Path)
            $value=if($LiteralPath){$LiteralPath}else{$Path}
            if($value -like 'Cert:*'){return $true}
            Microsoft.PowerShell.Management\Test-Path -LiteralPath $value
        }
        function script:Install-XonarProductRuntime {param($Redist) return $false}
        function script:Get-XonarProductShell {[pscustomobject]@{Value=$script:tx.Shell}}
        function script:Install-XonarProductShell {param($InstallRoot,$Version) $script:tx.Shell='new-shell'}
        function script:Restore-XonarProductShell {param($State) $script:tx.Shell=$State.Value}
        function script:Get-XonarApoProtectionSnapshot {param([switch]$AllowUnattached) [pscustomobject]@{Subkey='unit';Sddl=$script:tx.Protection}}
        function script:Restore-XonarApoEffectsProtection {param($InstallDir) $script:tx.Protection='unprotected'}
        function script:Restore-XonarApoProtectionSnapshot {param($Snapshot) $script:tx.Protection=$Snapshot[0].Sddl}
        function script:Test-XonarApoCurrent {param($Source,$InstallRoot) return $script:tx.Current}
        function script:Install-ApoTransactional {param($SourceDll,$InstallDir,$ConfigDir,$BackupDirectory)
            if($script:tx.FailApo){throw 'Injected APO failure'}
            New-Item -ItemType Directory -Path $BackupDirectory|Out-Null
            [pscustomobject]@{Value=$script:tx.Apo}|Export-Clixml (Join-Path $BackupDirectory 'install-manifest.clixml')
            $script:tx.Apo='new-apo'
        }
        function script:Restore-ApoInstallation {param($BackupDirectory)
            $script:tx.Apo=(Import-Clixml (Join-Path $BackupDirectory 'install-manifest.clixml')).Value
        }
        function script:Get-XonarSpeakerTarget {[pscustomobject]@{EndpointGuid='unit'}}
        function script:Invoke-XonarPnpUtil {param($Arguments)
            if($Arguments[0] -eq '/export-driver'){Set-Content -LiteralPath (Join-Path $Arguments[2] 'previous.inf') -Value 'previous'}
            return 0
        }
        function script:Set-XonarProductDriver {param($Inf,[switch]$AlreadyStaged)
            if($Inf -like '*\Driver\xonarse.inf'){$script:tx.Driver=$script:tx.NewDriver}else{$script:tx.Driver=$script:tx.OldDriver}
            return $false
        }
        function script:Copy-Item {param($LiteralPath,$Destination,[switch]$Recurse,[switch]$Force)
            if($script:tx.FailCopy -and $Destination -eq (Join-Path $script:tx.Root 'App')){
                New-Item -ItemType Directory -Path $Destination -Force|Out-Null
                Set-Content -LiteralPath (Join-Path $Destination 'partial.txt') -Value 'partial'
                throw 'Injected app copy failure'
            }
            Microsoft.PowerShell.Management\Copy-Item @PSBoundParameters
        }
        $passed=0
        foreach($case in 'ExistingNative','NativeInfUpdate','NewNative','ApoFailure','AppCopyFailure','TamperedPayload'){
            $folder=Join-Path $testRoot $case;$package=Join-Path $folder 'Package';$install=Join-Path $folder 'Install';$config=Join-Path $folder 'Config'
            $directories=@('App','Scripts','bin','Driver','Certificates','Defaults','Licenses','Prerequisites')
            foreach($name in $directories){New-Item -ItemType Directory -Path (Join-Path $package $name) -Force|Out-Null;Set-Content -LiteralPath (Join-Path $package ($name+'\marker.txt')) -Value 'package'}
            New-Item -ItemType Directory -Path (Join-Path $install 'App'),$config -Force|Out-Null
            Set-Content -LiteralPath (Join-Path $install 'App\previous.txt') -Value 'previous app'
            Set-Content -LiteralPath (Join-Path $config 'apo-efx.cfg') -Value 'personal settings'
            foreach($name in 'Driver\xonarse.sys','Driver\xonarse.cat','Driver\xonarse.inf','bin\XonarApo.dll','Uninstall-XonarSE.exe','README-INSTALLATION.md','APO-PROTECTION.fr.md','Certificates\XonarSeOpenDriver-TEST.cer','Defaults\apo-efx.cfg'){
                Set-Content -LiteralPath (Join-Path $package $name) -Value 'new package'
            }
            $oldDriver=[pscustomobject]@{InstanceId='unit-device';InfName='oem111.inf';Version='old';Service='previous';RuntimePath=$null}
            $newDriver=[pscustomobject]@{InstanceId='unit-device';InfName='oem222.inf';Version='0.5.0.0';Service='xonarse';RuntimePath=(Join-Path $package 'Driver\xonarse.sys')}
            if($case -eq 'ExistingNative'){$oldDriver=$newDriver}
            if($case -eq 'NativeInfUpdate'){
                $oldDriver=[pscustomobject]@{InstanceId='unit-device';InfName='oem111.inf';Version='0.4.0.0';Service='xonarse';RuntimePath=$newDriver.RuntimePath}
            }
            $script:tx=@{Driver=$oldDriver;OldDriver=$oldDriver;NewDriver=$newDriver;Current=$case -eq 'ExistingNative';Protection='previous-permissions';Apo='previous-apo';Shell='previous-shell';FailApo=$case -eq 'ApoFailure';FailCopy=$case -eq 'AppCopyFailure';Root=$install}
            $entries=@(Get-ChildItem -LiteralPath $package -Recurse -File|ForEach-Object {[pscustomobject]@{Path=$_.FullName.Substring($package.Length).TrimStart('\');Sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}})
            [pscustomobject]@{Schema=1;HardwareId=$script:ProductHardwareId;SignerThumbprint=$script:ProductSigner;Version='test';DriverVersion='0.5.0.0';Files=$entries}|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $package 'manifest.json')
            $hash=(Get-FileHash -LiteralPath (Join-Path $package 'manifest.json')).Hash
            if($case -eq 'TamperedPayload'){Set-Content -LiteralPath (Join-Path $package 'Driver\xonarse.sys') -Value 'tampered'}
            $failure=$null;$result=$null
            try{$result=Install-XonarProduct -PackageRoot $package -ManifestHash $hash -InstallRoot $install -ConfigRoot $config}catch{$failure=$_}
            if($case -in 'ExistingNative','NativeInfUpdate','NewNative'){
                if($failure){throw $failure}
                if(-not(Test-Path -LiteralPath (Join-Path $install 'App\marker.txt'))){throw 'New app missing'}
                if($case -eq 'ExistingNative' -and ($result.DriverChanged -or $result.ApoChanged)){throw 'Existing native stack changed'}
                if($case -eq 'NativeInfUpdate' -and -not $result.DriverChanged){throw 'New INF version skipped because SYS bytes are unchanged'}
                Restore-XonarProduct -BackupDirectory $result.BackupDirectory
            }elseif(-not $failure){throw "Expected failure missing : $case"}
            if(-not(Test-Path -LiteralPath (Join-Path $install 'App\previous.txt'))){throw "Previous app lost : $case"}
            if(Test-Path -LiteralPath (Join-Path $install 'App\partial.txt')){throw "Partial files left : $case"}
            if((Get-Content -LiteralPath (Join-Path $config 'apo-efx.cfg') -Raw).Trim() -ne 'personal settings'){throw 'Personal config changed'}
            if($script:tx.Driver.InfName -ne $oldDriver.InfName -or $script:tx.Apo -ne 'previous-apo' -or $script:tx.Shell -ne 'previous-shell'){throw "Native/shell state not restored : $case"}
            if($script:tx.Protection -ne 'previous-permissions'){throw "Effects permissions not restored : $case"}
            ++$passed;Write-Host "PASS : $case"
        }
        Write-Host "$passed product installer scenarios passed."
    } $testRoot
}finally{
    $absolute=[IO.Path]::GetFullPath($testRoot);$prefix=[IO.Path]::GetFullPath($env:TEMP).TrimEnd('\')+'\'
    if(-not $absolute.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($absolute) -notlike 'xonar-product-tests-*'){throw 'Unsafe cleanup'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
