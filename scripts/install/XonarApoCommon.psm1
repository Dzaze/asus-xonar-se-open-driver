# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
    Fonctions communes aux scripts d'installation / desinstallation de l'APO Xonar SE.
.DESCRIPTION
    Regroupe la logique sensible, partagee par install-xonar-apo.ps1 et
    uninstall-xonar-apo.ps1 (source unique de verite) :

      - Resolution de l'endpoint haut-parleurs PAR LE MATERIEL :
        HWID du peripherique MEDIA -> enfants PnP (endpoints) -> endpoint dont le
        format de mixage fait 6 canaux. JAMAIS par le nom convivial, car le GUID
        d'endpoint change a chaque reinstallation du pilote.

      - Ouverture des cles d'endpoint (FxProperties / Properties) en .NET avec les
        SEULS droits 'SetValue,QueryValues'. Ces cles appartiennent a SYSTEM : les
        Administrateurs possedent SetValue mais PAS CreateSubKey, si bien que reg.exe
        et New-ItemProperty echouent en << Acces refuse >>. On ne cree jamais la cle.

      - Jeu de valeurs FxProperties ecrit par l'installeur (formes historique REG_SZ,
        composite REG_MULTI_SZ et modes DEFAULT).

      - Journalisation horodatee, test d'elevation et auto-elevation.

    Aucune fonction de ce module ne touche au pilote xonarse (.sys) ni a la
    configuration 5.1 : seules les associations d'APO et le COM user-mode sont gerees.
#>
Set-StrictMode -Version Latest

# --- Constantes partagees (identifiants techniques : NE PAS traduire ni renommer) ---
$script:RenderStore      = 'SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
$script:PkeyDeviceFormat = '{f19f064d-082c-4e27-bc73-6882a1bb8e4c},0'  # PKEY_AudioEngine_DeviceFormat (format de mixage)
$script:PkeyFormFactor   = '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},0'  # PKEY_AudioEndpoint_FormFactor
$script:PkeyPhysSpeakers = '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},3'  # PKEY_AudioEndpoint_PhysicalSpeakers
$script:PkeyFriendlyName = '{a45c254e-df1c-4efd-8020-67d146a850e0},2'  # PKEY_Device_FriendlyName (diagnostic seulement)

# --- Journalisation -----------------------------------------------------------------
$script:LogPath = $null

function Initialize-XonarLog {
    # Prepare le fichier journal (en-tete horodatee). Les messages suivants s'ajoutent.
    param([Parameter(Mandatory)][string]$Path)
    $dir = Split-Path -Parent $Path
    if ($dir -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
    $script:LogPath = $Path
    Set-Content -LiteralPath $Path -Value ("=== Journal APO Xonar SE  {0}  (compte {1}) ===" -f (Get-Date), (whoami)) -Encoding utf8
}

function Write-XonarLog {
    # Ecrit une ligne horodatee sur la console (couleur selon le niveau) et dans le journal.
    param(
        [Parameter(Mandatory, Position = 0)][string]$Message,
        [Parameter(Position = 1)][ValidateSet('INFO', 'OK', 'PLAN', 'ATTENTION', 'ERREUR')][string]$Level = 'INFO'
    )
    $line = '{0}  [{1,-9}]  {2}' -f (Get-Date).ToString('HH:mm:ss.fff'), $Level, $Message
    $color = switch ($Level) {
        'ERREUR'    { 'Red' }
        'ATTENTION' { 'Yellow' }
        'OK'        { 'Green' }
        'PLAN'      { 'Cyan' }
        default     { 'Gray' }
    }
    Write-Host $line -ForegroundColor $color
    if ($script:LogPath) { Add-Content -LiteralPath $script:LogPath -Value $line -Encoding utf8 }
}

# --- Elevation ----------------------------------------------------------------------
function Test-XonarAdmin {
    # Vrai si la session courante est administrateur.
    ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Invoke-XonarSelfElevation {
    # Si deja administrateur : retourne $true (poursuivre).
    # Sinon : relance le script courant en eleve (en transmettant tous ses parametres) et
    #         retourne $false ; l'appelant DOIT alors quitter.
    # -SkipIfNotAdmin (essai a blanc) : poursuit sans elever, en lecture seule.
    param(
        [Parameter(Mandatory)][string]$ScriptPath,
        [Parameter(Mandatory)][System.Collections.IDictionary]$BoundParameters,
        [switch]$SkipIfNotAdmin
    )
    if (Test-XonarAdmin) { return $true }
    if ($SkipIfNotAdmin) {
        Write-Host 'Essai a blanc sans elevation : seules des lectures sont effectuees.' -ForegroundColor Yellow
        return $true
    }
    Write-Host 'Elevation requise : relance en tant qu''administrateur...' -ForegroundColor Yellow
    $psExe = (Get-Process -Id $PID).Path
    if (-not $psExe -or $psExe -notmatch '(?i)(powershell|pwsh)\.exe$') { $psExe = 'powershell.exe' }
    $argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $ScriptPath + '"'))
    foreach ($kv in $BoundParameters.GetEnumerator()) {
        if ($kv.Value -is [System.Management.Automation.SwitchParameter]) {
            if ($kv.Value.IsPresent) { $argList += "-$($kv.Key)" }
        }
        else {
            $val = [string]$kv.Value
            if ($val -match '\s') { $val = '"' + $val + '"' }
            $argList += @("-$($kv.Key)", $val)
        }
    }
    try {
        $style = if(@($BoundParameters.Keys) -contains 'Pause' -and [bool]$BoundParameters['Pause']){'Normal'}else{'Hidden'}
        $child = Start-Process -FilePath $psExe -Verb RunAs -ArgumentList ($argList -join ' ') -WindowStyle $style -Wait -PassThru
        if($child.ExitCode -ne 0){throw "Le processus eleve a echoue (code $($child.ExitCode)). Voir le journal dans TEMP."}
    }
    catch {
        throw "Elevation ou installation impossible : $($_.Exception.Message)"
    }
    return $false
}

# --- Resolution de l'endpoint -------------------------------------------------------
function Get-XonarEndpointMixChannels {
    # Nombre de canaux du format de mixage (PKEY_AudioEngine_DeviceFormat).
    # Le blob REG_BINARY = en-tete PROPVARIANT (8 octets) + WAVEFORMATEX ; nChannels
    # se trouve donc a l'offset 10. Repli robuste : on localise le wFormatTag
    # (0xFFFE EXTENSIBLE ou 0x0001 PCM) suivi d'un debit plausible.
    param([Parameter(Mandatory)][string]$EndpointGuid)
    $hive = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $k = $hive.OpenSubKey("$script:RenderStore\$EndpointGuid\Properties")
        if (-not $k) { return $null }
        try {
            $blob = $k.GetValue($script:PkeyDeviceFormat, $null)
            if ($blob -isnot [byte[]] -or $blob.Length -lt 18) { return $null }
            $nch = [BitConverter]::ToUInt16($blob, 10)
            $rate = [BitConverter]::ToUInt32($blob, 12)
            if ($nch -ge 1 -and $nch -le 16 -and $rate -ge 8000 -and $rate -le 768000) { return [int]$nch }
            for ($i = 0; $i -le $blob.Length - 8; $i++) {
                $tag = [BitConverter]::ToUInt16($blob, $i)
                if ($tag -ne 0xFFFE -and $tag -ne 0x0001) { continue }
                $c = [BitConverter]::ToUInt16($blob, $i + 2)
                $r = [BitConverter]::ToUInt32($blob, $i + 4)
                if ($c -ge 1 -and $c -le 16 -and $r -ge 8000 -and $r -le 768000) { return [int]$c }
            }
            return $null
        }
        finally { $k.Close() }
    }
    finally { $hive.Dispose() }
}

function Get-XonarEndpointDiagnostics {
    # Donnees LECTURE SEULE destinees au journal uniquement (jamais au choix de la cible).
    param([Parameter(Mandatory)][string]$EndpointGuid)
    $hive = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        $state = $null; $ff = $null; $spk = $null; $name = $null
        $ep = $hive.OpenSubKey("$script:RenderStore\$EndpointGuid")
        if ($ep) { try { $state = $ep.GetValue('DeviceState', $null) } finally { $ep.Close() } }
        $pr = $hive.OpenSubKey("$script:RenderStore\$EndpointGuid\Properties")
        if ($pr) {
            try {
                $ff   = $pr.GetValue($script:PkeyFormFactor, $null)
                $spk  = $pr.GetValue($script:PkeyPhysSpeakers, $null)
                $name = $pr.GetValue($script:PkeyFriendlyName, $null)
            }
            finally { $pr.Close() }
        }
        [pscustomobject]@{ DeviceState = $state; FormFactor = $ff; Speakers = $spk; FriendlyName = $name }
    }
    finally { $hive.Dispose() }
}

function Resolve-XonarSpeakerEndpoint {
    # Resout l'endpoint RENDER haut-parleurs PAR LE MATERIEL, jamais par le nom :
    #   1) peripherique(s) MEDIA dont l'InstanceId correspond au HWID Xonar ;
    #   2) enfants PnP = endpoints audio. Prefixe {0.0.0.00000000} = RENDER ;
    #   3) on retient l'unique endpoint dont le format de mixage fait 6 canaux
    #      (haut-parleurs 5.1 ; ni le casque stereo, ni le S/PDIF).
    param(
        [string]$HardwareIdPattern = 'USB\VID_0B05&PID_189D&MI_00*',
        [int]$ExpectedChannels = 6
    )
    $media = @(Get-PnpDevice -Class MEDIA -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like $HardwareIdPattern })
    if ($media.Count -eq 0) {
        throw "Aucun peripherique MEDIA correspondant au HWID '$HardwareIdPattern' (pilote Xonar absent ?)."
    }

    $candidates = @()
    foreach ($m in $media) {
        $children = @()
        try {
            $children = @((Get-PnpDeviceProperty -InstanceId $m.InstanceId -KeyName 'DEVPKEY_Device_Children' -ErrorAction Stop).Data)
        }
        catch { }
        foreach ($child in $children) {
            if ([string]::IsNullOrWhiteSpace($child)) { continue }
            # SWD\MMDEVAPI\{0.0.0.00000000}.{guid}  ->  RENDER uniquement
            if ($child -notmatch '(?i)^SWD\\MMDEVAPI\\\{0\.0\.0\.00000000\}\.(\{[0-9a-f-]{36}\})$') { continue }
            $guid = $Matches[1]
            $channels = Get-XonarEndpointMixChannels -EndpointGuid $guid
            $diag = Get-XonarEndpointDiagnostics -EndpointGuid $guid
            $candidates += [pscustomobject]@{
                EndpointGuid = $guid
                Channels     = $channels
                DeviceState  = $diag.DeviceState
                FormFactor   = $diag.FormFactor
                Speakers     = $diag.Speakers
                FriendlyName = $diag.FriendlyName
                Parent       = $m.InstanceId
            }
        }
    }
    if ($candidates.Count -eq 0) {
        throw 'Peripherique MEDIA Xonar trouve, mais aucun endpoint RENDER enfant (endpoints non provisionnes ?).'
    }

    $match = @($candidates | Where-Object { $_.Channels -eq $ExpectedChannels })
    if ($match.Count -ne 1) {
        $desc = ($candidates | ForEach-Object { "$($_.EndpointGuid)=$($_.Channels)ch" }) -join ', '
        throw "Endpoint haut-parleurs ($ExpectedChannels canaux) non unique : $($match.Count) trouve(s). Candidats : $desc. Aucun nom convivial utilise."
    }
    $sel = $match[0]
    $sel | Add-Member -NotePropertyName FxPropertiesSubkey -NotePropertyValue "$script:RenderStore\$($sel.EndpointGuid)\FxProperties"
    $sel | Add-Member -NotePropertyName PropertiesSubkey   -NotePropertyValue "$script:RenderStore\$($sel.EndpointGuid)\Properties"
    return $sel
}

# --- Acces .NET aux cles d'endpoint (SetValue,QueryValues uniquement) ---------------
function Open-XonarEndpointSubkey {
    # Ouvre .../Render/{guid}/FxProperties (ou /Properties) en .NET.
    # En ecriture : SEULEMENT SetValue,QueryValues (pas CreateSubKey, que les
    # Administrateurs n'ont pas sur ces cles SYSTEM). Retourne $null si la cle est
    # absente : on ne la cree JAMAIS (piege ACL confirme).
    param(
        [Parameter(Mandatory)][string]$Subkey,
        [switch]$Writable
    )
    if ($Subkey -notmatch '(?i)\\Render\\\{[0-9a-f-]{36}\}\\(FxProperties|Properties)$') {
        throw "Sous-cle d'endpoint invalide (FxProperties ou Properties attendu) : $Subkey"
    }
    $hive = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, [Microsoft.Win32.RegistryView]::Registry64)
    try {
        if ($Writable) {
            $rights = [System.Security.AccessControl.RegistryRights]'SetValue,QueryValues'
            return $hive.OpenSubKey($Subkey, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, $rights)
        }
        return $hive.OpenSubKey($Subkey)
    }
    finally { $hive.Dispose() }
}

# --- Donnees de reference (jeu de valeurs et CLSID) ---------------------------------
function Get-XonarApoClsids {
    # CLSID des deux APO (identifiants COM : ne pas modifier).
    [pscustomobject]@{
        Sfx = '{C243A8CB-D535-4BA0-BCC5-CC4338833E7A}'  # effet de flux (SFX)
        Efx = '{D2FBC737-00C3-4482-B3DD-02D92C06003E}'  # effet d'endpoint (EFX)
    }
}

function Get-XonarFxValuePlan {
    # Une seule generation : LFX par flux + GFX de sortie, formes REG_SZ.
    $fmt = '{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}'
    $clsids = Get-XonarApoClsids
    $kind = [Microsoft.Win32.RegistryValueKind]::String
    @(
        [pscustomobject]@{ Name="$fmt,0"; Kind=$kind; Value='{00000000-0000-0000-0000-000000000000}'; Note='Association ANY' }
        [pscustomobject]@{ Name="$fmt,1"; Kind=$kind; Value=$clsids.Sfx; Note='LFX avant mixage, format declare du flux' }
        [pscustomobject]@{ Name="$fmt,2"; Kind=$kind; Value=$clsids.Efx; Note='GFX apres mixage, calibration unique' }
    )
}

function Get-XonarDisableSysFxName {
    # PKEY_AudioEndpoint_Disable_SysFx : << Desactiver toutes les ameliorations >>.
    # Retire a la desinstallation s'il est present (sa presence couperait nos APO).
    '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5'
}

Export-ModuleMember -Function `
    Initialize-XonarLog, Write-XonarLog, Test-XonarAdmin, Invoke-XonarSelfElevation, `
    Resolve-XonarSpeakerEndpoint, Get-XonarEndpointMixChannels, Get-XonarEndpointDiagnostics, `
    Open-XonarEndpointSubkey, Get-XonarApoClsids, Get-XonarFxValuePlan, Get-XonarDisableSysFxName
