# SPDX-License-Identifier: Apache-2.0
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '..\apo\ApoRegistry.psm1') -Force
$script:BlockedRights=[Security.AccessControl.RegistryRights]'SetValue,CreateSubKey,CreateLink,Delete,ChangePermissions,TakeOwnership'
$script:BlockedSids=@('S-1-5-18','S-1-5-19','S-1-5-20')
function Initialize-XonarRegistrySecurity {
    if(-not('XonarRegistrySecurity' -as [type])){Add-Type -Path (Join-Path $PSScriptRoot 'ApoRegistrySecurity.cs')}
}
function Read-XonarRegistrySddl {
    param([string]$Subkey)
    Initialize-XonarRegistrySecurity
    $descriptor=[Security.AccessControl.RawSecurityDescriptor]::new([XonarRegistrySecurity]::Read($Subkey),0)
    $descriptor.GetSddlForm([Security.AccessControl.AccessControlSections]'Access,Owner,Group')
}
function Write-XonarRegistryDacl {
    param([string]$Subkey,[string]$Sddl)
    Assert-XonarProtectionSubkey $Subkey
    Initialize-XonarRegistrySecurity
    [XonarRegistrySecurity]::SetDacl($Subkey,$Sddl)
}
function Assert-XonarProtectionSubkey {
    param([string]$Subkey)
    $endpoint='^SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\\{[0-9a-f-]{36}\}\\FxProperties$'
    $interface='^SYSTEM\\CurrentControlSet\\Control\\DeviceClasses\\\{(6994ad04-93ef-11d0-a3cc-00a0c9223196|dda54a40-1e4c-11d1-a050-405705c10000)\}\\##\?\#USB#VID_0B05&PID_189D&MI_00#[^\\]+\\#TopologySpeaker\\Device Parameters\\FX\\0$'
    if($Subkey -notmatch $endpoint -and $Subkey -notmatch $interface){throw 'La protection est limitee aux effets des haut-parleurs Xonar.'}
    if($Subkey -match $endpoint){
        $parent=$Subkey.Substring(0,$Subkey.LastIndexOf('\'))
        $endpointKey=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($parent)
        if(-not $endpointKey){return}
        $endpointKey.Dispose()
        $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($parent+'\Properties')
        try{if(-not $key -or $key.GetValue('{b3f8fa53-0004-438e-9003-51a46e139bfc},2') -notmatch '(?i)USB\\VID_0B05&PID_189D&MI_00\\' -or $key.GetValue('{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},0') -ne 1){throw 'Endpoint de sauvegarde non Xonar.'}}finally{if($key){$key.Dispose()}}
    }
}
function Get-XonarLockedEffectsSddl {
    param([string]$OriginalSddl)
    $old=[Security.AccessControl.RegistrySecurity]::new();$old.SetSecurityDescriptorSddlForm($OriginalSddl)
    $acl=[Security.AccessControl.RegistrySecurity]::new();$acl.SetAccessRuleProtection($true,$false)
    foreach($sid in $script:BlockedSids){
        $acl.AddAccessRule([Security.AccessControl.RegistryAccessRule]::new([Security.Principal.SecurityIdentifier]::new($sid),$script:BlockedRights,'Deny'))
    }
    $acl.AddAccessRule([Security.AccessControl.RegistryAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'),'FullControl','Allow'))
    # OWNER RIGHTS removes the implicit WRITE_DAC granted to the SYSTEM owner.
    $acl.AddAccessRule([Security.AccessControl.RegistryAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-3-4'),'ReadPermissions','Allow'))
    foreach($rule in $old.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier])){
        if($rule.AccessControlType -ne 'Allow'){continue}
        $read=$rule.RegistryRights -band [Security.AccessControl.RegistryRights]::ReadKey
        if($read -ne 0){$acl.AddAccessRule([Security.AccessControl.RegistryAccessRule]::new($rule.IdentityReference,$read,'Allow'))}
    }
    foreach($sid in 'S-1-5-18','S-1-5-19','S-1-5-20','S-1-5-32-545','S-1-15-2-1'){
        $acl.AddAccessRule([Security.AccessControl.RegistryAccessRule]::new([Security.Principal.SecurityIdentifier]::new($sid),'ReadKey','Allow'))
    }
    $acl.GetSecurityDescriptorSddlForm([Security.AccessControl.AccessControlSections]::Access)
}
function Test-XonarEffectsSddlProtected {
    param([string]$Sddl)
    $acl=[Security.AccessControl.RegistrySecurity]::new();$acl.SetSecurityDescriptorSddlForm($Sddl)
    if(-not $acl.AreAccessRulesProtected){return $false}
    $rules=@($acl.GetAccessRules($true,$false,[Security.Principal.SecurityIdentifier]))
    foreach($sid in $script:BlockedSids){
        $denied=0
        foreach($rule in $rules|Where-Object {$_.IdentityReference.Value -eq $sid -and $_.AccessControlType -eq 'Deny'}){$denied=$denied -bor [int]$rule.RegistryRights}
        if(($denied -band [int]$script:BlockedRights) -ne [int]$script:BlockedRights){return $false}
    }
    @($rules|Where-Object {$_.IdentityReference.Value -eq 'S-1-3-4' -and $_.AccessControlType -eq 'Allow' -and $_.RegistryRights -eq [Security.AccessControl.RegistryRights]::ReadPermissions}).Count -gt 0 -and
    @($rules|Where-Object {$_.IdentityReference.Value -eq 'S-1-5-32-544' -and $_.AccessControlType -eq 'Allow' -and ($_.RegistryRights -band [Security.AccessControl.RegistryRights]::FullControl) -eq [Security.AccessControl.RegistryRights]::FullControl}).Count -gt 0
}
function Get-XonarProtectionStatePath {
    param([string]$InstallDir)
    $directory=[IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
    if($directory -in @([IO.Path]::GetPathRoot($directory).TrimEnd('\'),$env:WINDIR,$env:ProgramFiles,$env:ProgramData,$env:USERPROFILE)){throw 'Dossier de protection trop large.'}
    Join-Path $directory 'State\apo-effects-protection.clixml'
}
function Get-XonarProtectionTargets {
    param([switch]$AllowUnattached)
    $target=Get-XonarSpeakerTarget
    $key=if($AllowUnattached){[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($target.Subkey)}else{Open-ApoEffectsKey $target.Subkey}
    $endpointExists=$null -ne $key
    try{
        if(-not $AllowUnattached -and ($key.GetValue('{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1') -ne '{C243A8CB-D535-4BA0-BCC5-CC4338833E7A}' -or $key.GetValue('{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2') -ne '{D2FBC737-00C3-4482-B3DD-02D92C06003E}')){throw 'Restaurer la paire APO Xonar avant de proteger les effets.'}
    }finally{if($key){$key.Dispose()}}
    if($endpointExists){Assert-XonarProtectionSubkey $target.Subkey;$target.Subkey}
    $instance=($target.Devnode -replace '^\{1\}\.','').Replace('\','#')
    foreach($category in '{6994AD04-93EF-11D0-A3CC-00A0C9223196}','{DDA54A40-1E4C-11D1-A050-405705C10000}'){
        $path="SYSTEM\CurrentControlSet\Control\DeviceClasses\$category\##?#$instance#$category\#TopologySpeaker\Device Parameters\FX\0"
        $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($path)
        if($key){$key.Dispose();Assert-XonarProtectionSubkey $path;$path}
    }
}
function Get-XonarApoProtectionSnapshot {
    param([switch]$AllowUnattached)
    @(foreach($path in Get-XonarProtectionTargets -AllowUnattached:$AllowUnattached){[pscustomobject]@{Subkey=$path;Sddl=(Read-XonarRegistrySddl $path)}})
}
function Test-XonarRegistryKeyExists {
    param([string]$Subkey)
    $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($Subkey)
    if(-not $key){return $false}
    $key.Dispose();return $true
}
function Restore-XonarApoProtectionSnapshot {
    param([object[]]$Snapshot)
    foreach($item in $Snapshot){
        Assert-XonarProtectionSubkey $item.Subkey
        if(-not(Test-XonarRegistryKeyExists $item.Subkey)){continue}
        Write-XonarRegistryDacl $item.Subkey $item.Sddl
    }
}
function Protect-XonarApoEffects {
    [CmdletBinding()]
    param([string]$InstallDir='C:\Program Files\XonarSE',[switch]$Preview)
    $statePath=Get-XonarProtectionStatePath $InstallDir
    $before=@(Get-XonarApoProtectionSnapshot)
    if($Preview){Write-Host "PLAN : protection de $($before.Count) cles d'effets Xonar ; ecriture/suppression SYSTEM et services refusees ; administrateurs conserves.";return}
    $saved=@()
    $saveNeeded=-not(Test-Path -LiteralPath $statePath)
    if(Test-Path -LiteralPath $statePath){
        $state=Import-Clixml -LiteralPath $statePath
        if($state.Version -ne 1){throw 'Version de sauvegarde de protection inconnue.'}
        $saved=@($state.Keys)
        foreach($item in $saved){Assert-XonarProtectionSubkey $item.Subkey}
    }
    foreach($item in $before){if($item.Subkey -notin @($saved|ForEach-Object {$_.Subkey})){$saved+=,$item;$saveNeeded=$true}}
    if($saveNeeded){
        New-Item -ItemType Directory -Path (Split-Path -Parent $statePath) -Force|Out-Null
        [pscustomobject]@{Version=1;Keys=$saved}|Export-Clixml -LiteralPath ($statePath+'.tmp') -Encoding utf8
        Move-Item -LiteralPath ($statePath+'.tmp') -Destination $statePath -Force
    }
    try{
        foreach($item in $before){
            if(Test-XonarEffectsSddlProtected $item.Sddl){continue}
            Write-XonarRegistryDacl $item.Subkey (Get-XonarLockedEffectsSddl $item.Sddl)
            $actual=Read-XonarRegistrySddl $item.Subkey
            if(-not(Test-XonarEffectsSddlProtected $actual)){throw ('Protection non confirmee apres relecture : '+$actual)}
        }
    }catch{Restore-XonarApoProtectionSnapshot $before;throw}
    Write-Host "Effets Xonar proteges ($($before.Count) cles). Permissions originales : $statePath"
}
function Restore-XonarApoEffectsProtection {
    [CmdletBinding()]
    param([string]$InstallDir='C:\Program Files\XonarSE',[switch]$Preview)
    $path=Get-XonarProtectionStatePath $InstallDir
    if(-not(Test-Path -LiteralPath $path)){return}
    $state=Import-Clixml -LiteralPath $path
    if($state.Version -ne 1){throw 'Version de sauvegarde de protection inconnue.'}
    foreach($item in $state.Keys){Assert-XonarProtectionSubkey $item.Subkey}
    if($Preview){Write-Host 'PLAN : restaurer les permissions originales des effets Xonar.';return}
    Restore-XonarApoProtectionSnapshot @($state.Keys)
    Write-Host 'Permissions originales des effets Xonar restaurees.'
}
function Test-XonarApoEffectsProtected {
    try{
        $items=@(Get-XonarApoProtectionSnapshot)
        $items.Count -gt 0 -and @($items|Where-Object {-not(Test-XonarEffectsSddlProtected $_.Sddl)}).Count -eq 0
    }catch{return $false}
}
function Test-XonarApoProtectionSnapshotProtected {
    param([object[]]$Snapshot)
    try{return $Snapshot.Count -gt 0 -and @($Snapshot|Where-Object {-not(Test-XonarEffectsSddlProtected $_.Sddl)}).Count -eq 0}catch{return $false}
}
function Get-XonarEffectsEndpointKey {
    foreach($attempt in 0..20){
        try{return (Get-XonarSpeakerTarget).Subkey}catch{if($attempt -eq 20){throw};Start-Sleep -Milliseconds 100}
    }
}
function Test-XonarEffectsEndpointStable {
    param([string]$EndpointKey)
    # EndpointBuilder may publish a new GUID only after it restarts. Require a
    # full second of the same protected endpoint, rather than the pre-restart GUID.
    foreach($sample in 1..5){
        Start-Sleep -Milliseconds 200
        try{if((Get-XonarEffectsEndpointKey) -ne $EndpointKey -or -not(Test-XonarApoEffectsProtected)){return $false}}catch{return $false}
    }
    return $true
}
function Complete-XonarApoEffectsProtection {
    [CmdletBinding()]
    param([string]$InstallDir='C:\Program Files\XonarSE')
    $audioRunning=(Get-Service Audiosrv).Status -eq 'Running'
    $builderRunning=(Get-Service AudioEndpointBuilder).Status -eq 'Running'
    try{
        foreach($attempt in 1..4){
            $key=Get-XonarEffectsEndpointKey
            if((Test-XonarApoEffectsProtected) -and (Test-XonarEffectsEndpointStable $key)){return}
            if($audioRunning){Stop-Service Audiosrv -Force}
            if($builderRunning){Stop-Service AudioEndpointBuilder -Force}
            Protect-XonarApoEffects -InstallDir $InstallDir
            if($builderRunning){Start-Service AudioEndpointBuilder}
            if($audioRunning){Start-Service Audiosrv}
            $key=Get-XonarEffectsEndpointKey
            if(Test-XonarEffectsEndpointStable $key){return}
        }
        throw 'La sortie Xonar ne reste pas protegee apres reconstruction. Installation non validee.'
    }finally{
        if($builderRunning){Start-Service AudioEndpointBuilder}
        if($audioRunning){Start-Service Audiosrv}
    }
}
Export-ModuleMember -Function Protect-XonarApoEffects,Complete-XonarApoEffectsProtection,Restore-XonarApoEffectsProtection,Get-XonarApoProtectionSnapshot,Restore-XonarApoProtectionSnapshot,Test-XonarApoProtectionSnapshotProtected,Test-XonarApoEffectsProtected
