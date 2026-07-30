[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet("provision", "status")]
    [string]$Command,

    [string]$Serial,
    [string]$Adb = "adb",
    [string]$Ssid,
    [switch]$OpenNetwork
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
$WifiConnect = "/usr/share/thirdreality/script/wifi_connect"

function Invoke-AdbCommand {
    param(
        [Parameter(Mandatory = $true)][string[]]$AdbArguments,
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
        [Parameter(Mandatory = $true)][string]$ShellCommand,
        [switch]$AllowFailure
    )
    return Invoke-AdbCommand -AdbArguments @("shell", $ShellCommand) `
        -AllowFailure:$AllowFailure
}

function ConvertTo-SsidBytes {
    param([Parameter(Mandatory = $true)][string]$Value)
    [byte[]]$Bytes = [Text.Encoding]::UTF8.GetBytes($Value)
    if ($Bytes.Length -lt 1 -or $Bytes.Length -gt 32) {
        throw "SSID must contain between 1 and 32 UTF-8 bytes"
    }
    foreach ($Character in $Value.ToCharArray()) {
        $CodePoint = [int]$Character
        if ($CodePoint -lt 32 -or $CodePoint -eq 127) {
            throw "SSID must not contain control characters"
        }
    }
    if ($Value.Contains('"') -or $Value.Contains('\')) {
        throw "SSID cannot contain quotes or backslashes"
    }
    return ,$Bytes
}

function Read-WifiPassword {
    if ($OpenNetwork) {
        # Command substitution on the device strips this newline to an empty
        # password while keeping the adb-pushed file non-empty.
        return ,([byte[]]@(10))
    }
    $SecurePassword = Read-Host "Wi-Fi password" -AsSecureString
    $Bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR(
        $SecurePassword
    )
    try {
        $PlainPassword = [Runtime.InteropServices.Marshal]::PtrToStringBSTR(
            $Bstr
        )
        if ($PlainPassword.Length -lt 8 -or $PlainPassword.Length -gt 63) {
            throw "Wi-Fi password must contain 8 to 63 characters"
        }
        foreach ($Character in $PlainPassword.ToCharArray()) {
            $CodePoint = [int]$Character
            if ($CodePoint -lt 32 -or $CodePoint -gt 126) {
                throw "Wi-Fi password must contain printable ASCII"
            }
        }
        if ($PlainPassword.Contains('"') -or $PlainPassword.Contains('\')) {
            throw "Wi-Fi password cannot contain quotes or backslashes"
        }
        return ,[Text.Encoding]::ASCII.GetBytes($PlainPassword)
    }
    finally {
        [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($Bstr)
        $PlainPassword = $null
        $SecurePassword = $null
    }
}

function Write-RemoteFile {
    param(
        [Parameter(Mandatory = $true)][string]$RemotePath,
        [Parameter(Mandatory = $true)][byte[]]$Content
    )
    $LocalPath = Join-Path ([IO.Path]::GetTempPath()) `
        ("wifi-provision-" + [Guid]::NewGuid().ToString("N"))
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

switch ($Command) {
    "status" {
        $Arguments = @()
        if ($Serial) {
            $Arguments += @("-s", $Serial)
        }
        $Arguments += @("shell", "wpa_cli -i wlan0 status")
        & $Adb @Arguments
        exit $LASTEXITCODE
    }

    "provision" {
        if (-not $Ssid) {
            throw "provision requires -Ssid"
        }
        [byte[]]$SsidBytes = ConvertTo-SsidBytes $Ssid
        [byte[]]$PasswordBytes = Read-WifiPassword
        $Token = [Guid]::NewGuid().ToString("N")
        $SsidPath = "/tmp/wifi-provision-$Token.ssid"
        $PasswordPath = "/tmp/wifi-provision-$Token.psk"
        try {
            Write-RemoteFile $SsidPath $SsidBytes
            Write-RemoteFile $PasswordPath $PasswordBytes
            $ShellCommand = `
                'SSID="$(cat {0})"; PSK="$(cat {1})"; rm -f {0} {1}; exec {2} connect "$SSID" "$PSK"' `
                -f $SsidPath, $PasswordPath, $WifiConnect
            Invoke-AdbShell $ShellCommand | Out-Null
        }
        finally {
            [Array]::Clear($PasswordBytes, 0, $PasswordBytes.Length)
            Invoke-AdbShell "rm -f $SsidPath $PasswordPath" -AllowFailure |
                Out-Null
        }
        Write-Host "Provisioned Wi-Fi network $Ssid"
    }
}
