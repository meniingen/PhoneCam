# Hamtar scrcpy (inkl. adb + scrcpy-server) och ffmpeg till tools\
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$tmp  = Join-Path $env:TEMP 'phonecam-tools'
New-Item -ItemType Directory -Force $tmp | Out-Null

$ver = '5.0.1'
$sc  = Join-Path $tmp 'scrcpy.zip'
Invoke-WebRequest "https://github.com/Genymobile/scrcpy/releases/download/v$ver/scrcpy-win64-v$ver.zip" -OutFile $sc
Expand-Archive $sc $tmp -Force
Copy-Item "$tmp\scrcpy-win64-v$ver\*" "$root\tools\scrcpy" -Recurse -Force

$ff = Join-Path $tmp 'ffmpeg.zip'
Invoke-WebRequest 'https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip' -OutFile $ff
Expand-Archive $ff $tmp -Force
$exe = Get-ChildItem $tmp -Recurse -Filter ffmpeg.exe | Select-Object -First 1
Copy-Item $exe.FullName "$root\tools\ffmpeg\ffmpeg.exe" -Force
Write-Host 'Klart.'
