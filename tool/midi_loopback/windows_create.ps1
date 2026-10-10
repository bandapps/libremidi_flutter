# Creates the virtual MIDI loopback pair the integration tests need on Windows.
# See README.md in this directory for why, and for the macOS equivalent.
#
# The endpoints disappear when the MIDI service or the PC restarts, which is
# fine for a test run. No administrator rights needed; enabling Windows MIDI
# Services itself is a separate step and does need them.

$ErrorActionPreference = 'Stop'

$console = Join-Path $env:ProgramFiles 'Windows MIDI Services\Tools\Console\midi.exe'
if (-not (Test-Path $console)) {
    Write-Host 'Windows MIDI Services SDK tools not found at:' -ForegroundColor Red
    Write-Host "  $console"
    Write-Host 'Install them from https://aka.ms/midi, then run this again.'
    exit 1
}

# Without the feature enabled the console refuses every command, and the plugin
# falls back to WinUWP where no virtual ports exist. Say so plainly rather than
# letting the loopback creation fail with something less obvious.
$check = & $console enumerate endpoints 2>&1 | Out-String
if ($check -match 'does not appear to be enabled') {
    Write-Host 'Windows MIDI Services is not enabled on this PC.' -ForegroundColor Red
    Write-Host 'The plugin falls back to WinUWP, which has no virtual ports, so'
    Write-Host 'the loopback-dependent tests cannot run. See README.md here.'
    exit 1
}

$nameA = 'LRM Test Loopback A'
$nameB = 'LRM Test Loopback B'

if ($check -match [regex]::Escape($nameA)) {
    Write-Host "'$nameA' already exists - nothing to do." -ForegroundColor Green
    exit 0
}

& $console loopback create --name-a $nameA --name-b $nameB
if ($LASTEXITCODE -ne 0) {
    Write-Host 'Creating the loopback pair failed.' -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host ''
Write-Host "Created '$nameA' / '$nameB'." -ForegroundColor Green
Write-Host 'They last until the MIDI service or the PC restarts.'
Write-Host 'Verify with: flutter test integration_test -d windows'
Write-Host 'The test named "loopback-backed coverage is enabled" must not skip.'
