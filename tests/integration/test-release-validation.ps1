#requires -Version 7.0
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Import-Module (Join-Path $repo 'scripts\release\ReleaseValidation.psm1') -Force
$source=[IO.File]::ReadAllText((Join-Path $repo 'src\driver\xonarse\xonarse.inx'))
$valid='DriverVer=11/01/2026,0.5.0.1'+[Environment]::NewLine+$source
if((Get-XonarReleaseDriverVersion $valid) -ne '0.5.0.1'){throw 'Driver version incorrectly reported'}
Write-Host 'PASS : persistent FX and real driver version accepted'
$cases=@{
    MissingReference=$valid.Replace(', XonarSe.FX.Speaker.AddReg','')
    MissingLfx=$valid.Replace('{C243A8CB-D535-4BA0-BCC5-CC4338833E7A}','{00000000-0000-0000-0000-000000000000}')
    MissingGfx=$valid.Replace('{D2FBC737-00C3-4482-B3DD-02D92C06003E}','{00000000-0000-0000-0000-000000000000}')
    WrongSection=$valid.Replace('[XonarSe.FX.Speaker.AddReg]','[UnrelatedEffects]')
}
foreach($case in $cases.GetEnumerator()){
    $failed=$false;try{Get-XonarReleaseDriverVersion $case.Value|Out-Null}catch{$failed=$true}
    if(-not $failed){throw ('Invalid driver accepted: '+$case.Key)}
    Write-Host ('PASS : rejected '+$case.Key)
}
Write-Host '5 release validation scenarios passed.'
