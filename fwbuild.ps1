$env:PATH = "$env:USERPROFILE\arduino-cli-root;$env:PATH"
$sketch = "d:\dev\WorkBuddy-Cardputer-BLE\firmware\ask_master_ble"
$out = "d:\dev\WorkBuddy-Cardputer-BLE\firmware\ask_master_ble\build"
# huge_app: 3 MB app partition (no OTA). The default scheme caps the app at
# 1.25 MB, which the CJK font plus the pinyin IME would exhaust.
$fqbn = "m5stack:esp32:m5stack_cardputer:PartitionScheme=huge_app"
arduino-cli compile --fqbn $fqbn --build-path $out $sketch 2>&1 | Select-Object -Last 12
Write-Output "=== exit: $LASTEXITCODE ==="
