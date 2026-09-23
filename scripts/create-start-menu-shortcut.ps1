<#
.SYNOPSIS
  Creates (or removes) a Start Menu shortcut for the RayNeo Spatial controller.

.DESCRIPTION
  The shortcut is written to the per-user Start Menu programs folder and is
  tagged with the same AppUserModelID the application sets at runtime
  ("RayNeo.Spatial.Desktop"), so pinning either the running app or this shortcut
  produces a single taskbar identity.

  This script does NOT pin anything and does NOT install anything: it only
  creates the .lnk file. Pinning stays a deliberate user action (right-click the
  Start Menu entry -> Pin to Start / Pin to taskbar).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\create-start-menu-shortcut.ps1 `
      -AppPath "C:\Tools\RayNeoSpatial\RayNeo Spatial.exe"
  powershell -ExecutionPolicy Bypass -File scripts\create-start-menu-shortcut.ps1 -Remove
#>
[CmdletBinding()]
param(
    [string]$AppPath,
    [string]$Name = "RayNeo Spatial",
    [switch]$AllUsers,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

$appUserModelId = "RayNeo.Spatial.Desktop"

if ($AllUsers) {
    $principal = New-Object Security.Principal.WindowsPrincipal(
        [Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw "-AllUsers writes to the machine-wide Start Menu; re-run this script from an elevated prompt."
    }
    $programs = Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs"
} else {
    $programs = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
}
$shortcutPath = Join-Path $programs "$Name.lnk"

if ($Remove) {
    if (Test-Path -LiteralPath $shortcutPath) {
        Remove-Item -LiteralPath $shortcutPath -Force
        Write-Host "Removed '$shortcutPath'"
    } else {
        Write-Host "Nothing to remove at '$shortcutPath'"
    }
    exit 0
}

if (-not $AppPath) {
    $AppPath = Join-Path $PSScriptRoot "..\RayNeo Spatial.exe"
}
$AppPath = [System.IO.Path]::GetFullPath($AppPath)
if (-not (Test-Path -LiteralPath $AppPath)) {
    throw "'$AppPath' does not exist. Pass -AppPath with the full path of 'RayNeo Spatial.exe'."
}
$workingDirectory = Split-Path -Parent $AppPath

$null = New-Item -ItemType Directory -Force -Path $programs
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $AppPath
$shortcut.WorkingDirectory = $workingDirectory
$shortcut.Description = "RayNeo Spatial controller"
$shortcut.IconLocation = "$AppPath,0"
$shortcut.Save()

# Tag the shortcut with the application's AppUserModelID. This needs the shell
# property store, which PowerShell 5.1 does not expose; the interop below is
# the documented minimal route (IPropertyStore + IPersistFile on a ShellLink).
$tagged = $false
try {
    if (-not ("RayNeoSpatialShortcutTag" -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class RayNeoSpatialShortcutTag
{
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct PropertyKey
    {
        public Guid FormatId;
        public uint PropertyId;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct PropVariant
    {
        [FieldOffset(0)] public ushort VariantType;
        [FieldOffset(8)] public IntPtr PointerValue;
        [FieldOffset(16)] public long Padding;
    }

    [ComImport, Guid("00021401-0000-0000-C000-000000000046")]
    public class ShellLink
    {
    }

    [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("0000010b-0000-0000-C000-000000000046")]
    public interface IPersistFile
    {
        void GetClassID(out Guid classId);
        [PreserveSig] int IsDirty();
        void Load([MarshalAs(UnmanagedType.LPWStr)] string fileName, uint mode);
        void Save([MarshalAs(UnmanagedType.LPWStr)] string fileName, [MarshalAs(UnmanagedType.Bool)] bool remember);
        void SaveCompleted([MarshalAs(UnmanagedType.LPWStr)] string fileName);
        void GetCurFile([MarshalAs(UnmanagedType.LPWStr)] out string fileName);
    }

    [ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99")]
    public interface IPropertyStore
    {
        void GetCount(out uint count);
        void GetAt(uint index, out PropertyKey key);
        void GetValue(ref PropertyKey key, out PropVariant value);
        void SetValue(ref PropertyKey key, ref PropVariant value);
        void Commit();
    }

    public static void SetAppUserModelId(string shortcutPath, string appUserModelId)
    {
        var link = new ShellLink();
        var persist = (IPersistFile)link;
        persist.Load(shortcutPath, 2); // STGM_READWRITE: SetValue needs write access
        var store = (IPropertyStore)link;

        PropertyKey key;
        key.FormatId = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"); // PKEY_AppUserModel_ID
        key.PropertyId = 5;

        PropVariant value;
        value.VariantType = 31; // VT_LPWSTR
        value.Padding = 0;
        value.PointerValue = Marshal.StringToCoTaskMemUni(appUserModelId);
        try
        {
            store.SetValue(ref key, ref value);
            store.Commit();
        }
        finally
        {
            Marshal.FreeCoTaskMem(value.PointerValue);
        }
        persist.Save(shortcutPath, true);
        Marshal.ReleaseComObject(link);
    }
}
'@
    }
    [RayNeoSpatialShortcutTag]::SetAppUserModelId($shortcutPath, $appUserModelId)
    $tagged = $true
} catch {
    Write-Warning "The shortcut was created but could not be tagged with the AppUserModelID: $($_.Exception.Message)"
}

Write-Host "Shortcut: $shortcutPath"
Write-Host "Target:   $AppPath"
if ($tagged) {
    Write-Host "Identity: $appUserModelId (same as the running application)"
} else {
    Write-Host "Identity: not tagged (the running application still uses $appUserModelId, so pinning the running app is unaffected)"
}
Write-Host ""
Write-Host "Nothing was pinned. To pin it yourself:"
Write-Host "  * Start Menu -> search '$Name' -> right-click -> Pin to Start / Pin to taskbar"
Write-Host "  * or start the app and right-click its taskbar button -> Pin to taskbar"
