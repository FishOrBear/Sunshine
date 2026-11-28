# Check or install the Parsec adapter used for on-demand privacy displays.
[CmdletBinding()]
param([switch]$CheckOnly)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Get-ParsecDeviceProblemCode {
    Get-CimInstance -ClassName Win32_PnPEntity -Filter "Present=TRUE AND PNPDeviceID LIKE 'ROOT%'" -Property HardwareID, ConfigManagerErrorCode |
        Where-Object { $_.HardwareID -contains 'Root\Parsec\VDA' } |
        Select-Object -ExpandProperty ConfigManagerErrorCode
}

$mutex = $null
$owned = $false
$installer = $null
try {
    # Serialize detection and installation so concurrent callers cannot register duplicate adapters.
    if (!$CheckOnly) {
        $mutex = [Threading.Mutex]::new($false, 'Global\SunshineParsecVddInstall')
        try { $owned = $mutex.WaitOne([TimeSpan]::FromMinutes(3)) }
        catch [Threading.AbandonedMutexException] { $owned = $true }
        if (!$owned) { throw 'Another Parsec display driver installation is still running.' }
    }

    $problems = @(Get-ParsecDeviceProblemCode)
    if ($problems -contains 0) { return 'Already installed' }
    if ($problems -contains 14) { return 'Restart required' }
    if ($problems.Count) {
        throw "The Parsec display adapter is present with Windows problem code $($problems -join ', '). Check the device in Device Manager."
    }

    # The pinned driver INF supports NTamd64. Parsec supports Windows 10 21H2+
    # on desktops and Windows Server 2019+ on servers.
    $architectures = (Get-CimInstance -ClassName Win32_Processor -Property Architecture).Architecture
    $os = Get-CimInstance -ClassName Win32_OperatingSystem -Property BuildNumber, ProductType
    $minimumBuild = if ($os.ProductType -eq 1) { 19044 } else { 17763 }
    if (![Environment]::Is64BitOperatingSystem -or $architectures -notcontains 9 -or [int]$os.BuildNumber -lt $minimumBuild) {
        return 'Unsupported platform'
    }
    if ($CheckOnly) { return 'Would install' }

    $uri = 'https://builds.parsec.app/vdd/parsec-vdd-0.45.0.0.exe'
    $sha256 = 'E23332448FDAF5AA017CB308DB5EF6855FAC526A7DED05D80C039404126D5362'
    # Use an administrator-writable directory for the verified executable.
    $installer = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) "sunshine-parsec-vdd-$([guid]::NewGuid()).exe"
    $protocols = [Net.ServicePointManager]::SecurityProtocol
    try {
        if ($protocols -ne [Net.SecurityProtocolType]::SystemDefault) {
            [Net.ServicePointManager]::SecurityProtocol = $protocols -bor [Net.SecurityProtocolType]::Tls12
        }
        Invoke-WebRequest -UseBasicParsing -Uri $uri -OutFile $installer -TimeoutSec 90
    } finally {
        [Net.ServicePointManager]::SecurityProtocol = $protocols
    }
    if ((Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $sha256) {
        throw 'The downloaded Parsec display driver failed SHA-256 verification.'
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $installer
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.GetNameInfo('SimpleName', $false) -ne 'Parsec Cloud, Inc.') {
        throw 'The downloaded display driver does not have a valid Parsec Cloud, Inc. signature.'
    }

    $process = Start-Process -FilePath $installer -ArgumentList '/S' -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -eq 3010) { return 'Restart required' }
    if ($process.ExitCode -ne 0) { throw "The Parsec display driver installer returned $($process.ExitCode)." }

    # The installer can exit before Windows finishes binding the device.
    for ($i = 0; $i -lt 30; $i++) {
        $problems = @(Get-ParsecDeviceProblemCode)
        if ($problems -contains 0) { return 'Installed' }
        if ($problems -contains 14) { return 'Restart required' }
        Start-Sleep -Milliseconds 500
    }
    throw 'The installer finished but Windows did not report a working Parsec display adapter. Check Device Manager.'
} finally {
    try {
        if ($installer -and (Test-Path -LiteralPath $installer)) {
            Remove-Item -LiteralPath $installer -Force
        }
    } finally {
        if ($owned) { $mutex.ReleaseMutex() }
        if ($mutex) { $mutex.Dispose() }
    }
}
