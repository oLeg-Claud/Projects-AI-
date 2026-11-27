$ErrorActionPreference = 'Stop'
$projectDir = Split-Path -Parent $PSScriptRoot
python -m PyInstaller --noconfirm --onefile --console --name uso32-bootloader --distpath "$projectDir/dist" --workpath "$projectDir/build/pyinstaller" --specpath "$projectDir/build" "$projectDir/tools/uso_bootloader.py"
if ($LASTEXITCODE -ne 0) { throw 'Console build failed' }
