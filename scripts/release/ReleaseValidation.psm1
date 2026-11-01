# SPDX-License-Identifier: Apache-2.0
Set-StrictMode -Version Latest
function Get-XonarReleaseDriverVersion {
    param([Parameter(Mandatory)][string]$InfText)
    $topology=[regex]::Match($InfText,'(?ims)^\[XonarSe\.I\.TopologySpeaker\][^\r\n]*\r?\n(.*?)(?=^\[|\z)').Groups[1].Value
    $addReg=[regex]::Match($topology,'(?im)^AddReg\s*=\s*([^\r\n]+)').Groups[1].Value
    if('XonarSe.FX.Speaker.AddReg' -notin @($addReg.Split(',')|ForEach-Object {$_.Trim()})){throw 'Paquet pilote refuse : section FX haut-parleurs non referencee.'}
    $effects=[regex]::Match($InfText,'(?ims)^\[XonarSe\.FX\.Speaker\.AddReg\][^\r\n]*\r?\n(.*?)(?=^\[|\z)').Groups[1].Value
    foreach($pair in @(@('0','00000000-0000-0000-0000-000000000000'),@('1','C243A8CB-D535-4BA0-BCC5-CC4338833E7A'),@('2','D2FBC737-00C3-4482-B3DD-02D92C06003E'))){
        $pattern='(?im)^HKR,"FX\\0","\{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D\},'+$pair[0]+'",,"\{'+$pair[1]+'\}"\s*$'
        if($effects -notmatch $pattern){throw ('Paquet pilote refuse : declaration LFX/GFX persistante manquante, PID '+$pair[0]+'.')}
    }
    if($InfText -notmatch '(?im)^DriverVer\s*=\s*[^,]+,\s*([\d.]+)\s*$'){throw 'DriverVer absent du paquet signe.'}
    $Matches[1]
}
Export-ModuleMember -Function Get-XonarReleaseDriverVersion
