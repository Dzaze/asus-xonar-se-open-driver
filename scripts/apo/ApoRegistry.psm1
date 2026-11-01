# SPDX-License-Identifier: Apache-2.0
# Accès minimal : ne demande JAMAIS CreateSubKey sur MMDevices/FxProperties.
Set-StrictMode -Version Latest
$script:RenderSubkey='SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
function Get-XonarSpeakerTarget {
    param([string]$EndpointGuid)
    $hive=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
    try {
        $render=$hive.OpenSubKey($script:RenderSubkey)
        if(-not $render){throw 'Magasin Render absent.'}
        try {
            $ids=if($EndpointGuid){@(([guid]$EndpointGuid).ToString('B'))}else{@($render.GetSubKeyNames())}
            $found=@(foreach($id in $ids){
                $endpoint=$render.OpenSubKey($id)
                if(-not $endpoint){continue}
                try {
                    $properties=$endpoint.OpenSubKey('Properties')
                    if(-not $properties){continue}
                    try {
                        $devnode=[string]$properties.GetValue('{b3f8fa53-0004-438e-9003-51a46e139bfc},2')
                        $formFactor=$properties.GetValue('{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},0')
                        $speakers=$properties.GetValue('{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},3')
                        if($devnode -match '(?i)USB\\VID_0B05&PID_189D&MI_00\\' -and $formFactor -eq 1 -and $speakers -eq 0x60f -and $endpoint.GetValue('DeviceState') -eq 1){
                            [pscustomobject]@{EndpointGuid=$id;Devnode=$devnode;DeviceState=$endpoint.GetValue('DeviceState');Subkey="$script:RenderSubkey\$id\FxProperties"}
                        }
                    }finally{$properties.Dispose()}
                }finally{$endpoint.Dispose()}
            })
            if($found.Count -ne 1){throw "Cible haut-parleurs Xonar 5.1 non unique ou non conforme ($($found.Count)). Aucun nom convivial utilisé."}
            return $found[0]
        }finally{$render.Dispose()}
    }finally{$hive.Dispose()}
}
function Open-ApoEffectsKey {
    param([Parameter(Mandatory)][string]$Subkey,[switch]$Writable)
    if($Subkey -notmatch '^SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\Render\\\{[0-9a-f-]{36}\}\\FxProperties$'){throw 'Sous-clé FxProperties invalide.'}
    $hive=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
    try {
        if($Writable){$key=$hive.OpenSubKey($Subkey,[Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,[System.Security.AccessControl.RegistryRights]'SetValue,QueryValues')}
        else{$key=$hive.OpenSubKey($Subkey)}
        if(-not $key){throw 'FxProperties absent : aucun changement ACL ni création implicite. Utiliser le provisionnement INF.'}
        return $key
    }finally{$hive.Dispose()}
}
function Get-ApoValue {
    param([Microsoft.Win32.RegistryKey]$Key,[string]$Name)
    $exists=$Key.GetValueNames() -contains $Name
    [pscustomobject]@{Name=$Name;Exists=$exists;Kind=if($exists){$Key.GetValueKind($Name).ToString()}else{$null};Value=if($exists){$Key.GetValue($Name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}else{$null}}
}
function Set-ApoValue {
    param([Microsoft.Win32.RegistryKey]$Key,[Parameter(Mandatory)]$Value)
    if($Value.Exists){
        $kind=[Microsoft.Win32.RegistryValueKind]$Value.Kind;$data=$Value.Value
        if($kind -eq [Microsoft.Win32.RegistryValueKind]::MultiString){$data=[string[]]@($data)}
        if($kind -eq [Microsoft.Win32.RegistryValueKind]::DWord){$data=[int]$data}
        if($kind -eq [Microsoft.Win32.RegistryValueKind]::Binary){$data=[byte[]]$data}
        $Key.SetValue($Value.Name,$data,$kind)
    }else{$Key.DeleteValue($Value.Name,$false)}
    $read=Get-ApoValue -Key $Key -Name $Value.Name
    if($read.Exists -ne $Value.Exists -or ($read.Exists -and ($read.Kind -ne $Value.Kind -or (ConvertTo-Json -InputObject @($read.Value) -Compress) -cne (ConvertTo-Json -InputObject @($Value.Value) -Compress)))){throw "Relecture non conforme de $($Value.Name)."}
}
function Restore-ApoSnapshot {
    param([Parameter(Mandatory)][string]$Path)
    $snapshot=Import-Clixml -LiteralPath $Path
    if($snapshot.Version -ne 1){throw 'Version de sauvegarde inconnue.'}
    $savedGuid=([guid]$snapshot.EndpointGuid).ToString('B')
    if($snapshot.Subkey -ne "$script:RenderSubkey\$savedGuid\FxProperties"){throw 'Chemin de sauvegarde incoherent.'}
    try{$target=Get-XonarSpeakerTarget -EndpointGuid $snapshot.EndpointGuid}
    catch{$target=Get-XonarSpeakerTarget}
    if($target.Devnode -ne $snapshot.Devnode){throw 'La sauvegarde ne correspond pas au matériel actuel.'}
    if($target.EndpointGuid -ne $snapshot.EndpointGuid){Write-Host "Endpoint reconstruit : restauration $($snapshot.EndpointGuid) vers $($target.EndpointGuid), meme materiel."}
    $key=Open-ApoEffectsKey -Subkey $target.Subkey -Writable
    try {foreach($value in $snapshot.Values){Set-ApoValue -Key $key -Value $value};$key.Flush()}finally{$key.Dispose()}
    Write-Host "Sauvegarde restaurée et relue : $Path"
}
function Get-XonarDriverState {
    # Win32_PnPSignedDriver peut rester en cache après une installation : relire
    # le devnode et sa clé de classe active, sans utiliser ce cache pour rollback.
    $enum='SYSTEM\CurrentControlSet\Enum\USB\VID_0B05&PID_189D&MI_00'
    $hive=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
    try {
        $device=$hive.OpenSubKey($enum)
        try {
            $instances=@($device.GetSubKeyNames())
            if($instances.Count -ne 1){throw 'Devnode Xonar non unique.'}
            $instance=$device.OpenSubKey($instances[0])
            try {
                $driver=$hive.OpenSubKey('SYSTEM\CurrentControlSet\Control\Class\'+$instance.GetValue('Driver'))
                try {[pscustomobject]@{DeviceID='USB\VID_0B05&PID_189D&MI_00\'+$instances[0];InfName=$driver.GetValue('InfPath');DriverVersion=$driver.GetValue('DriverVersion');DriverProviderName=$driver.GetValue('ProviderName')}}finally{$driver.Dispose()}
            }finally{$instance.Dispose()}
        }finally{$device.Dispose()}
    }finally{$hive.Dispose()}
}
Export-ModuleMember -Function Get-XonarSpeakerTarget,Get-XonarDriverState,Open-ApoEffectsKey,Get-ApoValue,Set-ApoValue,Restore-ApoSnapshot
