BeforeAll {
    $setup = Join-Path $PSScriptRoot '..\..\src_assets\windows\misc\install-parsec-vdd.ps1'
}

Describe 'Parsec privacy display driver setup' {
    BeforeEach {
        $devices = @{ Adapters = @([pscustomobject]@{ HardwareID = @('Root\Other\VDA'); ConfigManagerErrorCode = 0 }) }
        Mock Get-CimInstance { $devices.Adapters } -ParameterFilter { $ClassName -eq 'Win32_PnPEntity' }
        Mock Get-CimInstance { [pscustomobject]@{ Architecture = 9 } } -ParameterFilter { $ClassName -eq 'Win32_Processor' }
        Mock Get-CimInstance { [pscustomobject]@{ ProductType = 1; BuildNumber = '19044' } } -ParameterFilter { $ClassName -eq 'Win32_OperatingSystem' }
        Mock Invoke-WebRequest {}
        Mock Get-FileHash { @{ Hash = 'E23332448FDAF5AA017CB308DB5EF6855FAC526A7DED05D80C039404126D5362' } }
        $signer = [pscustomobject]@{ Publisher = 'Parsec Cloud, Inc.' }
        $signer | Add-Member -MemberType ScriptMethod -Name GetNameInfo -Value { $this.Publisher }
        Mock Get-AuthenticodeSignature { @{ Status = 'Valid'; SignerCertificate = $signer } }
        Mock Start-Process {
            $devices.Adapters += [pscustomobject]@{ HardwareID = @('Root\Parsec\VDA'); ConfigManagerErrorCode = 0 }
            @{ ExitCode = 0 }
        }
        Mock Start-Sleep {}
    }

    It 'previews a missing adapter without downloading or running the installer' {
        (& $setup -CheckOnly) | Should -Be 'Would install'
        Should -Invoke Invoke-WebRequest -Times 0 -Exactly
        Should -Invoke Start-Process -Times 0 -Exactly
    }

    It 'installs silently and reuses the working adapter on a second run' {
        (& $setup) | Should -Be 'Installed'
        (& $setup) | Should -Be 'Already installed'
        Should -Invoke Invoke-WebRequest -Times 1 -Exactly -ParameterFilter { $Uri -eq 'https://builds.parsec.app/vdd/parsec-vdd-0.45.0.0.exe' }
        Should -Invoke Start-Process -Times 1 -Exactly -ParameterFilter { $ArgumentList -eq '/S' -and $Wait }
    }

    It 'reports a disabled device without trying to register a duplicate' {
        $devices.Adapters += [pscustomobject]@{ HardwareID = @('Root\Parsec\VDA'); ConfigManagerErrorCode = 22 }
        { & $setup } | Should -Throw '*problem code 22*'
        Should -Invoke Invoke-WebRequest -Times 0 -Exactly
    }

    It 'skips the AMD64-only installer on ARM64' {
        Mock Get-CimInstance { [pscustomobject]@{ Architecture = 12 } } -ParameterFilter { $ClassName -eq 'Win32_Processor' }
        (& $setup) | Should -Be 'Unsupported platform'
        Should -Invoke Invoke-WebRequest -Times 0 -Exactly
    }

    It 'checks the minimum supported Windows build for the product type' -ForEach @(
        @{ Product = 1; Build = '19043'; Expected = 'Unsupported platform' }
        @{ Product = 3; Build = '17763'; Expected = 'Would install' }
    ) {
        Mock Get-CimInstance { [pscustomobject]@{ ProductType = $Product; BuildNumber = $Build } } -ParameterFilter { $ClassName -eq 'Win32_OperatingSystem' }
        (& $setup -CheckOnly) | Should -Be $Expected
    }

    It 'rejects a changed download before execution' {
        Mock Get-FileHash { @{ Hash = 'wrong' } }
        { & $setup } | Should -Throw '*SHA-256*'
        Should -Invoke Start-Process -Times 0 -Exactly
    }

    It 'rejects a different signer before execution' {
        $signer.Publisher = 'Other publisher'
        { & $setup } | Should -Throw '*signature*'
        Should -Invoke Start-Process -Times 0 -Exactly
    }

    It 'rejects an invalid Authenticode signature' {
        Mock Get-AuthenticodeSignature { @{ Status = 'NotTrusted'; SignerCertificate = $signer } }
        { & $setup } | Should -Throw '*signature*'
        Should -Invoke Start-Process -Times 0 -Exactly
    }

    It 'propagates download and installer failures' {
        Mock Start-Process { @{ ExitCode = 5 } }
        { & $setup } | Should -Throw '*returned 5*'
        Mock Invoke-WebRequest { throw 'Download failed' }
        { & $setup } | Should -Throw '*Download failed*'
        Should -Invoke Start-Process -Times 1 -Exactly
    }

    It 'requires a working device after the installer exits' {
        Mock Start-Process { @{ ExitCode = 0 } }
        { & $setup } | Should -Throw '*did not report a working Parsec display adapter*'
    }

    It 'reports a required restart' {
        Mock Start-Process { @{ ExitCode = 3010 } }
        (& $setup) | Should -Be 'Restart required'
        $devices.Adapters += [pscustomobject]@{ HardwareID = @('Root\Parsec\VDA'); ConfigManagerErrorCode = 14 }
        (& $setup) | Should -Be 'Restart required'
        Should -Invoke Start-Process -Times 1 -Exactly
    }
}
