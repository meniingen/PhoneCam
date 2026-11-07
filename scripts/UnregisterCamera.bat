@echo off
net session >nul 2>&1 || (echo Hogerklicka och valj "Kor som administrator". & pause & exit /b 1)
regsvr32 /u "%~dp0..\third_party\softcam\bin\x64\softcam.dll"
pause
