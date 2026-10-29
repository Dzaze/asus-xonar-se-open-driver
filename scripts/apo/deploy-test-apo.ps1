<#
.SYNOPSIS
    Déploie seulement la DLL APO signée en TEST et prépare son journal LOCAL SERVICE.
.DESCRIPTION
    Aucun pilote, INF, format, volume, BCD ou script de cross-sign modifié.
    Le COM déjà enregistré doit viser exactement le chemin de destination.
    Sauvegarde binaire et SDDL avant remplacement ; restauration sur erreur.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Dll,[Parameter(Mandatory)][string]$BackupDirectory)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
$admin=([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if(-not $admin){throw 'Déploiement APO : console élevée requise.'}
$Dll=(Resolve-Path -LiteralPath $Dll).Path
$target='C:\Program Files\XonarSE\XonarApo.dll'
$clsid='Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\{D2FBC737-00C3-4482-B3DD-02D92C06003E}\InprocServer32'
if((Get-Item -LiteralPath $clsid).GetValue('') -ne $target){throw 'Le chemin COM préexistant ne correspond pas : arrêt.'}
$signature=Get-AuthenticodeSignature -LiteralPath $Dll
if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Thumbprint -ne 'FF68A5C11C614F5E45AD64594D5A978BD9E5D321'){throw 'La DLL doit être signée par le certificat de TEST Xonar attendu et sa signature valide.'}
New-Item -ItemType Directory -Path $BackupDirectory -Force|Out-Null
$BackupDirectory=(Resolve-Path -LiteralPath $BackupDirectory).Path
$backup=Join-Path $BackupDirectory 'XonarApo-before.dll'
if(Test-Path -LiteralPath $backup){throw 'Ce dossier contient déjà un témoin ; utiliser un dossier neuf.'}
Copy-Item -LiteralPath $target -Destination $backup
$data='C:\ProgramData\XonarSE';$log=Join-Path $data 'apo-log.txt';$status=Join-Path $data 'apo-live-status.json'
$dataExisted=Test-Path -LiteralPath $data
$logExisted=Test-Path -LiteralPath $log
$statusExisted=Test-Path -LiteralPath $status
$oldDirectoryAcl=if($dataExisted){(Get-Acl -LiteralPath $data).Sddl}else{$null}
$oldLogAcl=if($logExisted){(Get-Acl -LiteralPath $log).Sddl}else{$null}
$oldStatusAcl=if($statusExisted){(Get-Acl -LiteralPath $status).Sddl}else{$null}
[pscustomobject]@{Target=$target;BeforeHash=(Get-FileHash -LiteralPath $backup).Hash;DataExisted=$dataExisted;LogExisted=$logExisted;StatusExisted=$statusExisted;DirectorySddl=$oldDirectoryAcl;LogSddl=$oldLogAcl;StatusSddl=$oldStatusAcl}|Export-Clixml -LiteralPath (Join-Path $BackupDirectory 'deployment.clixml')
try {
    New-Item -ItemType Directory -Path $data -Force|Out-Null
    if(-not $logExisted){New-Item -ItemType File -Path $log|Out-Null}
    $sid=[Security.Principal.SecurityIdentifier]::new('S-1-5-19') # LOCAL SERVICE
    $acl=Get-Acl -LiteralPath $data
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,[Security.AccessControl.FileSystemRights]::ReadAndExecute,[Security.AccessControl.AccessControlType]::Allow))
    Set-Acl -LiteralPath $data -AclObject $acl
    $acl=Get-Acl -LiteralPath $log
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,[Security.AccessControl.FileSystemRights]::Modify,[Security.AccessControl.AccessControlType]::Allow))
    Set-Acl -LiteralPath $log -AclObject $acl
    if(-not(Test-Path -LiteralPath $status)){New-Item -ItemType File -Path $status|Out-Null}
    $acl=Get-Acl -LiteralPath $status
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,[Security.AccessControl.FileSystemRights]::Modify,[Security.AccessControl.AccessControlType]::Allow))
    Set-Acl -LiteralPath $status -AclObject $acl
    Stop-Service -Name Audiosrv -Force
    Copy-Item -LiteralPath $Dll -Destination $target -Force
    if((Get-FileHash -LiteralPath $Dll).Hash -ne (Get-FileHash -LiteralPath $target).Hash){throw 'Hash de copie incorrect.'}
    Write-Host "DLL signée copiée ; sauvegarde : $backup"
}catch{
    Copy-Item -LiteralPath $backup -Destination $target -Force
    if($oldDirectoryAcl){$acl=Get-Acl -LiteralPath $data;$acl.SetSecurityDescriptorSddlForm($oldDirectoryAcl);Set-Acl -LiteralPath $data -AclObject $acl}
    if($oldLogAcl){$acl=Get-Acl -LiteralPath $log;$acl.SetSecurityDescriptorSddlForm($oldLogAcl);Set-Acl -LiteralPath $log -AclObject $acl}
    if($oldStatusAcl){$acl=Get-Acl -LiteralPath $status;$acl.SetSecurityDescriptorSddlForm($oldStatusAcl);Set-Acl -LiteralPath $status -AclObject $acl}
    elseif(-not $statusExisted -and (Test-Path -LiteralPath $status)){Remove-Item -LiteralPath $status}
    throw
}finally{
    Start-Service -Name Audiosrv
}
