# Empirical effect-toggle audit: pause the preview, flip each effect
# off/on/off/on, and measure how much the viewport actually changed between
# consecutive captures. inf = identical frame = the toggle did nothing.
. "$PSScriptRootI_drive.ps1"
$FF = "X:\SmackMyRezUp\build\Release\ffmpeg.exe"
$CLIP = "X:\Viking_woman_recording_selfie_video_202608170622.mp4"   # has _depth.mp4 and _flow.mp4 beside it

# settings that make every effect visible: LUT on a real path, Motion = Model
# (estimated) so the Flow toggle matters, strong NR
$ini = "$UI\SmackMyRezUp.ini"
$txt = Get-Content $ini -Raw
foreach ($kv in @(@("LutPath","X:\SmackMyRezUp\luts\cine_teal_orange.cube"),@("FxLut","1.000000"),@("LutStrength","0.900000"),
                  @("Motion","2.000000"),@("FxFlow","1.000000"),@("FxDepth","1.000000"),@("FxSharpen","1.000000"),@("FxTone","1.000000"),
                  @("NrIntensity","1.000000"),@("NrLocalStructure","2.000000"),@("Sharpen","0.600000"),@("ToneMix","0.500000"),@("PostSharpen","0.300000"))) {
  $txt = [regex]::Replace($txt, "(?m)^$($kv[0])=.*$", "$($kv[0])=$($kv[1])")
}
Set-Content $ini $txt -NoNewline

Get-Process SmackMyRezUp -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$UI*" } | Stop-Process -Force
Start-Process -FilePath "$UI\SmackMyRezUp.exe" -ArgumentList "`"$CLIP`"" -WorkingDirectory $UI | Out-Null
$t=(Get-Date).AddSeconds(60); while((Get-Date) -lt $t){ try { Main | Out-Null; break } catch { Start-Sleep -Milliseconds 500 } }
Start-Sleep -Seconds 5
Place 1500 980
Click 742 837          # mute
Start-Sleep -Seconds 2
Click 568 837          # pause
Start-Sleep -Seconds 1
"log: " + ((Get-Content "$UI\SmackMyRezUp.log") | Where-Object { $_ -match 'LUT|attached|Motion' } | ForEach-Object { $_.Substring(15) }) -join " | "

function Cap($n){ Shot $n | Out-Null; Start-Sleep -Milliseconds 250 }
function Psnr($a,$b){
  $o = & $FF -hide_banner -i "$UI\$a.png" -i "$UI\$b.png" -lavfi "[0:v]crop=1270:715:8:100[x];[1:v]crop=1270:715:8:100[y];[x][y]psnr" -f null - 2>&1 | Select-String "average:" | Select-Object -Last 1
  if ($o -match "average:([0-9.]+|inf)") { $matches[1] } else { "?" }
}
$results = @()
Cap "t00"; Start-Sleep 1; Cap "t01"
$results += "no-op re-capture            : $(Psnr t00 t01)"
$buttons = @(@("DepthMap",1423,596),@("Flow",1332,596),@("LUT",1423,536),@("Sharp",1332,566),@("Tone",1423,566),@("Bypass",1332,536))
$i = 2
foreach ($b in $buttons) {
  $name=$b[0]; $prev="t{0:d2}" -f ($i-1); $seq=@()
  Cap ("t{0:d2}" -f $i); $base="t{0:d2}" -f $i; $i++
  foreach ($step in 1..4) {
    Click $b[1] $b[2]; Start-Sleep -Milliseconds 900
    $cur="t{0:d2}" -f $i; Cap $cur; $i++
    $seq += (Psnr $base $cur); $base=$cur
  }
  $results += ("{0,-10} off/on/off/on   : {1}" -f $name, ($seq -join "  "))
}
""; "PSNR between consecutive captures of the paused viewport (inf = no change):"; $results
"INI now: $(IniVal FxDepth) $(IniVal FxFlow) $(IniVal FxLut) $(IniVal FxSharpen) $(IniVal FxTone)"
