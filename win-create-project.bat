@echo off

rem Download premake5 beta2 (bundled exe is too old for vs2022)
powershell -NoProfile -NonInteractive -Command ^
  "Invoke-WebRequest -Uri 'https://github.com/premake/premake-core/releases/download/v5.0.0-beta2/premake-5.0.0-beta2-windows.zip' -OutFile '%TEMP%\premake5.zip' -UseBasicParsing; Expand-Archive '%TEMP%\premake5.zip' -DestinationPath '%TEMP%\premake5_dl' -Force; Copy-Item '%TEMP%\premake5_dl\premake5.exe' 'utils\premake5.exe' -Force"

rem Generate solutions
utils\premake5.exe vs2022

rem Create a shortcut to the solution - http://superuser.com/questions/392061/how-to-make-a-shortcut-from-cmd
set SCRIPTFILE="%TEMP%\CreateMyShortcut.vbs"
(
	echo Set oWS = WScript.CreateObject^("WScript.Shell"^)
	echo sLinkFile = oWS.ExpandEnvironmentStrings^("ml_pgsql.sln - Shortcut.lnk"^)
	echo Set oLink = oWS.CreateShortcut^(sLinkFile^)
	echo oLink.TargetPath = oWS.ExpandEnvironmentStrings^("%~dp0\Build\ml_pgsql.sln"^)
	echo oLink.Save
) 1>%SCRIPTFILE%
cscript //nologo %SCRIPTFILE%
del /f /q %SCRIPTFILE%

if %0 == "%~0" pause
