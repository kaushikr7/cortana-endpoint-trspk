[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet("provision", "status", "rotate-credential")]
    [string]$Command,

    [string]$Serial,
    [string]$Adb = "adb",
    [string]$Endpoint,
    [string]$SatelliteId,
    [string]$ExpectedAreaId,
    [string]$EndpointBinary = "/usr/bin/linux-voice-assistant-cpp"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RemoteDirectory = "/data/cortana"
$RemoteConfig = "$RemoteDirectory/config.json"
$RemoteCredential = "$RemoteDirectory/credential"
$EndpointService = "/etc/init.d/S99ha-speaker"

function Invoke-AdbCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$AdbArguments,
        [switch]$AllowFailure
    )

    $Arguments = @()
    if ($Serial) {
        $Arguments += @("-s", $Serial)
    }
    $Arguments += $AdbArguments
    & $Adb @Arguments
    $ExitCode = $LASTEXITCODE
    if (-not $AllowFailure -and $ExitCode -ne 0) {
        throw "ADB command failed with exit code $ExitCode"
    }
    return $ExitCode
}

function Invoke-AdbShell {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ShellCommand,
        [switch]$AllowFailure
    )
    return Invoke-AdbCommand -AdbArguments @("shell", $ShellCommand) `
        -AllowFailure:$AllowFailure
}

function Assert-Identifier {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value,
        [Parameter(Mandatory = $true)]
        [string]$Pattern,
        [Parameter(Mandatory = $true)]
        [string]$Description
    )
    if ($Value -notmatch $Pattern) {
        throw $Description
    }
}

function Normalize-Endpoint {
    param([Parameter(Mandatory = $true)][string]$Value)

    $Uri = $null
    if (-not [Uri]::TryCreate($Value, [UriKind]::Absolute, [ref]$Uri) -or
        $Uri.Scheme -ne "https" -or
        -not $Uri.Host -or
        $Uri.Host -notmatch '^[A-Za-z0-9.-]+$' -or
        $Uri.UserInfo -or
        $Uri.AbsolutePath -ne "/" -or
        $Uri.Query -or
        $Uri.Fragment) {
        throw "endpoint must be an HTTPS origin without a path, query, or credentials"
    }
    return $Value.TrimEnd("/")
}

function Read-Credential {
    $SecureCredential = Read-Host "Device credential" -AsSecureString
    $Bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR(
        $SecureCredential
    )
    try {
        $PlainCredential = [Runtime.InteropServices.Marshal]::PtrToStringBSTR(
            $Bstr
        )
        if ($PlainCredential.Length -lt 32) {
            throw "credential must contain at least 32 characters"
        }
        if ($PlainCredential.Length -gt 4096) {
            throw "credential is too large"
        }
        foreach ($Character in $PlainCredential.ToCharArray()) {
            $CodePoint = [int]$Character
            if ($CodePoint -lt 33 -or $CodePoint -gt 126) {
                throw "credential must contain printable ASCII without whitespace"
            }
        }
        $Bytes = [Text.Encoding]::ASCII.GetBytes($PlainCredential)
        return ,$Bytes
    }
    finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($Bstr)
        $PlainCredential = $null
        $SecureCredential = $null
    }
}

function Write-RemoteFile {
    param(
        [Parameter(Mandatory = $true)][string]$RemotePath,
        [Parameter(Mandatory = $true)][byte[]]$Content
    )

    $LocalPath = Join-Path ([IO.Path]::GetTempPath()) `
        ("cortana-provision-" + [Guid]::NewGuid().ToString("N"))
    try {
        [IO.File]::WriteAllBytes($LocalPath, $Content)
        Invoke-AdbCommand -AdbArguments @("push", $LocalPath, $RemotePath) |
            Out-Null
        Invoke-AdbShell "chmod 0600 $RemotePath" | Out-Null
    }
    finally {
        if (Test-Path -LiteralPath $LocalPath) {
            try {
                [IO.File]::WriteAllBytes(
                    $LocalPath,
                    (New-Object byte[] $Content.Length)
                )
            }
            finally {
                Remove-Item -LiteralPath $LocalPath -Force
            }
        }
    }
}

function Initialize-RemoteDirectory {
    Invoke-AdbShell `
        "umask 077; mkdir -p $RemoteDirectory; chmod 0700 $RemoteDirectory" |
        Out-Null
}

function Test-RemoteConfiguration {
    Invoke-AdbShell "$EndpointBinary --check-config" | Out-Null
}

function Restart-Endpoint {
    Invoke-AdbShell "$EndpointService voice-assistant restart" | Out-Null
}

switch ($Command) {
    "status" {
        $Arguments = @()
        if ($Serial) {
            $Arguments += @("-s", $Serial)
        }
        $Arguments += @("shell", "$EndpointBinary --status")
        & $Adb @Arguments
        exit $LASTEXITCODE
    }

    "provision" {
        if (-not $Endpoint -or -not $SatelliteId) {
            throw "provision requires -Endpoint and -SatelliteId"
        }
        $NormalizedEndpoint = Normalize-Endpoint $Endpoint
        Assert-Identifier $SatelliteId '^[a-z][a-z0-9-]{0,63}$' `
            "satellite ID must match [a-z][a-z0-9-]{0,63}"
        if ($ExpectedAreaId) {
            Assert-Identifier $ExpectedAreaId '^[a-z][a-z0-9_]{0,63}$' `
                "expected area ID must match [a-z][a-z0-9_]{0,63}"
        }

        $Document = [ordered]@{
            schemaVersion = 1
            endpoint = $NormalizedEndpoint
            satelliteId = $SatelliteId
        }
        if ($ExpectedAreaId) {
            $Document.expectedAreaId = $ExpectedAreaId
        }
        $Utf8 = New-Object -TypeName System.Text.UTF8Encoding `
            -ArgumentList $false
        [byte[]]$ConfigBytes = $Utf8.GetBytes(
            (($Document | ConvertTo-Json -Compress) + "`n")
        )
        [byte[]]$CredentialBytes = Read-Credential
        $Token = [Guid]::NewGuid().ToString("N")
        $ConfigTemporary = "$RemoteDirectory/.config.json.$Token.tmp"
        $CredentialTemporary = "$RemoteDirectory/.credential.$Token.tmp"

        Initialize-RemoteDirectory
        try {
            Write-RemoteFile $ConfigTemporary $ConfigBytes
            Write-RemoteFile $CredentialTemporary $CredentialBytes
            Invoke-AdbShell `
                "mv -f $CredentialTemporary $RemoteCredential; mv -f $ConfigTemporary $RemoteConfig; chmod 0600 $RemoteCredential $RemoteConfig" |
                Out-Null
        }
        finally {
            [Array]::Clear($CredentialBytes, 0, $CredentialBytes.Length)
            Invoke-AdbShell `
                "rm -f $ConfigTemporary $CredentialTemporary" `
                -AllowFailure | Out-Null
        }

        Test-RemoteConfiguration
        Restart-Endpoint
        Write-Host "Provisioned Cortana endpoint $SatelliteId"
    }

    "rotate-credential" {
        [byte[]]$CredentialBytes = Read-Credential
        $Token = [Guid]::NewGuid().ToString("N")
        $CredentialTemporary = "$RemoteDirectory/.credential.$Token.tmp"

        Initialize-RemoteDirectory
        try {
            Write-RemoteFile $CredentialTemporary $CredentialBytes
            Invoke-AdbShell `
                "mv -f $CredentialTemporary $RemoteCredential; chmod 0600 $RemoteCredential" |
                Out-Null
        }
        finally {
            [Array]::Clear($CredentialBytes, 0, $CredentialBytes.Length)
            Invoke-AdbShell "rm -f $CredentialTemporary" -AllowFailure |
                Out-Null
        }

        Test-RemoteConfiguration
        Restart-Endpoint
        Write-Host "Rotated Cortana endpoint credential"
    }
}
