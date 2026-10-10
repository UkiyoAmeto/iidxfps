# dump_module_sections.ps1
#
# Dumps the raw bytes of one or more PE sections of a module loaded in a *running*
# process, so the result can be compared between two runs (with and without a
# plugin).  Used to prove that a plugin does not modify the executable code of the
# game: the .text image of both runs must be byte identical.
#
# usage:
#   powershell -ExecutionPolicy Bypass -File .\dump_module_sections.ps1 `
#       -ProcessName spice64 -Module bm2dx.dll -Sections .text,.pdata,.rdata -OutPrefix runA
#
# The output is written to <OutPrefix>_<section>.bin plus <OutPrefix>.json with the
# module base, sizes and sha256 of every section.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ProcessName,
    [Parameter(Mandatory = $true)][string]$Module,
    [string[]]$Sections = @('.text', '.pdata', '.rdata'),
    [Parameter(Mandatory = $true)][string]$OutPrefix
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class Mem {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool CloseHandle(IntPtr h);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool ReadProcessMemory(IntPtr h, IntPtr addr, byte[] buf, IntPtr size, out IntPtr read);

    [DllImport("psapi.dll", SetLastError = true)]
    public static extern bool EnumProcessModulesEx(IntPtr h, IntPtr[] mods, uint size, out uint needed, uint filter);

    [DllImport("psapi.dll", CharSet = CharSet.Unicode)]
    public static extern uint GetModuleBaseNameW(IntPtr h, IntPtr mod, StringBuilder name, uint size);

    [DllImport("psapi.dll")]
    public static extern bool GetModuleInformation(IntPtr h, IntPtr mod, out MODULEINFO info, uint size);

    [StructLayout(LayoutKind.Sequential)]
    public struct MODULEINFO { public IntPtr lpBaseOfDll; public uint SizeOfImage; public IntPtr EntryPoint; }

    public const uint PROCESS_QUERY_INFORMATION = 0x0400;
    public const uint PROCESS_VM_READ = 0x0010;
    public const uint LIST_MODULES_ALL = 0x03;
}
'@

function Get-ModuleBase([IntPtr]$h, [string]$name) {
    $needed = 0
    $buf = New-Object IntPtr[] 1024
    if (-not [Mem]::EnumProcessModulesEx($h, $buf, [uint32]($buf.Length * [IntPtr]::Size), [ref]$needed, [Mem]::LIST_MODULES_ALL)) {
        throw "EnumProcessModulesEx failed: $([ComponentModel.Win32Exception]::new([Runtime.InteropServices.Marshal]::GetLastWin32Error()).Message)"
    }
    $count = [int]($needed / [IntPtr]::Size)
    for ($i = 0; $i -lt $count; $i++) {
        $sb = New-Object System.Text.StringBuilder 260
        [void][Mem]::GetModuleBaseNameW($h, $buf[$i], $sb, 260)
        if ($sb.ToString() -ieq $name) { return $buf[$i] }
    }
    throw "module $name not found in process"
}

function Get-SectionLayout([IntPtr]$h, [IntPtr]$base, [string]$section) {
    # read the PE headers of the *loaded* image so the layout matches the load
    $hdr = New-Object byte[] 0x1000
    $got = [IntPtr]::Zero
    if (-not [Mem]::ReadProcessMemory($h, $base, $hdr, [IntPtr]$hdr.Length, [ref]$got)) { throw "cannot read PE header" }
    $e_lfanew = [BitConverter]::ToInt32($hdr, 0x3C)
    $numSections = [BitConverter]::ToUInt16($hdr, $e_lfanew + 6)
    $optSize = [BitConverter]::ToUInt16($hdr, $e_lfanew + 20)
    $secTable = $e_lfanew + 24 + $optSize
    for ($i = 0; $i -lt $numSections; $i++) {
        $o = $secTable + $i * 40
        $n = ([System.Text.Encoding]::ASCII.GetString($hdr, $o, 8)).Trim([char]0)
        if ($n -ieq $section) {
            $vsize = [BitConverter]::ToUInt32($hdr, $o + 8)
            $vaddr = [BitConverter]::ToUInt32($hdr, $o + 12)
            $rsize = [BitConverter]::ToUInt32($hdr, $o + 16)
            $size = if ($vsize -gt 0) { $vsize } else { $rsize }
            return [pscustomobject]@{ Name = $n; Rva = $vaddr; Size = $size }
        }
    }
    throw "section $section not found"
}

$proc = Get-Process -Name $ProcessName -ErrorAction Stop | Select-Object -First 1
$h = [Mem]::OpenProcess(([Mem]::PROCESS_QUERY_INFORMATION -bor [Mem]::PROCESS_VM_READ), $false, $proc.Id)
if ($h -eq [IntPtr]::Zero) { throw "OpenProcess failed (try running as the same user, or elevated)" }

try {
    $base = Get-ModuleBase $h $Module
    Write-Host "[*] $Module base = 0x$('{0:X}' -f $base.ToInt64())"
    $result = [ordered]@{ process = $ProcessName; pid = $proc.Id; module = $Module; base = ('0x{0:X}' -f $base.ToInt64()); sections = @() }

    foreach ($s in $Sections) {
        $lay = Get-SectionLayout $h $base $s
        $addr = [IntPtr]($base.ToInt64() + $lay.Rva)
        $data = New-Object byte[] $lay.Size
        $chunk = 1MB
        $off = 0
        while ($off -lt $lay.Size) {
            $len = [Math]::Min($chunk, $lay.Size - $off)
            $got = [IntPtr]::Zero
            $tmp = New-Object byte[] $len
            if (-not [Mem]::ReadProcessMemory($h, [IntPtr]($addr.ToInt64() + $off), $tmp, [IntPtr]$len, [ref]$got)) {
                throw "ReadProcessMemory failed at $s + 0x$('{0:X}' -f $off)"
            }
            [Array]::Copy($tmp, 0, $data, $off, $len)
            $off += $len
        }
        $file = "$OutPrefix$($s.Replace('.','_')).bin"
        [System.IO.File]::WriteAllBytes($file, $data)
        $sha = [System.Security.Cryptography.SHA256]::Create().ComputeHash($data)
        $hex = ($sha | ForEach-Object { $_.ToString('x2') }) -join ''
        Write-Host ("[ok] {0,-8} rva=0x{1:X8} size={2,9} sha256={3}" -f $s, $lay.Rva, $lay.Size, $hex)
        $result.sections += [pscustomobject]@{ name = $s; rva = $lay.Rva; size = $lay.Size; sha256 = $hex; file = $file }
    }
    ($result | ConvertTo-Json -Depth 4) | Set-Content "$OutPrefix.json" -Encoding UTF8
} finally {
    [void][Mem]::CloseHandle($h)
}
