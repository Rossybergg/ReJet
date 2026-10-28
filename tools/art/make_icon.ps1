# Make loader\res\rejet.ico from the ReJet logo: the logo's "R" on a slanted lime badge with the
# pink stripe of the ReJet menu, at the standard icon sizes (PNG-compressed icon entries).
#   powershell -ExecutionPolicy Bypass -File tools\art\make_icon.ps1
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = [System.Drawing.Image]::FromFile("$root\loader\res\rejet_logo.png")
$sizes = 256, 64, 48, 40, 32, 24, 20, 16
$images = @()
foreach ($n in $sizes) {
    $b = New-Object System.Drawing.Bitmap $n, $n, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $s = $n / 256.0
    function P($x, $y) { New-Object System.Drawing.PointF ($x * $s), ($y * $s) }
    $lime = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 200, 240, 30))
    $pink = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 45, 138))
    $g.FillPolygon($lime, [System.Drawing.PointF[]]@((P 44 20), (P 252 20), (P 212 214), (P 4 214)))
    $g.FillPolygon($pink, [System.Drawing.PointF[]]@((P 4 214), (P 212 214), (P 205 240), (P 0 240)))
    # the "R": x 32..664, y 180..522 of the logo
    $sw = 632; $sh = 342; $dw = 202 * $s; $dh = $dw * $sh / $sw
    $g.DrawImage($src, (New-Object System.Drawing.RectangleF (25 * $s), ((117 * $s) - $dh / 2), $dw, $dh),
        (New-Object System.Drawing.RectangleF 32, 180, $sw, $sh), [System.Drawing.GraphicsUnit]::Pixel)
    $g.Dispose()
    $ms = New-Object System.IO.MemoryStream
    $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $images += , @($n, $ms.ToArray())
    $b.Dispose()
}
# ICO: header, one directory entry per size, then the PNG data
$out = New-Object System.IO.MemoryStream
$w = New-Object System.IO.BinaryWriter $out
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$images.Count)
$offset = 6 + 16 * $images.Count
foreach ($im in $images) {
    $n = $im[0]; $data = $im[1]
    $w.Write([byte]($n % 256)); $w.Write([byte]($n % 256)); $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]$data.Length); $w.Write([UInt32]$offset)
    $offset += $data.Length
}
foreach ($im in $images) { $w.Write($im[1]) }
[System.IO.File]::WriteAllBytes("$root\loader\res\rejet.ico", $out.ToArray())
"wrote loader\res\rejet.ico ($($out.Length) bytes)"
