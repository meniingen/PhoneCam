@echo off
rem Anvandning: copy-softcam.bat <sokvag till din softcam-mapp>   (den som innehaller "dist")
if "%~1"=="" (echo Ange softcam-mappen. & exit /b 1)
set S=%~1
set D=%~dp0..\third_party\softcam
copy /y "%S%\dist\include\softcam\softcam.h" "%D%\include\softcam\" || exit /b 1
copy /y "%S%\dist\lib\x64\softcam.lib"       "%D%\lib\x64\"        || exit /b 1
copy /y "%S%\dist\bin\x64\softcam.dll"       "%D%\bin\x64\"        || exit /b 1
echo Klart.
