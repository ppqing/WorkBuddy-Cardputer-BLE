# Compile the ask-master BLE firmware for M5Stack Cardputer.
# Requires: arduino-cli + m5stack:esp32 core + M5Unified/M5GFX/NimBLE/ArduinoJson + M5CardputerBLE lib
$ErrorActionPreference = "Stop"

# Locate arduino-cli: env override, common locations, then PATH.
$arduinoCli = $env:ARDUINO_CLI
if (-not $arduinoCli) {
    $candidates = @(
        "$env:USERPROFILE\arduino-cli-root\arduino-cli.exe",
        "$env:LOCALAPPDATA\arduino-cli\bin\arduino-cli.exe"
    )
    $arduinoCli = $candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $arduinoCli) { $arduinoCli = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source }
    if (-not $arduinoCli) { throw "arduino-cli not found. Set ARDUINO_CLI or install it." }
}

# Project-relative paths (works regardless of where the repo is checked out).
$projRoot = $PSScriptRoot
$sketch = Join-Path $projRoot "firmware\ask_master_ble"
$out = Join-Path $sketch "build"
$binDir = Join-Path $sketch "bin"
New-Item -ItemType Directory -Force -Path $out, $binDir | Out-Null

# huge_app: 3 MB app partition (no OTA). The default scheme caps the app at
# 1.25 MB, which the CJK font plus the pinyin IME would exhaust.
$fqbn = "m5stack:esp32:m5stack_cardputer:PartitionScheme=huge_app"
$extraLibs = Join-Path $projRoot "libraries"
$libArgs = @()
if (Test-Path $extraLibs) { $libArgs = @("--libraries", $extraLibs) }
& $arduinoCli compile --fqbn $fqbn --build-path $out $sketch @libArgs 2>&1 | Select-Object -Last 12
if ($LASTEXITCODE -ne 0) { throw "arduino-cli compile failed (exit $LASTEXITCODE)" }
Write-Output "=== compile OK ==="

# Merge into a single flashable image (esptool merge_bin).
$esptool = Get-Command esptool.py, esptool -ErrorAction SilentlyContinue | Select-Object -First 1
if ($esptool) {
    $bootloader = Join-Path $out "ask_master_ble.ino.bootloader.bin"
    $partitions = Join-Path $out "ask_master_ble.ino.partitions.bin"
    $app = Join-Path $out "ask_master_ble.ino.bin"
    $merged = Join-Path $binDir "ask-master-ble-merged.bin"
    & $esptool.Source --chip esp32s3 merge_bin -o $merged `
        --flash_mode dio --flash_freq 80m --flash_size 8MB `
        0x0 $bootloader 0x8000 $partitions 0x10000 $app 2>&1 | Select-Object -Last 5
    if ($LASTEXITCODE -eq 0) {
        Copy-Item $bootloader -Destination (Join-Path $binDir "bootloader.bin") -Force
        Copy-Item $partitions -Destination (Join-Path $binDir "partitions.bin") -Force
        Copy-Item $app -Destination (Join-Path $binDir "firmware.bin") -Force
        Write-Output "=== merged: $merged ==="
    }
} else {
    Write-Output "esptool not found; skipping merge. Build outputs in $out"
}
