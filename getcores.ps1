# Download every core systems.c names, from the libretro nightly buildbot, into cores\.
# The app does this itself now -- the Cores screen, or cartload.exe --selftest --fetch.
# This is still the way to fill cores\ before the folder is copied to the handheld.
# Re-run it whenever a system is added to the table -- it only fetches what is missing.
#   .\getcores.ps1              fetch the missing ones
#   .\getcores.ps1 -Force       re-fetch everything (updates existing cores)
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dest = Join-Path $root 'cores'
$base = 'https://buildbot.libretro.com/nightly/windows/x86_64/latest'

New-Item -ItemType Directory -Force -Path $dest | Out-Null

# the table is the list; no second copy of it to drift out of date
$cores = Select-String -Path (Join-Path $root 'systems.c') -Pattern '"([a-z0-9_]+_libretro\.dll)"' -AllMatches |
  ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique

$tmp = Join-Path $env:TEMP ("cartload-cores-" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$ok = 0; $skip = 0; $fail = @()

foreach ($core in $cores) {
  $out = Join-Path $dest $core
  if ((Test-Path $out) -and -not $Force) { $skip++; continue }
  $zip = Join-Path $tmp "$core.zip"
  try {
    Invoke-WebRequest -Uri "$base/$core.zip" -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $dest -Force
    if (Test-Path $out) { $ok++; "  fetched $core" } else { $fail += $core }
  } catch {
    $fail += $core
  }
}
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

"$ok fetched, $skip already there, $($fail.Count) unavailable"
if ($fail.Count -gt 0) {
  # a core the buildbot does not publish under that name: check it at
  # https://buildbot.libretro.com/nightly/windows/x86_64/latest/
  "not on the buildbot: $($fail -join ', ')"
}
