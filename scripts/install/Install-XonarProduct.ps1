# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageRoot,[Parameter(Mandatory)][ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ManifestHash,[switch]$Preview,[string]$ResultPath)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
Import-Module (Join-Path $PSScriptRoot 'XonarProduct.psm1') -Force
$logRoot=if($Preview){$env:TEMP}else{'C:\Program Files\XonarSE\Logs'}
New-Item -ItemType Directory -Path $logRoot -Force|Out-Null
$log=Join-Path $logRoot ('setup-'+(Get-Date -Format yyyyMMdd-HHmmss)+'.log')
Start-Transcript -LiteralPath $log|Out-Null
$code=0
try {
    $result=Install-XonarProduct -PackageRoot $PackageRoot -ManifestHash $ManifestHash -Preview:$Preview
    if($ResultPath){$result|ConvertTo-Json|Set-Content -LiteralPath $ResultPath -Encoding UTF8}
    if($result -and $result.NeedsRestart){Write-Host 'Windows demande un redemarrage. Enregistrer le travail puis redemarrer.';$code=3010}
}catch{Write-Host ('ECHEC : '+$_.Exception.Message);$code=1}
finally{Stop-Transcript|Out-Null}
exit $code
