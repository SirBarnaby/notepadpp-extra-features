# Install-KEX.ps1 - installs Notepad++ KEX (four views, snap-out windows and the built-in KEX
# features) into your installed Notepad++, and keeps it up to date.
#
#   Install-KEX.cmd      install from this folder, then offer automatic updates
#   Uninstall-KEX.cmd    put the official notepad++.exe back and remove the update task
#   -Target DIR          use a Notepad++ folder that isn't found automatically
#
# Your settings, sessions and other plugins are left alone. The official notepad++.exe of the same
# version is kept next to it as notepad++.exe.kex-original.
#
# Automatic updates: a daily Task Scheduler task ("KEX Notepad++ update check") runs a copy of this
# script with -Check. When the GitHub repository named in kex-repo.txt publishes a KEX build of a
# new Notepad++ release, it downloads that build and the official signed installer of the same
# release, waits until Notepad++ is closed, runs the official installer, then puts the KEX build on
# top. Log: %LOCALAPPDATA%\KEX-Notepad++\update.log
param([switch]$Uninstall, [switch]$Check, [string]$Target, [switch]$Yes)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$State = Join-Path $env:LOCALAPPDATA 'KEX-Notepad++'
$Log = Join-Path $State 'update.log'
$TaskName = 'KEX Notepad++ update check'
$Upstream = 'https://api.github.com/repos/notepad-plus-plus/notepad-plus-plus'

function Log($m) {
    $line = "$(Get-Date -Format s)  $m"
    Write-Host $line
    if (Test-Path $State) { Add-Content -Path $Log -Value $line }
}

function Notify($text) {
    Log $text
    if (-not $Check) { return }
    try {
        Add-Type -AssemblyName System.Windows.Forms, System.Drawing
        $n = New-Object System.Windows.Forms.NotifyIcon
        $n.Icon = [System.Drawing.SystemIcons]::Information
        $n.BalloonTipTitle = 'Notepad++ KEX'
        $n.BalloonTipText = $text
        $n.Visible = $true
        $n.ShowBalloonTip(15000)
        Start-Sleep -Seconds 10
        $n.Dispose()
    } catch { }
}

# "v8.9" -> 8.9.0.0, so versions from tags and from exe files compare as equals
function Norm([string]$v) {
    $p = @(($v.Trim() -replace '^v', '' -replace '-kex$', '').Split('.') | ForEach-Object { [int]$_ })
    while ($p.Count -lt 4) { $p += 0 }
    return [version]($p[0..3] -join '.')
}

function Get-Version($file) {
    if (-not (Test-Path $file)) { return [version]'0.0.0.0' }
    $v = (Get-Item $file).VersionInfo
    return Norm ('{0}.{1}.{2}.{3}' -f $v.FileMajorPart, $v.FileMinorPart, $v.FileBuildPart, $v.FilePrivatePart)
}

# a KEX build has the window class for snapped-out views; the official exe doesn't
function Test-Kex($file) {
    if (-not (Test-Path $file)) { return $false }
    return [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($file)).Contains('KexNppViewWindow')
}

function Find-Npp {
    if ($Target) { return $Target }
    foreach ($key in 'HKLM:\SOFTWARE\Notepad++', 'HKLM:\SOFTWARE\WOW6432Node\Notepad++') {
        $dir = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).'(default)'
        if ($dir -and (Test-Path (Join-Path $dir 'notepad++.exe'))) { return $dir }
    }
    foreach ($dir in (Join-Path $env:ProgramFiles 'Notepad++'), (Join-Path ${env:ProgramFiles(x86)} 'Notepad++')) {
        if (Test-Path (Join-Path $dir 'notepad++.exe')) { return $dir }
    }
    return (Join-Path $env:ProgramFiles 'Notepad++')   # not installed: the official installer puts it here
}

function Get-Headers([switch]$Auth) {
    $h = @{ 'User-Agent' = 'KEX-Notepad++' }
    $tokenFile = Join-Path $State 'token.dat'
    if ($Auth -and (Test-Path $tokenFile)) {
        $s = Get-Content $tokenFile | ConvertTo-SecureString
        $h.Authorization = 'token ' + [Runtime.InteropServices.Marshal]::PtrToStringBSTR([Runtime.InteropServices.Marshal]::SecureStringToBSTR($s))
    }
    return $h
}

function Get-Repo { return (Get-Content (Join-Path $State 'kex-repo.txt') -Raw).Trim() }

function Get-Asset($asset, $file, [switch]$Auth) {
    $h = Get-Headers -Auth:$Auth
    if (-not $h.Authorization) {
        Invoke-WebRequest $asset.browser_download_url -OutFile $file -UseBasicParsing -Headers $h
        return
    }
    # private repo: ask the API for the file, then fetch the storage link it redirects to without the token
    $req = [Net.HttpWebRequest]::Create($asset.url)
    $req.AllowAutoRedirect = $false
    $req.UserAgent = $h.'User-Agent'
    $req.Accept = 'application/octet-stream'
    $req.Headers['Authorization'] = $h.Authorization
    $resp = $req.GetResponse()
    $location = $resp.Headers['Location']
    $resp.Close()
    Invoke-WebRequest $location -OutFile $file -UseBasicParsing
}

function Read-State {
    $f = Join-Path $State 'state.json'
    $s = @{ installed = ''; warned = '' }
    if (Test-Path $f) { (Get-Content $f -Raw | ConvertFrom-Json).PSObject.Properties | ForEach-Object { $s[$_.Name] = "$($_.Value)" } }
    return $s
}

function Save-State($s) { [pscustomobject]$s | ConvertTo-Json | Set-Content (Join-Path $State 'state.json') }

function Wait-NppClosed($why) {
    $told = $false
    while (Get-Process -Name 'notepad++' -ErrorAction SilentlyContinue) {
        if (-not $told) {
            if ($Check) { Notify $why } else { Write-Host 'Close every Notepad++ window to continue...' -ForegroundColor Yellow }
            $told = $true
        }
        Start-Sleep -Seconds $(if ($Check) { 30 } else { 2 })
    }
}

# Puts the KEX build in $pkg (an unpacked KEX zip) into the Notepad++ folder $npp. When the installed
# Notepad++ is older than the build, the official installer of the build's release is run first.
# a KEX build with the suite built in; it registers the quick note's window class
function Test-BuiltinSuite($file) {
    $text = [Text.Encoding]::Unicode.GetString([IO.File]::ReadAllBytes($file))
    return $text.Contains('KexQuickNote')
}

# The KEX features used to be the plugin plugins\KexSuite. A KEX build with them built in never loads
# that folder, so it is removed (the package only carries an empty KexSuite.dll for older copies of
# this script, which expect the file).
function Remove-OldPlugin($npp) {
    $old = Join-Path $npp 'plugins\KexSuite'
    if (-not (Test-Path $old)) { return }
    if (Test-Path (Join-Path $old 'kex-created-disableNppAutoUpdate')) {
        New-Item -ItemType File -Force -Path (Join-Path $npp 'kex-created-disableNppAutoUpdate') | Out-Null
    }
    Remove-Item $old -Recurse -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path $old)) { Log 'Removed the old KEX Suite plugin folder; its features are built into Notepad++ now.' }
}

# Windows keeps showing an exe's old icon (Start menu, taskbar pins) from its icon cache; refresh it
# so the KEX badge, or the plain icon after an uninstall, shows at once.
function Update-IconCache {
    $tool = Join-Path $env:SystemRoot 'System32\ie4uinit.exe'
    if (Test-Path $tool) { Start-Process $tool -ArgumentList '-show' -WindowStyle Hidden -Wait -ErrorAction SilentlyContinue }
}

function Install-Package($pkg, $npp) {
    $exe = Join-Path $npp 'notepad++.exe'
    $backup = Join-Path $npp 'notepad++.exe.kex-original'
    $newExe = Join-Path $pkg 'notepad++.exe'
    $want = Get-Version $newExe
    $have = Get-Version $exe
    if ($have -gt $want) { throw "Your Notepad++ $have is newer than this KEX build ($want). Use a KEX build of $have." }

    $installer = $null
    if ($have -lt $want) {
        $tag = (Get-Content (Join-Path $pkg 'kex-version.txt') -Raw).Trim()
        $rel = Invoke-RestMethod "$Upstream/releases/tags/$tag" -Headers (Get-Headers) -UseBasicParsing
        $asset = $rel.assets | Where-Object { $_.name -match '^npp\.[\d.]+\.Installer\.x64\.exe$' } | Select-Object -First 1
        if (-not $asset) { throw "The official Notepad++ $tag release has no x64 installer." }
        $installer = Join-Path $env:TEMP $asset.name
        Log "Downloading the official $($asset.name)..."
        Get-Asset $asset $installer
        $sig = Get-AuthenticodeSignature $installer
        if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Notepad\+\+') {
            Remove-Item $installer -ErrorAction SilentlyContinue
            throw "$($asset.name) isn't validly signed by Notepad++ ($($sig.Status)), so it wasn't run."
        }
    }

    Wait-NppClosed "Notepad++ KEX $want is ready. It installs when you close Notepad++."
    if ($installer) {
        Log "Running the official Notepad++ $want installer..."
        $p = Start-Process $installer -ArgumentList '/S' -Wait -PassThru
        Remove-Item $installer -ErrorAction SilentlyContinue
        if ($p.ExitCode -ne 0) { throw "The official installer stopped with code $($p.ExitCode)." }
        Copy-Item $exe $backup -Force
    }
    elseif (-not (Test-Kex $exe)) { Copy-Item $exe $backup -Force }

    Copy-Item $newExe $exe -Force
    Update-IconCache
    Remove-OldPlugin $npp
    $noUpdate = Join-Path $npp 'disableNppAutoUpdate.xml'
    if (-not (Test-Path $noUpdate)) {
        Copy-Item (Join-Path $pkg 'disableNppAutoUpdate.xml') $noUpdate
        New-Item -ItemType File -Force -Path (Join-Path $npp 'kex-created-disableNppAutoUpdate') | Out-Null
    }
    Log "Installed Notepad++ $want with KEX into $npp."
}

function Enable-AutoUpdate($npp) {
    New-Item -ItemType Directory -Force -Path $State | Out-Null
    Copy-Item $PSCommandPath (Join-Path $State 'Install-KEX.ps1') -Force
    Copy-Item (Join-Path $Here 'kex-repo.txt') (Join-Path $State 'kex-repo.txt') -Force
    $repo = Get-Repo
    if (-not $repo) { throw 'This build has no kex-repo.txt, so it has nowhere to check for updates.' }
    try { Invoke-RestMethod "https://api.github.com/repos/$repo" -Headers (Get-Headers -Auth) -UseBasicParsing | Out-Null }
    catch {
        Write-Host "$repo is private or unreachable. Paste a GitHub token that can read it (fine-grained token, Contents: read-only)."
        Read-Host 'Token' -AsSecureString | ConvertFrom-SecureString | Set-Content (Join-Path $State 'token.dat')
        Invoke-RestMethod "https://api.github.com/repos/$repo" -Headers (Get-Headers -Auth) -UseBasicParsing | Out-Null
    }
    $script = Join-Path $State 'Install-KEX.ps1'
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$script`" -Check -Target `"$npp`""
    $trigger = New-ScheduledTaskTrigger -Daily -At '12:00'
    $settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 23) -MultipleInstances IgnoreNew
    $principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Highest
    Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Force | Out-Null
    Log "Automatic updates on: checks $repo daily (Task Scheduler: '$TaskName')."
}

function Do-Check {
    $npp = Find-Npp
    $exe = Join-Path $npp 'notepad++.exe'
    $repo = Get-Repo
    $rel = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" -Headers (Get-Headers -Auth) -UseBasicParsing
    $asset = $rel.assets | Where-Object { $_.name -like 'Notepad++-*-KEX.zip' } | Select-Object -First 1
    if (-not $asset) { Log "The latest release of $repo has no KEX zip."; return }
    $want = Norm $rel.tag_name
    $stamp = "$($asset.id)/$($asset.updated_at)"
    $have = Get-Version $exe
    $isKex = Test-Kex $exe
    $st = Read-State
    if ($isKex -and (Test-BuiltinSuite $exe)) { Remove-OldPlugin $npp }

    if ($isKex -and $have -eq $want -and -not $st.installed) { $st.installed = $stamp; Save-State $st; Log "Up to date: $have."; return }
    if ($isKex -and $have -eq $want -and $st.installed -eq $stamp) { Log "Up to date: $have."; return }
    if ($have -gt $want) {
        if (-not $isKex -and $st.warned -ne "$have") {
            Notify "Notepad++ $have is installed without four views; its KEX build isn't out yet. It installs automatically once it is."
            $st.warned = "$have"; Save-State $st
        }
        else { Log "Installed $have is newer than the latest KEX build $want." }
        return
    }

    Log "Downloading $($asset.name)..."
    $zip = Join-Path $State $asset.name
    $unpacked = Join-Path $State 'new'
    Remove-Item $unpacked -Recurse -Force -ErrorAction SilentlyContinue
    Get-Asset $asset $zip -Auth
    Expand-Archive $zip $unpacked -Force
    $pkg = (Get-ChildItem $unpacked -Directory | Select-Object -First 1).FullName
    Install-Package $pkg $npp
    Copy-Item (Join-Path $pkg 'Install-KEX.ps1') (Join-Path $State 'Install-KEX.ps1') -Force   # newer copy of this script
    Remove-Item $zip, $unpacked -Recurse -Force -ErrorAction SilentlyContinue
    $st.installed = $stamp; $st.warned = ''; Save-State $st
    Notify "Updated to Notepad++ KEX $want."
}

function Do-Uninstall {
    $npp = Find-Npp
    $exe = Join-Path $npp 'notepad++.exe'
    $backup = Join-Path $npp 'notepad++.exe.kex-original'
    $pluginDir = Join-Path $npp 'plugins\KexSuite'
    Wait-NppClosed ''
    if (Test-Path $backup) {
        if ((Get-Version $backup) -eq (Get-Version $exe)) { Move-Item $backup $exe -Force }
        else { Remove-Item $backup }
    }
    if (Test-Kex $exe) { Write-Host 'The official notepad++.exe could not be restored; reinstall Notepad++ from notepad-plus-plus.org.' -ForegroundColor Yellow }
    Update-IconCache
    foreach ($m in (Join-Path $npp 'kex-created-disableNppAutoUpdate'), (Join-Path $pluginDir 'kex-created-disableNppAutoUpdate')) {
        if (Test-Path $m) { Remove-Item (Join-Path $npp 'disableNppAutoUpdate.xml') -ErrorAction SilentlyContinue; Remove-Item $m }
    }
    Remove-Item $pluginDir -Recurse -Force -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
    Remove-Item $State -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host 'KEX removed. Notepad++ is back to normal.' -ForegroundColor Green
}

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin -and -not $Check) {
    $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    if ($Uninstall) { $argList += ' -Uninstall' }
    if ($Target) { $argList += " -Target `"$Target`"" }
    if ($Yes) { $argList += ' -Yes' }
    Start-Process powershell -Verb RunAs -ArgumentList $argList
    exit 0
}

try {
    if ($Check) {
        New-Item -ItemType Directory -Force -Path $State | Out-Null
        Do-Check
        exit 0
    }
    if ($Uninstall) { Do-Uninstall }
    else {
        $npp = Find-Npp
        Write-Host "Notepad++ folder: $npp"
        Get-ChildItem $Here -Recurse -File | Unblock-File
        New-Item -ItemType Directory -Force -Path $State | Out-Null
        Install-Package $Here $npp
        $repoFile = Join-Path $Here 'kex-repo.txt'
        if (-not (Test-Path $repoFile) -or -not (Get-Content $repoFile -Raw).Trim()) {
            Write-Host 'This build has no GitHub repository to update from, so automatic updates are off.'
        }
        else {
            $answer = if ($Yes) { 'y' } else { Read-Host 'Install new Notepad++ releases with KEX automatically? [Y/n]' }
            if ($answer -notmatch '^[nN]') { Enable-AutoUpdate $npp }
        }
        Write-Host 'Done. Start Notepad++ as usual; the KEX settings are under Settings > KEX Features.' -ForegroundColor Green
    }
}
catch {
    Log "ERROR: $($_.Exception.Message)"
    if ($Check) { Notify "The update check failed: $($_.Exception.Message)"; exit 1 }
    Write-Host $_.Exception.Message -ForegroundColor Red
}
if (-not $Check) { Read-Host 'Press Enter to close' | Out-Null }
