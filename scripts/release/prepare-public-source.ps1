# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([string]$Destination)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if(-not $Destination){$Destination=Join-Path (Split-Path -Parent $repo) 'asus-xonar-se-open-driver-public'}
$Destination=[IO.Path]::GetFullPath($Destination)
if(Test-Path -LiteralPath $Destination){throw 'Le dossier public doit etre nouveau ; aucun historique prive ne sera reutilise.'}
New-Item -ItemType Directory -Path $Destination|Out-Null
function Copy-PublicFile([string]$Source,[string]$Relative){
    $target=Join-Path $Destination $Relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force|Out-Null
    Copy-Item -LiteralPath $Source -Destination $target
}
$trees=@('cmake','configs','contracts','src/common','src/dsp-core','src/audio-control','src/windows-apo','src/driver','apps/control-center','tests','tools/dsp-demo','tools/endpoint-probe','tools/usb-descriptor-dump','tools/apo-live-probe','tools/dsp-live-render','packaging','scripts/install','scripts/release')
foreach($tree in $trees){
    $source=Join-Path $repo $tree
    foreach($file in Get-ChildItem -LiteralPath $source -Recurse -File){
        $relative=$file.FullName.Substring($repo.Length).TrimStart('\')
        if($relative -match '(?i)(^|\\)(bin|obj|build|\.git|\.serena)(\\|$)'){continue}
        if($file.Extension -in '.exe','.dll','.sys','.cat','.pdb','.obj','.lib','.pfx','.p12','.key','.pem','.cer','.zip','.log','.tmp','.bak','.xbf','.pri'){continue}
        if($file.Extension -eq '.md' -and $tree -ne 'packaging'){continue}
        if($file.Extension -eq '.wav' -and $relative -notlike 'tests\dsp-golden-files\*'){continue}
        Copy-PublicFile $file.FullName $relative
    }
}
foreach($name in 'CMakeLists.txt','CMakePresets.json','.editorconfig','.gitattributes','LICENSE') {Copy-PublicFile (Join-Path $repo $name) $name}
Copy-PublicFile (Join-Path $repo 'docs\backlog.md') 'docs\backlog.md'
foreach($name in 'build.ps1','run-tests.ps1'){Copy-PublicFile (Join-Path $repo ('scripts\'+$name)) ('scripts\'+$name)}
foreach($name in 'ApoRegistry.psm1','register-apo.ps1','unregister-apo.ps1','repair-speaker-effects.ps1','protect-speaker-effects.ps1','deploy-test-apo.ps1','test-auto-mode-live.ps1','test-control-center-live.ps1','test-product-recovery-live.ps1'){
    Copy-PublicFile (Join-Path $repo ('scripts\apo\'+$name)) ('scripts\apo\'+$name)
}
foreach($name in 'build-driver.ps1','sign-test.ps1'){Copy-PublicFile (Join-Path $repo ('scripts\driver\'+$name)) ('scripts\driver\'+$name)}
foreach($file in Get-ChildItem (Join-Path $repo 'packaging\public') -Recurse -File){
    Copy-PublicFile $file.FullName $file.FullName.Substring((Join-Path $repo 'packaging\public').Length).TrimStart('\')
}
foreach($name in 'APO-PROTECTION.fr.md','RELEASE-0.5.0-preview.2.fr.md'){
    Copy-PublicFile (Join-Path $repo ('docs\'+$name)) ('docs\'+$name)
}
Copy-PublicFile (Join-Path $repo 'scripts\install\README.md') 'scripts\install\README.md'
Write-Host "Export public prepare : $Destination"
