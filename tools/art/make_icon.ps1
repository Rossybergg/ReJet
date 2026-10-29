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
    $field = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 255, 194, 26))
    $orange = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 243, 154, 30))
    $ink = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, 17, 17, 17))
    $o = [Math]::Max(1.0, 12 * $s) # the outline, at least a pixel at 16x16
    $g.FillPolygon($orange, [System.Drawing.PointF[]]@((P 0 196), (P 216 196), (P 207 244), (P 0 244)))
    $g.FillPolygon($ink, [System.Drawing.PointF[]]@((P 42 12), (P 256 12), (P 216 222), (P 2 222)))
    $g.FillPolygon($field, [System.Drawing.PointF[]]@((New-Object System.Drawing.PointF ((42 * $s) + $o * 1.2), ((12 * $s) + $o)),
        (New-Object System.Drawing.PointF ((256 * $s) - $o * 1.2), ((12 * $s) + $o)),
        (New-Object System.Drawing.PointF ((216 * $s) - $o * 1.2), ((222 * $s) - $o)),
        (New-Object System.Drawing.PointF ((2 * $s) + $o * 1.2), ((222 * $s) - $o))))
    # the "R": x 32..664, y 180..522 of the logo
    $sw = 632; $sh = 342; $dw = 180 * $s; $dh = $dw * $sh / $sw
    $g.DrawImage($src, (New-Object System.Drawing.RectangleF (34 * $s), ((117 * $s) - $dh / 2), $dw, $dh),
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
