# End-to-end test of GpsAssistUpdater against a subst drive (never a real card).
# Never takes the foreground: drives controls by message, captures only the app's own
# windows with PrintWindow. Screenshots and results land in -Out.
#
#   run-test.ps1 -Exe dist\GpsAssistUpdater-x64.exe -Drive T -Out <dir> [-Server host] [-Cache dir]
#                [-Steps update,update-again,thai,about] [-Tag name]
param(
  [Parameter(Mandatory)][string]$Exe,
  [Parameter(Mandatory)][string]$Drive,
  [Parameter(Mandatory)][string]$Out,
  [string]$Server = '',
  [string]$Cache = '',
  [string[]]$Steps = @('update'),
  [string]$Tag = 'run'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
if (-not ("GA" -as [type])) {
Add-Type @"
using System; using System.Runtime.InteropServices; using System.Text;
public class GA {
  public delegate bool P(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(P cb, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, string l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, StringBuilder l);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, int m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static IntPtr Find(uint pid, string cls) { IntPtr found = IntPtr.Zero; EnumWindows((h,l)=>{ uint p; GetWindowThreadProcessId(h, out p); var c=new StringBuilder(128); GetClassName(h,c,128); if (p==pid && IsWindowVisible(h) && c.ToString()==cls){ found=h; return false; } return true; }, IntPtr.Zero); return found; }
  public static string Text(IntPtr h) { var sb = new StringBuilder(65536); SendMessage(h, 0x000D, (IntPtr)65536, sb); return sb.ToString(); }
}
"@
}
$ID = @{ CARD=1004; SHOWALL=1006; CARD_STATUS=1007; SERVER_STATUS=1008; UPDATE=1009; LANG=1010; ABOUT=1011; LOG=1013 }
New-Item -ItemType Directory -Force $Out | Out-Null

function Shot($hwnd, $name) {
  $r = New-Object GA+RECT; [GA]::GetWindowRect($hwnd, [ref]$r) | Out-Null
  $bmp = New-Object System.Drawing.Bitmap ($r.R-$r.L), ($r.B-$r.T)
  $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc(); [GA]::PrintWindow($hwnd, $dc, 2) | Out-Null; $g.ReleaseHdc($dc); $g.Dispose()
  $bmp.Save((Join-Path $Out "$Tag-$name.png")); $bmp.Dispose()
}
function Ctl($id) { [GA]::GetDlgItem($script:h, $id) }
function Cmd($id) { [GA]::PostMessage($script:h, 0x0111, [IntPtr]$id, [IntPtr]::Zero) | Out-Null }
function LogText { [GA]::Text((Ctl $ID.LOG)) }
function WaitFor([scriptblock]$cond, [int]$ms) { $sw=[Diagnostics.Stopwatch]::StartNew(); while ($sw.ElapsedMilliseconds -lt $ms) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 200 }; return $false }
function PressDialog([int]$button) {
  $d = [IntPtr]::Zero
  if (-not (WaitFor { $script:d = [GA]::Find([uint32]$script:p.Id, '#32770'); $script:d -ne [IntPtr]::Zero } 5000)) { throw "no dialog appeared" }
  Shot $script:d "dialog-$button"
  [GA]::PostMessage($script:d, 0x0111, [IntPtr]$button, [IntPtr]::Zero) | Out-Null
  WaitFor { [GA]::Find([uint32]$script:p.Id, '#32770') -eq [IntPtr]::Zero } 3000 | Out-Null
}
function SelectTestDrive {
  # Show all drives (a subst drive is a fixed disk), then pick only our test letter.
  [GA]::SendMessage((Ctl $ID.SHOWALL), 0x00F1, [IntPtr]1, [IntPtr]::Zero) | Out-Null
  Cmd $ID.SHOWALL; Start-Sleep -Milliseconds 800
  $combo = Ctl $ID.CARD
  $idx = [GA]::SendMessage($combo, 0x014D, [IntPtr](-1), "${Drive}:\")
  [GA]::PostMessage($script:h, 0x0111, [IntPtr](($ID.CARD) -bor (1 -shl 16)), $combo) | Out-Null
  Start-Sleep -Milliseconds 500
  $sel = [GA]::Text($combo)
  if (-not $sel.StartsWith("${Drive}:\")) { throw "SAFETY: selected drive is '$sel', not ${Drive}:" }
  "selected: $sel"
}
function RunUpdate {
  $before = (LogText).Length
  Cmd $ID.UPDATE
  PressDialog 6   # IDYES on "not a removable drive"
  $ok = WaitFor { $t = (LogText).Substring([Math]::Min($before, (LogText).Length)); $t -match 'Done\.|Update failed|Nothing to do|เสร็จแล้ว|ไม่สำเร็จ|ไม่ต้องอัปเดต' } 90000
  if (-not $ok) { throw "update did not finish" }
  Start-Sleep -Milliseconds 600
}

$psi = New-Object Diagnostics.ProcessStartInfo $Exe
$psi.UseShellExecute = $false
if ($Server) { $psi.EnvironmentVariables['GPSASSIST_SERVER'] = $Server }
if ($Cache) { $psi.EnvironmentVariables['GPSASSIST_CACHE'] = $Cache }
$script:p = [Diagnostics.Process]::Start($psi)
try {
  if (-not (WaitFor { $script:p.Refresh(); $script:p.MainWindowHandle -ne [IntPtr]::Zero } 10000)) { throw "no main window" }
  $script:h = $script:p.MainWindowHandle
  WaitFor { [GA]::Text((Ctl $ID.SERVER_STATUS)) -notmatch 'checking|กำลังตรวจสอบ' } 40000 | Out-Null
  SelectTestDrive
  Shot $h 'start'
  foreach ($s in $Steps) {
    switch ($s) {
      'update'       { RunUpdate; Shot $h 'after-update' }
      'update-again' { RunUpdate; Shot $h 'after-update-again' }
      'thai'         { Cmd $ID.LANG; Start-Sleep -Milliseconds 800; SelectTestDrive | Out-Null; Shot $h 'thai' }
      'about'        { Cmd $ID.ABOUT; PressDialog 1 }
    }
  }
  "--- server status: " + [GA]::Text((Ctl $ID.SERVER_STATUS))
  "--- card status:   " + [GA]::Text((Ctl $ID.CARD_STATUS))
  "--- log:"; LogText
}
finally {
  if (-not $script:p.HasExited) { $script:p.Kill(); $script:p.WaitForExit(3000) | Out-Null }
}
