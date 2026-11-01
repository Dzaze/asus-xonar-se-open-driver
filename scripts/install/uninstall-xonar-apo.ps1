# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
    Desinstalle l'APO Xonar SE et remet le systeme dans son etat d'avant installation.
.DESCRIPTION
    Inverse exactement install-xonar-apo.ps1. Chaque etape est journalisee et non
    destructive pour le pilote.

      1. Auto-elevation si necessaire.
      2. Resolution de l'endpoint haut-parleurs par le materiel (echec = non bloquant :
         si le pilote a disparu, ses cles d'endpoint ont disparu avec lui).
      3. Retrait cible de nos CLSID FxProperties (LFX/GFX ou anciennes familles).
      4. Conservation du choix utilisateur Disable_SysFx et des autres effets.
      5. Desenregistrement COM : regsvr32 /u.
      6. Filet de securite : suppression de HKCR\CLSID\{...} et
         AudioEngine\AudioProcessingObjects\{...} pour les 2 CLSID.
      7. Suppression du dossier C:\Program Files\XonarSE (2e tentative apres le
         redemarrage audio si la DLL etait encore chargee par audiodg).
      8. Redemarrage de AudioEndpointBuilder.

    -RemoveConfig supprime aussi C:\ProgramData\XonarSE (sinon la config apo-efx.cfg
    est conservee). Le pilote xonarse (.sys) et la configuration 5.1 ne sont
    JAMAIS touches.
.PARAMETER InstallDir
    Dossier de la DLL a retirer. Defaut : C:\Program Files\XonarSE.
.PARAMETER ConfigDir
    Dossier de configuration. Defaut : C:\ProgramData\XonarSE.
.PARAMETER RemoveConfig
    Supprime egalement C:\ProgramData\XonarSE (config utilisateur).
.PARAMETER WhatIf
    Essai a blanc : affiche le plan sans rien supprimer (lectures seules).
.PARAMETER Pause
    Laisse la fenetre ouverte a la fin.
.EXAMPLE
    .\uninstall-xonar-apo.ps1 -WhatIf
    Previsualise la desinstallation sans rien modifier.
.EXAMPLE
    .\uninstall-xonar-apo.ps1 -RemoveConfig -Pause
    Desinstalle tout, y compris C:\ProgramData\XonarSE.
#>
[CmdletBinding()]
param(
    [string]$InstallDir = 'C:\Program Files\XonarSE',
    [string]$ConfigDir  = 'C:\ProgramData\XonarSE',
    [switch]$RemoveConfig,
    [switch]$WhatIf,
    [switch]$Pause
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
foreach($directory in $InstallDir,$ConfigDir){
    $full=[IO.Path]::GetFullPath($directory).TrimEnd('\')
    if($full -in @([IO.Path]::GetPathRoot($full).TrimEnd('\'),$env:WINDIR,$env:ProgramFiles,$env:ProgramData,$env:USERPROFILE)){
        throw "Dossier trop large : $full"
    }
}
$InstallDir=[IO.Path]::GetFullPath($InstallDir)
$ConfigDir=[IO.Path]::GetFullPath($ConfigDir)

$here = $PSScriptRoot
Import-Module (Join-Path $here 'XonarApoCommon.psm1') -Force
Import-Module (Join-Path $here 'ApoEffectsProtection.psm1') -Force

# Auto-elevation (sauf en essai a blanc, qui ne fait que des lectures).
if (-not (Invoke-XonarSelfElevation -ScriptPath $PSCommandPath -BoundParameters $PSBoundParameters -SkipIfNotAdmin:$WhatIf)) {
    exit 0
}

$stamp   = Get-Date -Format 'yyyyMMdd_HHmmss'
$logFile = Join-Path $env:TEMP ("xonar-apo-uninstall_{0}.log" -f $stamp)
Initialize-XonarLog -Path $logFile

$regsvr   = Join-Path $env:WINDIR 'System32\regsvr32.exe'
$dstDll   = Join-Path $InstallDir 'XonarApo.dll'
$retryRemoveInstall = $false
$exitCode = 0

try {
    Write-XonarLog ("Desinstallation APO Xonar SE  (essai a blanc = {0}, RemoveConfig = {1})" -f $WhatIf.IsPresent, $RemoveConfig.IsPresent)
    Write-XonarLog "Journal : $logFile"
    Restore-XonarApoEffectsProtection -InstallDir $InstallDir -Preview:$WhatIf

    # --- 2) Resolution de l'endpoint (non bloquante) ---------------------------------
    $target = $null
    try {
        $target = Resolve-XonarSpeakerEndpoint
        Write-XonarLog ("Endpoint : {0}  (canaux={1}, nom='{2}')" -f $target.EndpointGuid, $target.Channels, $target.FriendlyName) 'OK'
    }
    catch {
        Write-XonarLog "Endpoint haut-parleurs non resolu ($($_.Exception.Message))." 'ATTENTION'
        Write-XonarLog "Nettoyage FxProperties ignore : l'endpoint et ses cles ont disparu avec le pilote." 'ATTENTION'
    }

    # --- 3+4) Retirer seulement nos CLSID, sans reactivation de SysFx utilisateur -----
    if ($target) {
        $detach = Join-Path $PSScriptRoot '..\apo\unregister-apo.ps1'
        if ($WhatIf) { & $detach -EndpointGuid $target.EndpointGuid -DryRun }
        else { & $detach -EndpointGuid $target.EndpointGuid }
    }

    # --- 5) Desenregistrement COM ----------------------------------------------------
    if (Test-Path -LiteralPath $dstDll) {
        if ($WhatIf) { Write-XonarLog "Desenregistrerait le COM : regsvr32 /u /s `"$dstDll`"." 'PLAN' }
        else {
            $r = Start-Process -FilePath $regsvr -ArgumentList ('/u /s "{0}"' -f $dstDll) -Wait -PassThru
            Write-XonarLog "regsvr32 /u : code $($r.ExitCode)." $(if ($r.ExitCode -eq 0) { 'OK' } else { 'ATTENTION' })
        }
    }
    else { Write-XonarLog "DLL $dstDll absente : desenregistrement par la DLL ignore (filet HKCR ci-dessous)." 'ATTENTION' }

    # --- 6) Filet de securite : nettoyage HKCR ---------------------------------------
    $clsids = Get-XonarApoClsids
    foreach ($c in @($clsids.Sfx, $clsids.Efx)) {
        foreach ($path in @("HKLM\SOFTWARE\Classes\CLSID\$c", "HKLM\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$c")) {
            $drivePath = 'HKLM:' + $path.Substring(4)
            if ($WhatIf) {
                $etat = if (Test-Path -LiteralPath $drivePath) { 'presente' } else { 'absente' }
                Write-XonarLog "Supprimerait (si presente) la cle $path [$etat]." 'PLAN'
            }
            else {
                if (Test-Path -LiteralPath $drivePath) {
                    & reg delete $path /f *> $null
                    Write-XonarLog "supprime $path (code $LASTEXITCODE)." 'OK'
                }
                else { Write-XonarLog "$path deja absente." }
            }
        }
    }

    # --- 7) Suppression du dossier d'installation ------------------------------------
    if ($WhatIf) { Write-XonarLog "Supprimerait seulement $dstDll ; les sauvegardes sont conservees." 'PLAN' }
    else {
        if (Test-Path -LiteralPath $dstDll) {
            try { Remove-Item -LiteralPath $dstDll -Force; Write-XonarLog "DLL supprimee : $InstallDir" 'OK' }
            catch {
                Write-XonarLog "Suppression differee ($($_.Exception.Message)) : nouvelle tentative apres le redemarrage audio." 'ATTENTION'
                $retryRemoveInstall = $true
            }
        }
        else { Write-XonarLog "$InstallDir deja absent." }
    }

    # --- 8) Rechargement du moteur audio ---------------------------------------------
    if ($WhatIf) { Write-XonarLog "Redemarrerait AudioEndpointBuilder (-Force), puis demarrerait Audiosrv." 'PLAN' }
    else {
        Restart-Service -Name AudioEndpointBuilder -Force
        Start-Service -Name Audiosrv
        Start-Sleep -Seconds 2
        Write-XonarLog ("Services : Audiosrv={0}, AudioEndpointBuilder={1}" -f (Get-Service Audiosrv).Status, (Get-Service AudioEndpointBuilder).Status) 'OK'
        if ($retryRemoveInstall -and (Test-Path -LiteralPath $dstDll)) {
            try { Remove-Item -LiteralPath $dstDll -Force; Write-XonarLog "DLL supprimee apres redemarrage : $InstallDir" 'OK' }
            catch { Write-XonarLog "Dossier encore verrouille : $InstallDir ($($_.Exception.Message))." 'ATTENTION' }
        }
    }

    # --- Config partagee (optionnelle) -----------------------------------------------
    if ($RemoveConfig) {
        if ($WhatIf) { Write-XonarLog "Supprimerait aussi la config : $ConfigDir." 'PLAN' }
        else {
            if (Test-Path -LiteralPath $ConfigDir) { Remove-Item -LiteralPath $ConfigDir -Recurse -Force; Write-XonarLog "Config supprimee : $ConfigDir" 'OK' }
            else { Write-XonarLog "$ConfigDir deja absent." }
        }
    }
    else { Write-XonarLog "Config conservee : $ConfigDir (utiliser -RemoveConfig pour l'effacer)." }

    Write-XonarLog "=== Desinstallation terminee : etat audio = avant APO ===" 'OK'
}
catch {
    Write-XonarLog "ECHEC : $($_.Exception.Message)" 'ERREUR'
    if ($_.ScriptStackTrace) { Write-XonarLog $_.ScriptStackTrace 'ERREUR' }
    $exitCode = 1
}
finally {
    if ($Pause) { Read-Host 'Appuyez sur Entree pour fermer' | Out-Null }
}

exit $exitCode
