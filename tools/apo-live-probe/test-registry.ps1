# Test de l'accès minimal et de la restauration typée ; HKCU éphémère uniquement.
$ErrorActionPreference='Stop'
$root=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Import-Module (Join-Path $root 'scripts\apo\ApoRegistry.psm1') -Force
$path='Software\XonarSE-ApoRegistry-Test-'+[Guid]::NewGuid().ToString('N')
$key=[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey($path)
try {
    $values=@(
        [pscustomobject]@{Name='String';Exists=$true;Kind='String';Value='test'},
        [pscustomobject]@{Name='Multi';Exists=$true;Kind='MultiString';Value=[string[]]@('a','b')},
        [pscustomobject]@{Name='OneMulti';Exists=$true;Kind='MultiString';Value=[string[]]@('a')},
        [pscustomobject]@{Name='Binary';Exists=$true;Kind='Binary';Value=[byte[]]@(0,1,255)},
        [pscustomobject]@{Name='Dword';Exists=$true;Kind='DWord';Value=42}
    )
    $acl=$key.GetAccessControl()
    $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User
    $acl.SetAccessRuleProtection($true,$false)
    $acl.AddAccessRule([Security.AccessControl.RegistryAccessRule]::new($sid,[Security.AccessControl.RegistryRights]'ReadKey,SetValue,ChangePermissions',[Security.AccessControl.AccessControlType]::Allow))
    $key.SetAccessControl($acl)
    $restricted=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($path,[Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,[System.Security.AccessControl.RegistryRights]'SetValue,QueryValues')
    try {
        foreach($value in $values){Set-ApoValue -Key $restricted -Value $value}
        $snapshot=@(foreach($value in $values){Get-ApoValue -Key $restricted -Name $value.Name})
        $xml=[Management.Automation.PSSerializer]::Serialize($snapshot)
        foreach($value in $values){Set-ApoValue -Key $restricted -Value ([pscustomobject]@{Name=$value.Name;Exists=$false;Kind=$null;Value=$null})}
        foreach($value in [Management.Automation.PSSerializer]::Deserialize($xml)){Set-ApoValue -Key $restricted -Value $value}
        if($restricted.GetValueKind('OneMulti') -ne [Microsoft.Win32.RegistryValueKind]::MultiString){throw 'Type MULTI_SZ perdu.'}
        $forbidden=$false
        try {$created=$restricted.CreateSubKey('forbidden');$created.Dispose()}catch {$forbidden=$true}
        if(-not $forbidden){throw 'Le handle demandé a obtenu CreateSubKey par erreur.'}
    }finally{$restricted.Dispose()}
    Write-Host 'PASS : accès SetValue/QueryValues, cinq types/variantes restaurés, CreateSubKey refusé.'
}finally{$key.Dispose();[Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree($path,$false)}
