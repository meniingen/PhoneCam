@echo off
rem Kor som administrator. Registrerar softcam.dll sa Windows ser "Softcam" som webbkamera.
net session >nul 2>&1 || (echo Hogerklicka och valj "Kor som administrator". & pause & exit /b 1)
for %%D in ("%~dp0..\bin\x64\Release\softcam.dll" "%~dp0..\third_party\softcam\bin\x64\softcam.dll") do (
  if exist %%D (regsvr32 %%D & goto :done)
)
echo softcam.dll hittades inte.
:done
pause
