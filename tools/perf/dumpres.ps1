# Dump the bytes right after the UTF-16LE "FileDescription" key in a .res file.
param([Parameter(Mandatory=$true)][string]$Path)
$b = [IO.File]::ReadAllBytes($Path)
$pat = [Text.Encoding]::Unicode.GetBytes('FileDescription')
for ($i = 0; $i -le $b.Length - $pat.Length; $i += 2) {
    $ok = $true
    for ($j = 0; $j -lt $pat.Length; $j++) {
        if ($b[$i + $j] -ne $pat[$j]) { $ok = $false; break }
    }
    if ($ok) {
        $start = $i + $pat.Length + 2
        $hex = ($b[$start..($start + 47)] | ForEach-Object { $_.ToString('X2') }) -join ' '
        Write-Host ("hex: " + $hex)
        $u16 = [Text.Encoding]::Unicode.GetString($b, $start, 48)
        Write-Host ("utf16: [" + ($u16 -replace "[\x00-\x1f]", ".") + "]")
        $u8 = [Text.Encoding]::UTF8.GetString($b, $start, 24)
        Write-Host ("utf8 : [" + ($u8 -replace "[\x00-\x1f]", ".") + "]")
        return
    }
}
Write-Host "FileDescription key not found"
