# SPDX-License-Identifier: Apache-2.0
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '..\apo\ApoRegistry.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'ApoEffectsProtection.psm1') -Force

function Get-ApoComRoots {
    foreach($id in '{C243A8CB-D535-4BA0-BCC5-CC4338833E7A}','{D2FBC737-00C3-4482-B3DD-02D92C06003E}') {
        "SOFTWARE\Classes\CLSID\$id"
        "SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$id"
    }
}
function Read-ApoRegistryTree {
    param([string]$Path)
    $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($Path)
    if(-not $key){return [pscustomobject]@{Path=$Path;Exists=$false;Values=@();Children=@()}}
    try {
        $values=@(foreach($name in $key.GetValueNames()){
            [pscustomobject]@{Name=$name;Kind=$key.GetValueKind($name).ToString();Value=$key.GetValue($name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}
        })
        $children=@(foreach($child in $key.GetSubKeyNames()){Read-ApoRegistryTree -Path ($Path+'\'+$child)})
        [pscustomobject]@{Path=$Path;Exists=$true;Values=$values;Children=$children}
    }finally{$key.Dispose()}
}
function Write-ApoRegistryTree {
    param($Node)
    if(-not $Node.Exists){return}
    $key=[Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($Node.Path)
    try {
        foreach($v in $Node.Values){
            $kind=[Microsoft.Win32.RegistryValueKind]$v.Kind
            $value=$v.Value
            if($kind -eq 'MultiString'){$value=[string[]]$value}
            if($kind -eq 'Binary'){$value=[byte[]]$value}
            if($kind -eq 'DWord'){$value=[int]$value}
            $key.SetValue($v.Name,$value,$kind)
        }
    }finally{$key.Dispose()}
    foreach($child in $Node.Children){Write-ApoRegistryTree $child}
}
function Restore-ApoCom {
    param($Trees)
    $allowed=@(Get-ApoComRoots)
    foreach($tree in $Trees){
        if($tree.Path -notin $allowed){throw 'Racine COM de sauvegarde inconnue.'}
        [Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree($tree.Path,$false)
        Write-ApoRegistryTree $tree
    }
}
function Assert-ApoDestination {
    param([string]$InstallDir,[string]$ConfigDir)
    foreach($directory in $InstallDir,$ConfigDir){
        $full=[IO.Path]::GetFullPath($directory).TrimEnd('\')
        $forbidden=@([IO.Path]::GetPathRoot($full).TrimEnd('\'),$env:WINDIR,$env:ProgramFiles,$env:ProgramData,$env:USERPROFILE)
        if($full -in $forbidden){throw "Dossier trop large : $full"}
    }
}
function Stop-ApoAudio {
    Stop-Service -Name Audiosrv -Force
    (Get-Service Audiosrv).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(15))
}
function Copy-ApoBinary {
    param([string]$Source,[string]$Destination)
    for($attempt=0;$attempt -lt 5;$attempt++){
        try {Copy-Item -LiteralPath $Source -Destination $Destination -Force;return}
        catch {if($attempt -eq 4){throw};Start-Sleep -Milliseconds 200}
    }
}
function Save-ApoManifest {
    param($Manifest,[string]$Directory)
    $Manifest | Export-Clixml -LiteralPath (Join-Path $Directory 'install-manifest.clixml') -Encoding utf8
}
function Invoke-ApoAttachment {
    param([string]$BackupDirectory,[switch]$Preview)
    $script=Join-Path $PSScriptRoot '..\apo\register-apo.ps1'
    if($Preview){& $script -Attachment LfxGfx -Stage Both -DryRun}
    else{& $script -Attachment LfxGfx -Stage Both -BackupDirectory $BackupDirectory}
}
function Assert-ApoComDestination {
    param([string]$Target)
    foreach($root in @(Get-ApoComRoots | Where-Object {$_ -like '*\CLSID\*'})){
        $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($root+'\InprocServer32')
        try{if($key -and $key.GetValue('') -ne $Target){throw "CLSID deja enregistre sur une autre DLL : $root"}}
        finally{if($key){$key.Dispose()}}
    }
}
function Restore-ApoInstallation {
    [CmdletBinding()]
    param([Parameter(Mandatory)][string]$BackupDirectory,[switch]$AudioAlreadyStopped)
    $manifest=Import-Clixml -LiteralPath (Join-Path $BackupDirectory 'install-manifest.clixml')
    if($manifest.Version -ne 2){throw 'Version de sauvegarde inconnue.'}
    Assert-ApoDestination -InstallDir $manifest.InstallDir -ConfigDir $manifest.ConfigDir
    $target=Join-Path $manifest.InstallDir 'XonarApo.dll'
    if($target -ne $manifest.Target){throw 'Destination de sauvegarde incoherente.'}
    $before=Join-Path $BackupDirectory 'XonarApo-before.dll'
    if($manifest.DllExisted -and ((Get-FileHash -LiteralPath $before).Hash -ne $manifest.BeforeHash)){throw 'Sauvegarde DLL alteree.'}
    $running=(Get-Service Audiosrv).Status -eq 'Running'
    try {
        if(-not $AudioAlreadyStopped){Stop-ApoAudio}
        Restore-XonarApoEffectsProtection -InstallDir $manifest.InstallDir
        if($manifest.DllExisted){Copy-ApoBinary $before $target}
        elseif(Test-Path -LiteralPath $target){Remove-Item -LiteralPath $target -Force}
        Restore-ApoCom $manifest.ComTrees
        foreach($snapshot in @(Get-ChildItem -LiteralPath $BackupDirectory -Filter 'fx-*.clixml')){
            Restore-ApoSnapshot -Path $snapshot.FullName
        }
        if($manifest.PSObject.Properties.Name -contains 'ProtectionBefore'){
            Restore-XonarApoProtectionSnapshot -Snapshot @($manifest.ProtectionBefore)
        }
        foreach($item in $manifest.Acls){
            if($item.Sddl -and (Test-Path -LiteralPath $item.Path)){
                $acl=Get-Acl -LiteralPath $item.Path
                $acl.SetSecurityDescriptorSddlForm($item.Sddl)
                Set-Acl -LiteralPath $item.Path -AclObject $acl
            }
        }
        if($manifest.DllExisted -and ((Get-FileHash -LiteralPath $target).Hash -ne $manifest.BeforeHash)){throw 'Restauration DLL non conforme.'}
        $manifest.Outcome='Restored';Save-ApoManifest $manifest $BackupDirectory
        Write-Host "DLL et associations restaurees : $BackupDirectory"
    }finally{if(-not $AudioAlreadyStopped -and $running){Start-Service Audiosrv}}
}
function Install-ApoTransactional {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$SourceDll,
        [string]$InstallDir='C:\Program Files\XonarSE',
        [string]$ConfigDir='C:\ProgramData\XonarSE',
        [string]$BackupDirectory,
        [string]$TrustedSignerThumbprint='FF68A5C11C614F5E45AD64594D5A978BD9E5D321',
        [ValidateSet('None','AfterCopy','AfterRegistration','AfterAttachment','AfterProtection')][string]$FailurePoint='None',
        [switch]$Preview
    )
    Assert-ApoDestination $InstallDir $ConfigDir
    $InstallDir=[IO.Path]::GetFullPath($InstallDir)
    $ConfigDir=[IO.Path]::GetFullPath($ConfigDir)
    $SourceDll=(Resolve-Path -LiteralPath $SourceDll).Path
    $target=Join-Path $InstallDir 'XonarApo.dll'
    if($SourceDll -eq $target){throw 'La DLL source doit etre distincte de la destination.'}
    $signature=Get-AuthenticodeSignature -LiteralPath $SourceDll
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne $TrustedSignerThumbprint){
        throw 'Signature DLL invalide ou signataire different de celui attendu. Signer la build avant installation.'
    }
    $sourceHash=(Get-FileHash -LiteralPath $SourceDll).Hash
    $hardware=Get-XonarSpeakerTarget
    $driver=Get-XonarDriverState
    Assert-ApoComDestination $target
    # Preflight : refuse les effets tiers avant toute modification de DLL/COM.
    Invoke-ApoAttachment -Preview
    if($Preview){
        Write-Host "PLAN : sauvegarde DLL + COM + FxProperties + ACL ; arret Audiosrv ; copie signee ; COM ; LFX/GFX ; redemarrage ; rollback automatique sur echec."
        Write-Host "Cible : $($hardware.EndpointGuid) ; pilote conserve : $($driver.DriverVersion) / $($driver.InfName)"
        return
    }
    if($FailurePoint -ne 'None' -and $env:XONAR_INSTALL_TEST -ne '1'){throw 'Injection de panne reservee au banc XONAR_INSTALL_TEST=1.'}
    if(-not $BackupDirectory){$BackupDirectory=Join-Path $InstallDir ('Backups\'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N'))}
    if(Test-Path -LiteralPath (Join-Path $BackupDirectory 'install-manifest.clixml')){throw 'Dossier de sauvegarde deja utilise.'}
    New-Item -ItemType Directory -Path $InstallDir,$BackupDirectory -Force|Out-Null
    $BackupDirectory=[IO.Path]::GetFullPath($BackupDirectory)
    $existed=Test-Path -LiteralPath $target
    if($existed){Copy-Item -LiteralPath $target -Destination (Join-Path $BackupDirectory 'XonarApo-before.dll')}
    $acls=@(foreach($path in @($ConfigDir,(Join-Path $ConfigDir 'apo-log.txt'),(Join-Path $ConfigDir 'apo-live-status.json'))){
        [pscustomobject]@{Path=$path;Sddl=if(Test-Path -LiteralPath $path){(Get-Acl -LiteralPath $path).Sddl}else{$null}}
    })
    $manifest=[pscustomobject]@{
        Version=2;Target=$target;InstallDir=$InstallDir;ConfigDir=$ConfigDir;DllExisted=$existed;
        BeforeHash=if($existed){(Get-FileHash -LiteralPath $target).Hash}else{$null};
        SourceHash=$sourceHash;ComTrees=@(foreach($root in Get-ApoComRoots){Read-ApoRegistryTree $root});
        Acls=$acls;EndpointGuid=$hardware.EndpointGuid;Devnode=$hardware.Devnode;Outcome='Prepared';
        ProtectionBefore=@(Get-XonarApoProtectionSnapshot -AllowUnattached)
    }
    Save-ApoManifest $manifest $BackupDirectory
    $running=(Get-Service Audiosrv).Status -eq 'Running'
    $changesStarted=$false
    try {
        Stop-ApoAudio
        $changesStarted=$true
        Copy-ApoBinary $SourceDll $target
        if((Get-FileHash -LiteralPath $target).Hash -ne $sourceHash){throw 'Copie DLL non conforme.'}
        if($FailurePoint -eq 'AfterCopy'){throw 'Panne injectee apres copie.'}
        $regsvr=Join-Path $env:WINDIR 'System32\regsvr32.exe'
        $process=Start-Process $regsvr -ArgumentList @('/s',('"'+$target+'"')) -PassThru -Wait -WindowStyle Hidden
        if($process.ExitCode -ne 0){throw "Enregistrement COM echoue : $($process.ExitCode)"}
        if($FailurePoint -eq 'AfterRegistration'){throw 'Panne injectee apres COM.'}
        New-Item -ItemType Directory -Path $ConfigDir -Force|Out-Null
        $acl=Get-Acl -LiteralPath $ConfigDir
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-5-32-545'),'Modify','ContainerInherit,ObjectInherit','None','Allow'))
        $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-5-19'),'ReadAndExecute','ContainerInherit,ObjectInherit','None','Allow'))
        Set-Acl -LiteralPath $ConfigDir -AclObject $acl
        foreach($name in 'apo-log.txt','apo-live-status.json'){
            $path=Join-Path $ConfigDir $name
            if(-not(Test-Path -LiteralPath $path)){New-Item -ItemType File -Path $path|Out-Null}
            $acl=Get-Acl -LiteralPath $path
            $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-5-19'),'Modify','Allow'))
            Set-Acl -LiteralPath $path -AclObject $acl
        }
        Invoke-ApoAttachment -BackupDirectory $BackupDirectory
        if($FailurePoint -eq 'AfterAttachment'){throw 'Panne injectee apres rattachement.'}
        $builderRunning=(Get-Service AudioEndpointBuilder).Status -eq 'Running'
        try{
            if($builderRunning){Stop-Service AudioEndpointBuilder -Force}
            Protect-XonarApoEffects -InstallDir $InstallDir
            if($FailurePoint -eq 'AfterProtection'){throw 'Panne injectee apres protection.'}
        }finally{if($builderRunning){Start-Service AudioEndpointBuilder}}
        $afterDriver=Get-XonarDriverState
        if($afterDriver.InfName -ne $driver.InfName -or $afterDriver.DriverVersion -ne $driver.DriverVersion){throw 'Le pilote a change pendant le deploiement.'}
        if($running){Start-Service Audiosrv;(Get-Service Audiosrv).WaitForStatus('Running',[TimeSpan]::FromSeconds(15))}
        Complete-XonarApoEffectsProtection -InstallDir $InstallDir
        $manifest.Outcome='Installed'
        Save-ApoManifest $manifest $BackupDirectory
        Write-Host "Installation validee. Sauvegarde : $BackupDirectory"
        [pscustomobject]@{BackupDirectory=$BackupDirectory;Target=$target;Hash=$sourceHash}
    }catch{
        $original=$_
        if(-not $changesStarted){$manifest.Outcome='FailedWithoutChanges';Save-ApoManifest $manifest $BackupDirectory;throw}
        $manifest.Outcome='RollbackRequired';Save-ApoManifest $manifest $BackupDirectory
        try {
            Stop-ApoAudio
            Restore-ApoInstallation -BackupDirectory $BackupDirectory -AudioAlreadyStopped
            if($running){Start-Service Audiosrv;(Get-Service Audiosrv).WaitForStatus('Running',[TimeSpan]::FromSeconds(15))}
            $manifest.Outcome='RolledBack';Save-ApoManifest $manifest $BackupDirectory
        }catch{throw "Installation : $($original.Exception.Message). ROLLBACK INCOMPLET : $($_.Exception.Message). Sauvegarde : $BackupDirectory"}
        throw "Installation annulee et restauree : $($original.Exception.Message). Sauvegarde : $BackupDirectory"
    }finally{if($running){Start-Service Audiosrv}}
}
Export-ModuleMember -Function Install-ApoTransactional,Restore-ApoInstallation
