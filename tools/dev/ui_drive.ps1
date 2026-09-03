# Drives a running SmackMyRezUp window for automated UI tests: every action is a
# PostMessage/SendMessage to the app (no real mouse or keyboard), captures are
# PrintWindow, dialogs are answered by WM_COMMAND (message boxes: close with
# WM_CLOSE - their single OK button has id 2). Point SMRU_UI_DIR at the install
# under test (exe + ini; screenshots land there). ui_fx_audit.ps1 is a worked
# example that measures every effect toggle with PSNR.

# Drive the SmackMyRezUp player window without touching the real mouse or
# keyboard: everything is PostMessage/SendMessage to the app's own windows.
$ErrorActionPreference = "Stop"
$script:UI = $env:SMRU_UI_DIR
Add-Type -AssemblyName System.Drawing
if (-not ("U" -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public static class U {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc cb, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x,int y,int cx,int cy, uint flags);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X,Y; }
  public static List<IntPtr> Top() { var r = new List<IntPtr>(); EnumWindows((h,l)=>{ r.Add(h); return true; }, IntPtr.Zero); return r; }
  public static List<IntPtr> Children(IntPtr p) { var r = new List<IntPtr>(); EnumChildWindows(p,(h,l)=>{ r.Add(h); return true; }, IntPtr.Zero); return r; }
  public static string Cls(IntPtr h){ var s=new StringBuilder(256); GetClassNameW(h,s,256); return s.ToString(); }
  public static string Txt(IntPtr h){ var s=new StringBuilder(4096); GetWindowTextW(h,s,4096); return s.ToString(); }
}
"@
}
$WM_MOUSEMOVE=0x200; $WM_LBUTTONDOWN=0x201; $WM_LBUTTONUP=0x202; $WM_RBUTTONDOWN=0x204; $WM_RBUTTONUP=0x205
$WM_COMMAND=0x111; $WM_KEYDOWN=0x100; $WM_KEYUP=0x101; $WM_CLOSE=0x10; $WM_MOUSEWHEEL=0x20A
$GW_OWNER=4

function Main { foreach($h in [U]::Top()){ if([U]::Cls($h) -eq "SmackMyRezUp.MainWindow" -and [U]::IsWindowVisible($h)){ return $h } }; throw "player window not found" }
function LP($x,$y){ [IntPtr](($y -band 0xFFFF) -shl 16 -bor ($x -band 0xFFFF)) }
function Click($x,$y,[int]$holdMs=40){ $h=Main
  [U]::PostMessageW($h,$WM_MOUSEMOVE,[IntPtr]0,(LP $x $y))|Out-Null; Start-Sleep -Milliseconds 30
  [U]::PostMessageW($h,$WM_LBUTTONDOWN,[IntPtr]1,(LP $x $y))|Out-Null; Start-Sleep -Milliseconds $holdMs
  [U]::PostMessageW($h,$WM_LBUTTONUP,[IntPtr]0,(LP $x $y))|Out-Null; Start-Sleep -Milliseconds 120 }
function RClick($x,$y){ $h=Main
  [U]::PostMessageW($h,$WM_RBUTTONDOWN,[IntPtr]2,(LP $x $y))|Out-Null; Start-Sleep -Milliseconds 40
  [U]::PostMessageW($h,$WM_RBUTTONUP,[IntPtr]0,(LP $x $y))|Out-Null; Start-Sleep -Milliseconds 120 }
function Drag($x0,$y,$x1,[int]$steps=8){ $h=Main
  [U]::PostMessageW($h,$WM_LBUTTONDOWN,[IntPtr]1,(LP $x0 $y))|Out-Null; Start-Sleep -Milliseconds 40
  for($i=1;$i -le $steps;$i++){ $x=[int]($x0+($x1-$x0)*$i/$steps); [U]::PostMessageW($h,$WM_MOUSEMOVE,[IntPtr]1,(LP $x $y))|Out-Null; Start-Sleep -Milliseconds 25 }
  [U]::PostMessageW($h,$WM_LBUTTONUP,[IntPtr]0,(LP $x1 $y))|Out-Null; Start-Sleep -Milliseconds 150 }
function Key([int]$vk){ $h=Main; [U]::PostMessageW($h,$WM_KEYDOWN,[IntPtr]$vk,[IntPtr]0)|Out-Null; Start-Sleep -Milliseconds 30; [U]::PostMessageW($h,$WM_KEYUP,[IntPtr]$vk,[IntPtr]0)|Out-Null; Start-Sleep -Milliseconds 120 }
function Wheel($x,$y,[int]$delta){ $h=Main; $p=New-Object U+POINT; $p.X=$x;$p.Y=$y; [U]::ClientToScreen($h,[ref]$p)|Out-Null
  [U]::PostMessageW($h,$WM_MOUSEWHEEL,[IntPtr](($delta -band 0xFFFF) -shl 16),(LP $p.X $p.Y))|Out-Null; Start-Sleep -Milliseconds 120 }
function Place([int]$w=1500,[int]$ht=980){ $h=Main; [U]::SetWindowPos($h,[IntPtr]0,40,40,$w,$ht,0x0010 -bor 0x0004)|Out-Null; Start-Sleep -Milliseconds 400 }  # SWP_NOACTIVATE|NOZORDER
function Shot($name){ $h=Main; $r=New-Object U+RECT; [U]::GetWindowRect($h,[ref]$r)|Out-Null
  $bmp=New-Object System.Drawing.Bitmap(($r.R-$r.L),($r.B-$r.T)); $g=[System.Drawing.Graphics]::FromImage($bmp)
  $dc=$g.GetHdc(); [U]::PrintWindow($h,$dc,2)|Out-Null; $g.ReleaseHdc($dc); $g.Dispose()
  $p="$script:UI\$name.png"; $bmp.Save($p,[System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose(); $p }
function ClientSize { $h=Main; $r=New-Object U+RECT; [U]::GetClientRect($h,[ref]$r)|Out-Null; "$($r.R)x$($r.B)" }
function ClientOrigin { $h=Main; $p=New-Object U+POINT; [U]::ClientToScreen($h,[ref]$p)|Out-Null; $w=New-Object U+RECT; [U]::GetWindowRect($h,[ref]$w)|Out-Null; "client origin in window shot: $($p.X-$w.L),$($p.Y-$w.T)" }
# Dialogs and message boxes owned by the player (class #32770): describe and answer them.
function Dialogs { $main=Main; $out=@()
  foreach($h in [U]::Top()){ if(-not [U]::IsWindowVisible($h)){continue}; if([U]::Cls($h) -ne "#32770"){continue}
    $o=[U]::GetWindow($h,$GW_OWNER); $own=$o -eq $main
    if(-not $own){ # or owned by a dialog that the player owns (overwrite prompt)
      if($o -ne [IntPtr]::Zero -and [U]::GetWindow($o,$GW_OWNER) -eq $main){ $own=$true } }
    if(-not $own){continue}
    $kids=@(); foreach($c in [U]::Children($h)){ $t=[U]::Txt($c); if($t){ $kids += ("[" + [U]::Cls($c) + " id=" + [U]::GetDlgCtrlID($c) + "] " + $t.Replace("`r`n"," / ")) } }
    $out += [pscustomobject]@{ hwnd=$h; title=[U]::Txt($h); controls=($kids -join " | ") } }
  $out }
function Answer($hwnd,[int]$id){ [U]::PostMessageW([IntPtr]$hwnd,$WM_COMMAND,[IntPtr]$id,[IntPtr]0)|Out-Null; Start-Sleep -Milliseconds 300 }
function Ini { Get-Content "$script:UI\SmackMyRezUp.ini" }
function IniVal($k){ (Get-Content "$script:UI\SmackMyRezUp.ini" | Select-String "^$k=" | Select-Object -Last 1).Line }
function LogTail([int]$n=12){ Get-Content "$script:UI\SmackMyRezUp.log" -Tail $n }
