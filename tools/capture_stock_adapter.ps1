<#
.SYNOPSIS
  Capture everything obtainable from a stock HarvestRight adapter, without
  opening the case or writing to it.

.DESCRIPTION
  Built for a unit that is going back. Every step is READ-ONLY:

    * no write-flash
    * no case opening
    * nothing that leaves the device in a changed state

  Only the OTG connector is needed. That is the adapter's USB *device* port -
  the one the dryer plugs into - so a PC standing in as host reaches it fine.

  Run with the adapter plugged into this PC by its OTG port.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\capture_stock_adapter.ps1
#>

$ErrorActionPreference = 'Continue'
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$outDir  = "stock-adapter-$stamp"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
Write-Output "Capturing to $outDir`n"

# --------------------------------------------------------------------------
# 1. USB identity. Cheapest and completely passive - and once the hardware is
#    gone these values cannot be recovered at any price.
# --------------------------------------------------------------------------
Write-Output '=== 1/4  USB identity ==='
$usb = Get-PnpDevice | Where-Object { $_.InstanceId -match 'USB\\VID_' -and $_.Present }
$usb | Select-Object Status, Class, FriendlyName, InstanceId |
    Format-List | Out-File "$outDir\pnp-devices.txt"

foreach ($d in $usb) {
    "=== $($d.FriendlyName) ===" | Out-File "$outDir\pnp-properties.txt" -Append
    Get-PnpDeviceProperty -InstanceId $d.InstanceId -ErrorAction SilentlyContinue |
        Select-Object KeyName, Type, Data |
        Format-Table -AutoSize | Out-File "$outDir\pnp-properties.txt" -Append
}
$usb | Select-Object FriendlyName, InstanceId | Format-Table -AutoSize
Write-Output "  -> pnp-devices.txt, pnp-properties.txt`n"

# --------------------------------------------------------------------------
# 2. Serial ports, so the right one can be identified for step 3.
# --------------------------------------------------------------------------
Write-Output '=== 2/4  Serial ports ==='
$ports = Get-CimInstance Win32_PnPEntity |
    Where-Object { $_.Name -match 'COM\d+' } |
    Select-Object Name, DeviceID
$ports | Format-Table -AutoSize
$ports | Format-List | Out-File "$outDir\serial-ports.txt"
Write-Output "  -> serial-ports.txt`n"

# --------------------------------------------------------------------------
# 3. Protocol interrogation. The main event: this is what captures the GOTIT
#    payload that PROTOCOL_NOTES.md flags as the session gate.
# --------------------------------------------------------------------------
Write-Output '=== 3/4  Protocol interrogation ==='
# Prefer an ESP-IDF python if one is installed; fall back to whatever is on PATH.
$py = Join-Path $env:USERPROFILE '.espressif\python_env\idf6.0_py3.11_env\Scripts\python.exe'
if (-not (Test-Path $py)) { $py = 'python' }

$cand = $ports | Where-Object { $_.DeviceID -notmatch 'ACPI' } | Select-Object -First 1
if ($cand -and $cand.Name -match '(COM\d+)') {
    $com = $Matches[1]
    Write-Output "  Using $com  (override by running interrogate_adapter.py yourself)"
    & $py tools\interrogate_adapter.py $com 2>&1 | Tee-Object "$outDir\interrogation.txt"
    Move-Item adapter-transcript-*.txt $outDir -ErrorAction SilentlyContinue
} else {
    Write-Output '  No non-ACPI COM port found. If the adapter did not enumerate as'
    Write-Output '  a serial device, note its VID/PID from step 1 - a vendor-specific'
    Write-Output '  interface is itself a finding.'
}
Write-Output ''

# --------------------------------------------------------------------------
# 4. Flash dump ATTEMPT. May well fail: entering download mode normally needs
#    the BOOT button, which is inside the case. Worth trying because it costs
#    nothing and is read-only either way.
# --------------------------------------------------------------------------
Write-Output '=== 4/4  Flash dump attempt (read-only) ==='
if ($cand -and $com) {
    Write-Output "  Trying flash-id on $com ..."
    & $py -m esptool --port $com flash-id 2>&1 | Tee-Object "$outDir\flash-id.txt"
    Write-Output ''
    Write-Output '  If that reported a flash chip, dump it with:'
    Write-Output "    python -m esptool --port $com --baud 921600 read-flash 0 0x400000 $outDir\stock.bin"
    Write-Output ''
    Write-Output '  If it could not connect, the ROM bootloader needs BOOT held at'
    Write-Output '  reset - which means opening the case. Skip it; steps 1-3 are the'
    Write-Output '  ones that matter.'
}

Write-Output ''
Write-Output "Done. Everything is in $outDir"
Write-Output 'Nothing was written to the adapter; it is unchanged and returnable.'
