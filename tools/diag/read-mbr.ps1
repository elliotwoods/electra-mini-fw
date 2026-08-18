# Read-only diagnostic for the Electra One Mini USB DISK MODE volume.
# Reads a handful of sectors and reports what is actually on them.
# Writes nothing to the device. Requires Administrator (raw disk access does).

$ErrorActionPreference = 'Stop'

function Get-Sector {
    param([System.IO.FileStream]$Stream, [long]$Lba)
    $buf = New-Object byte[] 512
    $Stream.Position = $Lba * 512
    $n = $Stream.Read($buf, 0, 512)
    if ($n -ne 512) { throw "short read at LBA $Lba ($n bytes)" }
    return $buf
}

function Show-Hex {
    param([byte[]]$B, [int]$Off, [int]$Len, [string]$Label)
    $hex = ($B[$Off..($Off + $Len - 1)] | ForEach-Object { $_.ToString('X2') }) -join ' '
    "  {0,-14} {1}" -f $Label, $hex
}

# Find the Electra disk rather than assuming a number.
$disk = Get-Disk | Where-Object { $_.FriendlyName -match 'ELECTRA' } | Select-Object -First 1
if (-not $disk) { throw "No disk with 'ELECTRA' in its name. Is the device still in USB DISK MODE?" }

"=== disk ==="
"  number     : $($disk.Number)"
"  name       : $($disk.FriendlyName)"
"  size       : $($disk.Size) bytes ($([math]::Round($disk.Size/1MB,1)) MB)"
"  style      : $($disk.PartitionStyle)"
"  readonly   : $($disk.IsReadOnly)"
""

$path = "\\.\PhysicalDrive$($disk.Number)"
$fs = New-Object IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)

try {
    $s0 = Get-Sector $fs 0

    "=== sector 0 ==="
    $sig = '{0:X2}{1:X2}' -f $s0[510], $s0[511]
    "  boot signature : $sig  $(if($sig -eq '55AA'){'(valid)'}else{'(NOT the expected 55AA)'})"
    "  OEM name       : '$([Text.Encoding]::ASCII.GetString($s0,3,8))'"
    $nonzero = ($s0 | Where-Object { $_ -ne 0 }).Count
    "  non-zero bytes : $nonzero / 512  $(if($nonzero -eq 0){'<-- ALL ZERO: reads are returning nothing'}else{''})"
    Show-Hex $s0 0 16 'first 16:'
    ""

    # Interpret sector 0 as a FAT volume boot record (superfloppy layout)
    $bps = [BitConverter]::ToUInt16($s0, 11)
    $spc = $s0[13]
    $rsv = [BitConverter]::ToUInt16($s0, 14)
    $nfat = $s0[16]
    "=== sector 0 read as a FAT boot record (superfloppy?) ==="
    "  bytes/sector   : $bps  $(if($bps -in 512,1024,2048,4096){'(plausible)'}else{'(implausible -> not a VBR)'})"
    "  sectors/cluster: $spc"
    "  reserved       : $rsv"
    "  num FATs       : $nfat"
    "  fs type field  : '$([Text.Encoding]::ASCII.GetString($s0,54,8))' / '$([Text.Encoding]::ASCII.GetString($s0,82,8))'"
    ""

    "=== MBR partition table ==="
    for ($i = 0; $i -lt 4; $i++) {
        $o = 446 + $i * 16
        $type = $s0[$o + 4]
        $lba = [BitConverter]::ToUInt32($s0, $o + 8)
        $cnt = [BitConverter]::ToUInt32($s0, $o + 12)
        $boot = $s0[$o]
        "  [$i] type=0x{0:X2} boot=0x{1:X2} startLBA={2} sectors={3} ({4} MB)" -f $type, $boot, $lba, $cnt, [math]::Round($cnt * 512 / 1MB, 1)
        Show-Hex $s0 $o 16 "     raw:"

        if ($type -ne 0 -and $lba -gt 0 -and $lba * 512 -lt $disk.Size) {
            try {
                $vbr = Get-Sector $fs $lba
                $vsig = '{0:X2}{1:X2}' -f $vbr[510], $vbr[511]
                "       -> VBR at LBA $lba : sig=$vsig oem='$([Text.Encoding]::ASCII.GetString($vbr,3,8))' fstype='$([Text.Encoding]::ASCII.GetString($vbr,54,8))'/'$([Text.Encoding]::ASCII.GetString($vbr,82,8))'"
            } catch {
                "       -> could not read VBR at LBA $lba : $($_.Exception.Message)"
            }
        }
    }
    ""

    # A FAT VBR often sits at a conventional offset even when the table looks empty.
    "=== probing common filesystem offsets ==="
    foreach ($lba in 1, 2, 8, 32, 63, 128, 2048) {
        try {
            $b = Get-Sector $fs $lba
            $s = '{0:X2}{1:X2}' -f $b[510], $b[511]
            $nz = ($b | Where-Object { $_ -ne 0 }).Count
            $oem = [Text.Encoding]::ASCII.GetString($b, 3, 8)
            $ft = ([Text.Encoding]::ASCII.GetString($b, 54, 8) + ' ' + [Text.Encoding]::ASCII.GetString($b, 82, 8)).Trim()
            $flag = if ($s -eq '55AA' -and $ft -match 'FAT') { '  <-- LOOKS LIKE A FAT VOLUME' } else { '' }
            "  LBA {0,-5} sig={1} nonzero={2,3} oem='{3}' fstype='{4}'{5}" -f $lba, $s, $nz, $oem, $ft, $flag
        } catch {
            "  LBA {0,-5} read failed: {1}" -f $lba, $_.Exception.Message
        }
    }
}
finally {
    $fs.Close()
}

""
"Done. Nothing was written to the device."
