# SPDX-License-Identifier: Apache-2.0
<# Restaure la déclaration LFX/GFX sur les interfaces TopologySpeaker existantes.
   AudioEndpointBuilder crée lui-même FxProperties. Les permissions d'origine sont
   sauvegardées puis les clés FX sont protégées. Aucun changement de pilote,
   de format audio ou de profil. Sauvegarde typée et rollback sur erreur. #>
[CmdletBinding()]
param([string]$BackupDirectory,[switch]$Preview)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'ApoRegistry.psm1') -Force
Import-Module (Join-Path $PSScriptRoot '..\install\XonarApoCommon.psm1') -Force
Import-Module (Join-Path $PSScriptRoot '..\install\ApoEffectsProtection.psm1') -Force
if(-not(Invoke-XonarSelfElevation -ScriptPath $PSCommandPath -BoundParameters $PSBoundParameters -SkipIfNotAdmin:$Preview)){exit 0}
$target=Get-XonarSpeakerTarget
$driver=Get-XonarDriverState
$expected=@{
    '{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},0'='{00000000-0000-0000-0000-000000000000}'
    '{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},1'='{C243A8CB-D535-4BA0-BCC5-CC4338833E7A}'
    '{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D},2'='{D2FBC737-00C3-4482-B3DD-02D92C06003E}'
}
# Ne jamais remplacer une chaîne d'effets tierce.
$endpointFx=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($target.Subkey)
if($endpointFx){
    try {
        foreach($name in $endpointFx.GetValueNames() | Where-Object {$_ -match '^\{d04e05a6-594b-4fb6-a80d-01af5eed7d1d\},(1|2|5|6|7|13|14|15)$'}){
            foreach($value in @($endpointFx.GetValue($name))){if($value -notin $expected.Values){throw "Effet tiers sur l'endpoint : $name"}}
        }
    }finally{$endpointFx.Dispose()}
}
$dll='C:\Program Files\XonarSE\XonarApo.dll'
if(-not(Test-Path -LiteralPath $dll)){throw 'Installer le paquet APO complet avant la reparation.'}
foreach($clsid in @($expected.Values | Where-Object {$_ -ne '{00000000-0000-0000-0000-000000000000}'})){
    $com=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey("SOFTWARE\Classes\CLSID\$clsid\InprocServer32")
    try{if(-not $com -or $com.GetValue('') -ne $dll){throw "Classe APO non enregistrée vers la DLL installée : $clsid"}}finally{if($com){$com.Dispose()}}
}
$hive=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryView]::Registry64)
$snapshots=@()
try {
    foreach($category in '{6994AD04-93EF-11D0-A3CC-00A0C9223196}','{DDA54A40-1E4C-11D1-A050-405705C10000}'){
        $base="SYSTEM\CurrentControlSet\Control\DeviceClasses\$category"
        $parent=$hive.OpenSubKey($base)
        if(-not $parent){throw 'Catégorie audio absente.'}
        try {
            $devices=@($parent.GetSubKeyNames() | Where-Object {$_ -match '^##\?\#USB#VID_0B05&PID_189D&MI_00#'})
            if($devices.Count -ne 1){throw 'Interface KS Xonar non unique.'}
            $path="$base\$($devices[0])\#TopologySpeaker\Device Parameters\FX\0"
            $key=$hive.OpenSubKey($path)
            if(-not $key){throw 'Magasin FX interface absent : installer un pilote avec la déclaration FX dans son INF.'}
            try {
                foreach($name in $key.GetValueNames()){
                    if(-not $expected.ContainsKey($name) -or $key.GetValue($name) -ne $expected[$name]){throw "Déclaration FX tierce ou incompatible : $name"}
                }
                $snapshots+= [pscustomobject]@{Subkey=$path;Values=@(foreach($name in $expected.Keys){Get-ApoValue $key $name})}
            }finally{$key.Dispose()}
        }finally{$parent.Dispose()}
    }
    if($Preview){$snapshots | Select-Object Subkey;Write-Host "PLAN : déclarer LFX/GFX, redémarrer uniquement la Xonar et les services audio, vérifier FxProperties. Pilote conservé : $($driver.DriverVersion).";return}
    if(-not $BackupDirectory){$BackupDirectory=Join-Path $env:ProgramData ('XonarSE\Repair-'+(Get-Date -Format yyyyMMdd-HHmmss))}
    if(Test-Path -LiteralPath $BackupDirectory){throw 'Choisir un dossier de sauvegarde neuf.'}
    New-Item -ItemType Directory -Path $BackupDirectory|Out-Null
    $snapshots|Export-Clixml -LiteralPath (Join-Path $BackupDirectory 'interface-fx-before.clixml')
    $driver|Export-Clixml -LiteralPath (Join-Path $BackupDirectory 'driver-before.clixml')
    $cfg='C:\ProgramData\XonarSE\apo-efx.cfg'
    if(Test-Path -LiteralPath $cfg){Copy-Item -LiteralPath $cfg -Destination (Join-Path $BackupDirectory 'apo-efx.cfg')}
    $audioRunning=(Get-Service Audiosrv).Status -eq 'Running'
    $builderRunning=(Get-Service AudioEndpointBuilder).Status -eq 'Running'
    $protectionBefore=@(Get-XonarApoProtectionSnapshot -AllowUnattached)
    try {
        Restore-XonarApoEffectsProtection
        foreach($snapshot in $snapshots){
            $key=$hive.OpenSubKey($snapshot.Subkey,$true)
            try{foreach($name in $expected.Keys){Set-ApoValue $key ([pscustomobject]@{Name=$name;Exists=$true;Kind='String';Value=$expected[$name]})};$key.Flush()}finally{$key.Dispose()}
        }
        Stop-Service Audiosrv -Force
        $instance=$target.Devnode -replace '^\{1\}\.',''
        & pnputil.exe /restart-device $instance
        if($LASTEXITCODE -ne 0){throw "Redémarrage ciblé en échec : $LASTEXITCODE"}
        Restart-Service AudioEndpointBuilder -Force
        if($audioRunning){Start-Service Audiosrv}
        $verified=$false
        for($i=0;$i -lt 20;$i++){
            Start-Sleep -Milliseconds 250
            $current=Get-XonarSpeakerTarget
            $fx=$hive.OpenSubKey($current.Subkey)
            if(-not $fx){continue}
            $fx.Dispose()
            # Sur un endpoint déjà connu, Windows recrée le magasin sans recopier
            # les nouvelles valeurs d'interface. Appliquer le rattachement explicite.
            & (Join-Path $PSScriptRoot 'register-apo.ps1') -Attachment LfxGfx -Stage Both -BackupDirectory $BackupDirectory
            $fx=$hive.OpenSubKey($current.Subkey)
            try{$verified=@($expected.Keys|Where-Object {$fx.GetValue($_) -ne $expected[$_]}).Count -eq 0}finally{$fx.Dispose()}
            break
        }
        if(-not $verified){throw 'Windows ne reconstruit pas les associations APO attendues.'}
        $after=Get-XonarDriverState
        if($after.InfName -ne $driver.InfName -or $after.DriverVersion -ne $driver.DriverVersion){throw 'Le pilote a changé pendant la réparation.'}
        Stop-Service Audiosrv -Force
        Stop-Service AudioEndpointBuilder -Force
        Protect-XonarApoEffects
        if($builderRunning){Start-Service AudioEndpointBuilder}
        if($audioRunning){Restart-Service Audiosrv -Force}
        "LFX/GFX restaurés et relus. Pilote conservé : $($driver.DriverVersion). Sauvegarde : $BackupDirectory" | Tee-Object -FilePath (Join-Path $BackupDirectory 'result.txt')
    }catch{
        $_ | Out-String | Set-Content -LiteralPath (Join-Path $BackupDirectory 'error.txt')
        foreach($file in Get-ChildItem -LiteralPath $BackupDirectory -Filter 'fx-*.clixml'){Restore-ApoSnapshot -Path $file.FullName}
        foreach($snapshot in $snapshots){$key=$hive.OpenSubKey($snapshot.Subkey,$true);try{foreach($value in $snapshot.Values){Set-ApoValue $key $value};$key.Flush()}finally{$key.Dispose()}}
        Restore-XonarApoProtectionSnapshot -Snapshot $protectionBefore
        throw
    }finally{
        if($builderRunning){Start-Service AudioEndpointBuilder}
        if($audioRunning){Start-Service Audiosrv}
    }
}finally{$hive.Dispose()}
