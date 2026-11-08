param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("AdapterInfo", "AdapterOn", "AdapterOff", "UtcOffset",
                 "InspectVolume", "WriteFile", "BluetoothEvents",
                 "CaptureInfo", "WaitCapture")]
    [string]$Action,
    [string]$Path,
    [string]$Source,
    [string]$StartUtc,
    [string]$Vid = "2357",
    [string]$ProductId = "0604",
    [long]$MinimumLength = -1,
    [int]$TimeoutSeconds = 20
)

$ErrorActionPreference = "Stop"

function Get-TestAdapter {
    $needle = "VID_$Vid&PID_$ProductId"
    $devices = @(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -like "*$needle*"
    })
    if ($devices.Count -ne 1) {
        throw "Expected one $needle adapter, found $($devices.Count)"
    }
    return $devices[0]
}

switch ($Action) {
    "AdapterInfo" {
        Get-TestAdapter | Select-Object Status, Class, FriendlyName, InstanceId |
            ConvertTo-Json -Compress
    }
    "AdapterOn" {
        Get-TestAdapter | Enable-PnpDevice -Confirm:$false
    }
    "AdapterOff" {
        Get-TestAdapter | Disable-PnpDevice -Confirm:$false
    }
    "UtcOffset" {
        [pscustomobject]@{
            capturedUtc = [DateTime]::UtcNow.ToString("o")
            timezoneId = [TimeZoneInfo]::Local.Id
            offsetMinutes = [int][TimeZoneInfo]::Local.GetUtcOffset(
                [DateTime]::Now
            ).TotalMinutes
        } | ConvertTo-Json -Compress
    }
    "InspectVolume" {
        if (-not $Path) { throw "-Path is required" }
        Get-ChildItem -Force -Recurse -LiteralPath $Path | ForEach-Object {
            [pscustomobject]@{
                path = $_.FullName
                length = if ($_.PSIsContainer) { $null } else { $_.Length }
                attributes = $_.Attributes.ToString()
                modifiedLocal = $_.LastWriteTime.ToString("o")
                modifiedUtc = $_.LastWriteTimeUtc.ToString("o")
            }
        } | ConvertTo-Json -Compress
    }
    "WriteFile" {
        if (-not $Path -or -not $Source) {
            throw "-Path and -Source are required"
        }
        $bytes = [IO.File]::ReadAllBytes($Source)
        $stream = [IO.File]::Open($Path, [IO.FileMode]::Create,
            [IO.FileAccess]::Write, [IO.FileShare]::Read)
        try {
            $stream.Write($bytes, 0, $bytes.Length)
            $stream.Flush($true)
        } finally {
            $stream.Dispose()
        }
        Get-Item -Force -LiteralPath $Path | Select-Object FullName, Length,
            LastWriteTime, LastWriteTimeUtc | ConvertTo-Json -Compress
    }
    "BluetoothEvents" {
        $start = if ($StartUtc) {
            [DateTime]::Parse($StartUtc).ToUniversalTime()
        } else {
            [DateTime]::UtcNow.AddMinutes(-10)
        }
        Get-WinEvent -FilterHashtable @{LogName = "System"; StartTime = $start} |
            Where-Object {
                $_.ProviderName -match "BTH|Bluetooth|Kernel-PnP|UserPnp"
            } | Select-Object TimeCreated, ProviderName, Id, LevelDisplayName,
                Message | ConvertTo-Json -Compress
    }
    "CaptureInfo" {
        if (-not $Path) { throw "-Path is required" }
        $item = Get-Item -LiteralPath $Path
        [pscustomobject]@{
            length = $item.Length
            modifiedUtc = $item.LastWriteTimeUtc.ToString("o")
            line = [string](Get-Content -Encoding UTF8 -Tail 1 -LiteralPath $Path)
        } | ConvertTo-Json -Compress
    }
    "WaitCapture" {
        if (-not $Path) { throw "-Path is required" }
        if ($MinimumLength -lt 0) { throw "-MinimumLength is required" }
        $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
        do {
            $item = Get-Item -LiteralPath $Path
            if ($item.Length -gt $MinimumLength) { break }
            Start-Sleep -Milliseconds 100
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($item.Length -le $MinimumLength) {
            throw "Capture did not grow beyond $MinimumLength bytes"
        }
        [pscustomobject]@{
            length = $item.Length
            modifiedUtc = $item.LastWriteTimeUtc.ToString("o")
            line = [string](Get-Content -Encoding UTF8 -Tail 1 -LiteralPath $Path)
        } | ConvertTo-Json -Compress
    }
}
