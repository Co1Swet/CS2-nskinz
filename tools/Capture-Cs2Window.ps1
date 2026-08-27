param(
    [Parameter(Mandatory = $true)]
    [int]$TargetProcessId,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
if (-not ('NSkinzWindowCaptureNative' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class NSkinzWindowCaptureNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    public delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);

    [DllImport("user32.dll")]
    public static extern bool EnumDesktopWindows(
        IntPtr desktop,
        EnumWindowsProc callback,
        IntPtr parameter);

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr OpenDesktop(
        string desktop,
        uint flags,
        bool inherit,
        uint desiredAccess);

    [DllImport("user32.dll")]
    public static extern bool CloseDesktop(IntPtr desktop);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr window);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr window, IntPtr deviceContext, uint flags);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr window, int command);

    public static IntPtr FindLargestWindow(uint processId)
    {
        IntPtr result = IntPtr.Zero;
        int largestArea = 0;
        EnumWindowsProc scan = delegate(IntPtr window, IntPtr parameter)
        {
            uint ownerProcessId;
            Rect rect;
            GetWindowThreadProcessId(window, out ownerProcessId);
            if (ownerProcessId == processId && IsWindowVisible(window) && GetWindowRect(window, out rect))
            {
                int area = Math.Max(0, rect.Right - rect.Left) * Math.Max(0, rect.Bottom - rect.Top);
                if (area > largestArea)
                {
                    largestArea = area;
                    result = window;
                }
            }
            return true;
        };

        EnumWindows(scan, IntPtr.Zero);

        // A DLL-owned helper/tool window can be visible on the caller's
        // window station while the actual CS2 render window lives on the
        // interactive user's default desktop. Always scan that desktop too;
        // otherwise the first tiny helper window prevents discovery of the
        // much larger game window.
        const uint DESKTOP_READOBJECTS = 0x0001;
        const uint DESKTOP_ENUMERATE = 0x0040;
        IntPtr desktop = OpenDesktop(
            "default",
            0,
            false,
            DESKTOP_READOBJECTS | DESKTOP_ENUMERATE);
        if (desktop != IntPtr.Zero)
        {
            EnumDesktopWindows(desktop, scan, IntPtr.Zero);
            CloseDesktop(desktop);
        }
        return result;
    }

    public static bool Capture(uint processId, string outputPath)
    {
        IntPtr window = FindLargestWindow(processId);
        Rect rect;
        if (window == IntPtr.Zero || !GetWindowRect(window, out rect))
        {
            return false;
        }

        bool wasMinimized = rect.Left <= -30000 && rect.Top <= -30000;
        if (wasMinimized)
        {
            const int SW_SHOWNOACTIVATE = 4;
            ShowWindow(window, SW_SHOWNOACTIVATE);
            System.Threading.Thread.Sleep(300);
            if (!GetWindowRect(window, out rect))
            {
                return false;
            }
        }

        int width = rect.Right - rect.Left;
        int height = rect.Bottom - rect.Top;
        using (Bitmap bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb))
        using (Graphics graphics = Graphics.FromImage(bitmap))
        {
            IntPtr deviceContext = graphics.GetHdc();
            bool captured = PrintWindow(window, deviceContext, 2);
            graphics.ReleaseHdc(deviceContext);
            if (!captured)
            {
                if (wasMinimized)
                {
                    const int SW_MINIMIZE = 6;
                    ShowWindow(window, SW_MINIMIZE);
                }
                return false;
            }
            bitmap.Save(outputPath, ImageFormat.Png);
            if (wasMinimized)
            {
                const int SW_MINIMIZE = 6;
                ShowWindow(window, SW_MINIMIZE);
            }
            return true;
        }
    }
}
'@ -ReferencedAssemblies System.Drawing
}

$resolvedOutput = [IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path $resolvedOutput -Parent
if (-not (Test-Path -LiteralPath $outputDirectory)) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}

if (-not [NSkinzWindowCaptureNative]::Capture([uint32]$TargetProcessId, $resolvedOutput)) {
    throw "PrintWindow failed for process $TargetProcessId."
}

Get-Item -LiteralPath $resolvedOutput | Select-Object FullName, Length, LastWriteTime
