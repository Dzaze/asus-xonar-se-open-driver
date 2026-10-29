@echo off
setlocal
set "XONAR_PS=%ProgramFiles%\PowerShell\7\pwsh.exe"
if not exist "%XONAR_PS%" set "XONAR_PS=powershell.exe"
"%XONAR_PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-xonar-apo.ps1" %*
if errorlevel 1 (
  echo Installation non terminee. Consultez le journal xonar-apo-install dans TEMP.
  pause
  exit /b 1
)
echo Operation terminee.
endlocal
