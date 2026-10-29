# Make docs\images\banner.png (the README's header) from the ReJet logo: the logo (black, as
# drawn) on a slanted yellow Garage badge outlined in black, with the menu's orange bars, so it
# reads on GitHub's light and dark themes.
#   powershell -ExecutionPolicy Bypass -File tools\art\make_banner.ps1
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$src = [System.Drawing.Image]::FromFile("$root\loader\res\rejet_logo.png")
$W = 1600; $H = 420
$b = New-Object System.Drawing.Bitmap $W, $H, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($b)
$g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
function P($x, $y) { New-Object System.Drawing.PointF $x, $y }
function Brush($r, $gr, $bl) { New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(255, $r, $gr, $bl)) }
$field = Brush 255 194 26; $orange = Brush 243 154 30; $ink = Brush 17 17 17
# orange bars behind the badge's top edge, as the menu's
foreach ($bar in @(@(160, 36), @(214, 60), @(292, 24), @(334, 84), @(436, 30), @(1330, 30), @(1378, 50))) {
    $g.FillRectangle($orange, $bar[0], 0, $bar[1], 70)
}
# the badge: black, then yellow inset, with a drop shadow
$g.FillPolygon((New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(110, 0, 0, 0))),
    [System.Drawing.PointF[]]@((P 134 50), (P 1606 50), (P 1494 412), (P 22 412)))
$g.FillPolygon($ink, [System.Drawing.PointF[]]@((P 120 36), (P 1592 36), (P 1480 398), (P 8 398)))
$g.FillPolygon($field, [System.Drawing.PointF[]]@((P 134 48), (P 1574 48), (P 1470 386), (P 26 386)))
# the logo, trimmed to its ink (the PNG has a margin), centred on the badge
$sx = 32; $sy = 180; $sw = 2111; $sh = 344
$dh = 210; $dw = $dh * $sw / $sh
$g.DrawImage($src, (New-Object System.Drawing.RectangleF ((($W - $dw) / 2) + 6), (217 - $dh / 2), $dw, $dh),
    (New-Object System.Drawing.RectangleF $sx, $sy, $sw, $sh), [System.Drawing.GraphicsUnit]::Pixel)
$g.Dispose()
$b.Save("$root\docs\images\banner.png", [System.Drawing.Imaging.ImageFormat]::Png)
"wrote docs\images\banner.png"
