# Mewjector-diagnose.ps1  --  one-shot "Mewjector isn't loading" diagnostic
#
# Run ON THE AFFECTED MACHINE. Best as Administrator (policy + event-log reads):
#   powershell -ExecutionPolicy Bypass -File .\Mewjector-diagnose.ps1
#   powershell -ExecutionPolicy Bypass -File .\Mewjector-diagnose.ps1 -GameDir "D:\SteamLibrary\steamapps\common\Mewgenics"
#
# Answers, in order: is the proxy present and a valid Mewjector build? did the
# game load it or the system copy? and if it was skipped, WHY: a redirect
# (KnownDLLs / .local), a trust policy (Smart App Control / WDAC / AppLocker),
# or Defender. Launch the game before running so section 2 can be captured.

param([string]$GameDir)

$ErrorActionPreference = 'SilentlyContinue'

function Section($t){ Write-Host "`n== $t ==" -ForegroundColor Cyan }

# --- Resolve the game folder: running process first (handles SD-card installs)
$proc = Get-Process Mewgenics -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $GameDir) {
    if ($proc -and $proc.Path) { $GameDir = Split-Path $proc.Path -Parent }
    else { $GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Mewgenics" }
}
$dll = Join-Path $GameDir "version.dll"

Write-Host "==== Mewjector diagnostic ====" -ForegroundColor Cyan
Write-Host "Game folder : $GameDir"

# ---------------------------------------------------------------------------
Section "1. Proxy file: present, valid, trusted?"
if (Test-Path $dll) {
    Write-Host ("present         : YES  ({0} bytes)" -f (Get-Item $dll).Length)
    Write-Host ("sha256          : {0}" -f (Get-FileHash $dll -Algorithm SHA256).Hash)
    # Validate it is actually a Mewjector proxy (x64 + expected exports) rather
    # than trusting any single build's hash -- legit rebuilds differ by a few
    # timestamp bytes, so a hash compare gives false negatives.
    $b = [IO.File]::ReadAllBytes($dll)
    $machine = [BitConverter]::ToUInt16($b, ([BitConverter]::ToInt32($b, 0x3C)) + 4)
    $archMap = @{0x8664='x64';0x14C='x86';0xAA64='ARM64'}
    $arch = $archMap[[int]$machine]
    $ascii = [Text.Encoding]::ASCII.GetString($b)
    $isProxy = ($ascii -match 'GetFileVersionInfoSizeW') -and ($ascii -match 'MJ_InstallHook')
    Write-Host ("architecture    : {0}  (must be x64 to match the game)" -f $arch)
    Write-Host ("valid Mewjector : {0}  (exports version API + MJ_ funcs)" -f $isProxy)
    $motw = Get-Content $dll -Stream Zone.Identifier -ErrorAction SilentlyContinue
    Write-Host ("mark-of-the-web : {0}  (True = downloaded, not unblocked)" -f [bool]$motw)
    Write-Host ("signature       : {0}" -f (Get-AuthenticodeSignature $dll).Status)
} else {
    Write-Host "present         : NO  (file gone -- AV quarantine? -- or wrong GameDir)" -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
Section "2. Which version.dll did the game actually load?"
if ($proc) {
    $m = $proc.Modules | Where-Object { $_.ModuleName -ieq 'version.dll' }
    if ($m) {
        Write-Host ("loaded          : {0}" -f $m.FileName)
        if ($m.FileName -like "$GameDir*") {
            Write-Host "                = PROXY loaded. Injection should work; look elsewhere." -ForegroundColor Green
        } else {
            Write-Host "                = SYSTEM copy loaded; the proxy was skipped (see 3, 4, 5)." -ForegroundColor Yellow
        }
    } else { Write-Host "Mewgenics is running but has no version.dll module (unexpected)." }
} else {
    Write-Host "Mewgenics not running -- launch it, then re-run to capture this."
}

# ---------------------------------------------------------------------------
Section "3. Was it redirected away from the app directory?"
# 3a KnownDLLs: if version.dll is listed here, the System32 copy is forced and
#    the app-dir proxy is never even looked at -- no crash, no injection.
$kd  = Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\KnownDLLs"
$hit = $kd.PSObject.Properties | Where-Object { $_.Value -match '(?i)version\.dll' }
Write-Host ("version.dll in KnownDLLs : {0}" -f [bool]$hit)
if ($hit) { $hit | ForEach-Object { Write-Host ("   {0} = {1}   (remove this entry to fix)" -f $_.Name,$_.Value) -ForegroundColor Yellow } }
# 3b .local / manifest redirection
Write-Host ("Mewgenics.exe.local      : {0}" -f (Test-Path (Join-Path $GameDir 'Mewgenics.exe.local')))
Write-Host ("Mewgenics.exe.manifest   : {0}" -f (Test-Path (Join-Path $GameDir 'Mewgenics.exe.manifest')))

# ---------------------------------------------------------------------------
Section "4. Was it blocked by a trust policy or AV?"
# 4a Smart App Control -- the registry value is authoritative (the Defender
#    cmdlet field is often blank). 0=Off 1=Enforce 2=Eval.
$sac = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\CI\Policy" -Name VerifiedAndReputablePolicyState -ErrorAction SilentlyContinue).VerifiedAndReputablePolicyState
if ($null -eq $sac) { $sac = "n/a (key not present)" }
Write-Host ("Smart App Control : {0}  (0=Off 1=Enforce 2=Eval)" -f $sac)
# 4b EVERY registered AV, not just Defender. A third-party AV (Malwarebytes,
#    ESET, Bitdefender, ...) takes over real-time protection and makes Defender
#    report RealTimeProtection=False. Do NOT read that as "no AV involved" --
#    the Get-Mp* cmdlets below only ever see Defender.
$avs = Get-CimInstance -Namespace root\SecurityCenter2 -ClassName AntiVirusProduct -ErrorAction SilentlyContinue
if ($avs) { $avs | ForEach-Object { Write-Host ("registered AV     : {0}" -f $_.displayName) } }
else      { Write-Host "registered AV     : (none reported)" }
# 4c Defender real-time + any detection naming our file
$mp = Get-MpComputerStatus
Write-Host ("Defender realtime : {0}  (False usually = a third-party AV is active)" -f $mp.RealTimeProtectionEnabled)
$det = Get-MpThreatDetection | Where-Object { $_.Resources -match 'version\.dll|Mewgenics' }
if ($det) {
    Write-Host "Defender detections naming version.dll/Mewgenics:" -ForegroundColor Yellow
    $det | Select-Object InitialDetectionTime, @{n='Resource';e={$_.Resources}} | Format-List
} else { Write-Host "Defender detections: none naming version.dll/Mewgenics" }
# 4c File ACL -- can a normal process even read the file?
Write-Host "ACL (read/full entries):"
(Get-Acl $dll).Access |
    Where-Object { $_.FileSystemRights -match 'Read|FullControl' } |
    Select-Object IdentityReference, FileSystemRights, AccessControlType |
    Format-Table -Auto | Out-String | Write-Host

# 4d Exploit Protection (Windows Security > App & browser control). A per-
#    program "Code integrity guard" or an image-load restriction rejects the
#    unsigned proxy and the loader falls through to the System32 copy with
#    no error. Checked system-wide and for Mewgenics.exe specifically.
$exeName = Split-Path (Join-Path $GameDir 'Mewgenics.exe') -Leaf
foreach ($scope in @(@{label='system default'; args=@{System=$true}},
                     @{label="program $exeName"; args=@{Name=$exeName}})) {
    $mit = $null
    $err = ''
    $splat = $scope.args
    try { $mit = Get-ProcessMitigation @splat -ErrorAction Stop } catch { $err = $_.Exception.Message }
    if ($err) { Write-Host ("Exploit Protection ({0}): not readable ({1})" -f $scope.label, $err); continue }
    if ($null -eq $mit) {
        # -Name returns nothing when the program has no per-program overrides
        # and is not running: the system default applies to it.
        Write-Host ("Exploit Protection ({0}): no per-program overrides, system default applies" -f $scope.label)
        continue
    }
    function Show($v) { if ("$v" -eq '') { 'NOTSET' } else { "$v" } }
    $cig  = Show $mit.CodeIntegrity.MicrosoftSignedOnly
    $img  = $mit.ImageLoad
    Write-Host ("Exploit Protection ({0}): CodeIntegrityGuard={1} BlockRemoteImages={2} BlockLowLabel={3} PreferSystem32={4}" -f `
        $scope.label, $cig, (Show $img.BlockRemoteImageLoads), (Show $img.BlockLowLabelImageLoads), (Show $img.PreferSystem32Images))
    if ($cig -eq 'ON' -or (Show $img.PreferSystem32Images) -eq 'ON') {
        Write-Host "   = a mitigation here rejects or bypasses unsigned app-dir DLLs; turn it OFF for Mewgenics.exe." -ForegroundColor Yellow
    }
}

# ---------------------------------------------------------------------------
Section "5. Policy block events (WDAC / AppLocker / CodeIntegrity)"
$found = $false
foreach ($log in @('Microsoft-Windows-CodeIntegrity/Operational',
                   'Microsoft-Windows-AppLocker/EXE and DLL')) {
    Get-WinEvent -LogName $log -MaxEvents 60 -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -match 'version\.dll|Mewgenics' } |
        ForEach-Object {
            $found = $true
            Write-Host ("[{0}]  {1}  id={2}" -f $log, $_.TimeCreated, $_.Id) -ForegroundColor Yellow
            Write-Host ("   " + (($_.Message -split "`n" | Select-Object -First 3) -join ' '))
        }
}
if (-not $found) { Write-Host "No CodeIntegrity/AppLocker events name version.dll or Mewgenics." }

# ---------------------------------------------------------------------------
Section "6. Injectors: what else loads into the game, and global hooks"
# 6a Third-party (non-Windows) modules in the running game. An overlay or
#    utility injected here can pre-load System32\version.dll or hook the
#    loader, so the app-dir proxy is skipped even with no policy block.
if ($proc) {
    $all   = $proc.Modules | Where-Object { $_.FileName } | Select-Object -ExpandProperty FileName
    $third = $all | Where-Object { $_ -notlike "$env:WINDIR\*" }
    Write-Host "non-Windows modules loaded in Mewgenics.exe:"
    if ($third) { $third | ForEach-Object { Write-Host "   $_" } } else { Write-Host "   (none besides the exe/game dlls)" }
    $flag = $all | Where-Object { $_ -match '(?i)rtss|afterburner|armoury|asus|adrenalin|radeon|amdihk|geforce|nvspcap|discord|overwolf|rivatuner|fraps|obs|reshade|specialk' }
    if ($flag) {
        Write-Host "SUSPECT injectors/overlays present:" -ForegroundColor Yellow
        $flag | Sort-Object -Unique | ForEach-Object { Write-Host "   $_" -ForegroundColor Yellow }
    }
} else {
    Write-Host "Mewgenics not running -- launch it and re-run to list injected modules."
}
# 6b Global injection points that load a DLL into (nearly) every process
$win = Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Windows"
Write-Host ("AppInit_DLLs      : '{0}'  (LoadAppInit_DLLs={1})" -f $win.AppInit_DLLs, $win.LoadAppInit_DLLs)
$acp = (Get-ItemProperty "HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\AppCertDlls" -ErrorAction SilentlyContinue).PSObject.Properties |
       Where-Object { $_.Name -notmatch '^PS' }
if ($acp) { Write-Host "AppCertDlls:"; $acp | ForEach-Object { Write-Host ("   {0} = {1}" -f $_.Name,$_.Value) } }
else { Write-Host "AppCertDlls       : (none)" }
# 6c AppCompat shim applied to Mewgenics.exe (a shim can redirect DLL loads)
$layerHits = @()
foreach ($rootKey in 'HKLM:','HKCU:') {
    $lp = "$rootKey\SOFTWARE\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers"
    (Get-ItemProperty $lp -ErrorAction SilentlyContinue).PSObject.Properties |
        Where-Object { $_.Name -match '(?i)Mewgenics' } |
        ForEach-Object { $layerHits += ("{0} = {1}" -f $_.Name,$_.Value) }
}
if ($layerHits) { Write-Host "AppCompat shim on Mewgenics.exe:" -ForegroundColor Yellow; $layerHits | ForEach-Object { Write-Host "   $_" -ForegroundColor Yellow } }
else { Write-Host "AppCompat shim    : none on Mewgenics.exe" }
# 6d Load order up to version.dll. Modules are listed in load order, so
#    whatever sits immediately before version.dll is a candidate for having
#    pulled in the System32 copy ahead of the app-dir proxy.
if ($proc) {
    $ordered = @($proc.Modules | Where-Object { $_.FileName } | Select-Object -ExpandProperty FileName)
    $idx = -1
    for ($i = 0; $i -lt $ordered.Count; $i++) {
        if ($ordered[$i] -match '(?i)\\version\.dll$') { $idx = $i; break }
    }
    if ($idx -ge 0) {
        Write-Host ("version.dll is module #{0} of {1} in load order. Loaded up to it:" -f ($idx+1), $ordered.Count)
        $ordered[0..$idx] | ForEach-Object { Write-Host "   $_" }
    } else {
        Write-Host "version.dll not found in the module list (unexpected)."
    }
}

# ---------------------------------------------------------------------------
Section "How to read this"
Write-Host @"
- 2 says SYSTEM copy loaded   = the proxy was NOT used. Find why in 3/4/5.
- 3 KnownDLLs = True          = signing would not help; delete that registry entry.
- 3 .local/manifest = True    = a redirect is overriding the app dir.
- 4 Smart App Control = 1     = unsigned DLLs blocked; turn SAC off (one-way)
                                or exclude the folder (the build is unsigned).
- 4 Defender detection listed = restore from quarantine + add a folder exclusion.
- 5 an AppLocker/WDAC event   = an OEM 'must be signed' policy is the gate.
- 1 present=NO / valid=False  = wrong/absent/blocked file; re-copy a good build.
- 6 overlay/injector listed   = close it (Armoury Crate, AMD/RTSS/Discord overlay)
                                and retest; it may pre-load System32 version.dll.
- 6 AppInit/AppCert/shim set  = a global injector or compat shim is redirecting loads.
- all clear but still SYSTEM  = capture a load trace (Process Monitor, filter
                                Path ends with version.dll) to see who loads it first.
"@
Write-Host "==== end ====" -ForegroundColor Cyan
