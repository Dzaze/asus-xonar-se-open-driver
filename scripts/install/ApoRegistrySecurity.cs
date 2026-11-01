// SPDX-License-Identifier: Apache-2.0
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Security.AccessControl;

public static class XonarRegistrySecurity
{
    const uint ReadControl = 0x20000, WriteDac = 0x40000, WriteOwner = 0x80000;
    static readonly IntPtr LocalMachine = new IntPtr(unchecked((int)0x80000002));
    [StructLayout(LayoutKind.Sequential)] struct Luid { public uint Low; public int High; }
    [StructLayout(LayoutKind.Sequential)] struct TokenPrivileges { public uint Count; public Luid Id; public uint Attributes; }
    [StructLayout(LayoutKind.Sequential)] struct AuthzRequest { public uint Desired; public IntPtr Self, Types; public uint TypeCount; public IntPtr Arguments; }
    [StructLayout(LayoutKind.Sequential)] struct AuthzReply { public uint Count; public IntPtr Granted, Sacl, Error; }
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError=true)] static extern IntPtr OpenProcess(uint rights, bool inherit, int pid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr process, uint rights, out IntPtr token);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool LookupPrivilegeValue(string system, string name, out Luid id);
    [DllImport("advapi32.dll", SetLastError=true)] static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TokenPrivileges state, uint length, out TokenPrivileges previous, out uint returned);
    [DllImport("advapi32.dll", CharSet=CharSet.Unicode)] static extern int RegOpenKeyEx(IntPtr root, string path, uint options, uint rights, out IntPtr key);
    [DllImport("advapi32.dll")] static extern int RegGetKeySecurity(IntPtr key, uint info, byte[] descriptor, ref uint length);
    [DllImport("advapi32.dll")] static extern int RegSetKeySecurity(IntPtr key, uint info, byte[] descriptor);
    [DllImport("advapi32.dll")] static extern int RegCloseKey(IntPtr key);
    [DllImport("authz.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool AuthzInitializeResourceManager(uint flags, IntPtr access, IntPtr groups, IntPtr free, string name, out IntPtr manager);
    [DllImport("authz.dll", SetLastError=true)] static extern bool AuthzInitializeContextFromToken(uint flags, IntPtr token, IntPtr manager, IntPtr expires, Luid identifier, IntPtr arguments, out IntPtr context);
    [DllImport("authz.dll", SetLastError=true)] static extern bool AuthzAccessCheck(uint flags, IntPtr context, ref AuthzRequest request, IntPtr audit, byte[] descriptor, IntPtr optional, uint optionalCount, ref AuthzReply reply, IntPtr results);
    [DllImport("authz.dll")] static extern bool AuthzFreeContext(IntPtr context);
    [DllImport("authz.dll")] static extern bool AuthzFreeResourceManager(IntPtr manager);

    sealed class Privilege : IDisposable
    {
        IntPtr token;
        TokenPrivileges previous;
        public Privilege(string name)
        {
            if(!OpenProcessToken(GetCurrentProcess(), 0x28, out token)) throw new Win32Exception();
            try {
                Luid id;
                if(!LookupPrivilegeValue(null, name, out id)) throw new Win32Exception();
                var state = new TokenPrivileges { Count=1, Id=id, Attributes=2 };
                uint returned;
                if(!AdjustTokenPrivileges(token, false, ref state, (uint)Marshal.SizeOf(typeof(TokenPrivileges)), out previous, out returned)) throw new Win32Exception();
                int error=Marshal.GetLastWin32Error();
                if(error != 0) throw new Win32Exception(error);
            } catch { CloseHandle(token); token=IntPtr.Zero; throw; }
        }
        public void Dispose()
        {
            if(token == IntPtr.Zero) return;
            TokenPrivileges ignored; uint returned;
            AdjustTokenPrivileges(token, false, ref previous, (uint)Marshal.SizeOf(typeof(TokenPrivileges)), out ignored, out returned);
            CloseHandle(token); token=IntPtr.Zero;
        }
    }
    static void Check(int result) { if(result != 0) throw new Win32Exception(result); }
    static byte[] Bytes(RawSecurityDescriptor descriptor)
    {
        var bytes=new byte[descriptor.BinaryLength]; descriptor.GetBinaryForm(bytes, 0); return bytes;
    }
    public static byte[] Read(string path)
    {
        IntPtr key; Check(RegOpenKeyEx(LocalMachine, path, 0, ReadControl | 0x100, out key));
        try {
            uint size=0; int result=RegGetKeySecurity(key, 7, null, ref size);
            if(result != 122) Check(result);
            var bytes=new byte[size]; Check(RegGetKeySecurity(key, 7, bytes, ref size)); return bytes;
        } finally { RegCloseKey(key); }
    }
    // SeTakeOwnership permits opening WRITE_OWNER without relaxing the parent.
    // Ownership is temporary; the exact previous owner is restored in finally.
    public static void SetDacl(string path, string sddl)
    {
        var original=Read(path);
        var target=new RawSecurityDescriptor(sddl);
        var restoredOwner=new RawSecurityDescriptor(original, 0);
        if(target.Owner != null) restoredOwner.Owner=target.Owner;
        if(target.Group != null) restoredOwner.Group=target.Group;
        using(var take=new Privilege("SeTakeOwnershipPrivilege"))
        using(var restore=new Privilege("SeRestorePrivilege")) {
            IntPtr ownerKey; Check(RegOpenKeyEx(LocalMachine, path, 0, WriteOwner | 0x100, out ownerKey));
            bool took=false;
            try {
                Check(RegSetKeySecurity(ownerKey, 1, Bytes(new RawSecurityDescriptor("O:BA")))); took=true;
                IntPtr aclKey; Check(RegOpenKeyEx(LocalMachine, path, 0, WriteDac | 0x20019 | 0x100, out aclKey));
                try {
                    if(target.DiscretionaryAcl == null) throw new InvalidOperationException("A null DACL is not permitted.");
                    // Changing owner marks OWNER RIGHTS ACEs inherit-only. Restore
                    // ownership, then reapply those ACEs using the granted handle.
                    Check(RegSetKeySecurity(ownerKey, 3, Bytes(restoredOwner))); took=false;
                    uint protection=(target.ControlFlags & ControlFlags.DiscretionaryAclProtected) != 0 ? 0x80000000u : 0x20000000u;
                    Check(RegSetKeySecurity(aclKey, 4u | protection, Bytes(target)));
                } finally { RegCloseKey(aclKey); }
            } finally {
                try { if(took) Check(RegSetKeySecurity(ownerKey, 3, original)); }
                finally { RegCloseKey(ownerKey); }
            }
        }
    }
    public static bool IsAllowedForProcess(int pid, byte[] descriptor, uint desired)
    {
        using(var debug=new Privilege("SeDebugPrivilege")) return CheckProcessAccess(pid, descriptor, desired);
    }
    static bool CheckProcessAccess(int pid, byte[] descriptor, uint desired)
    {
        IntPtr process=OpenProcess(0x1000, false, pid);
        if(process == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcess");
        IntPtr token=IntPtr.Zero, manager=IntPtr.Zero, context=IntPtr.Zero, buffer=IntPtr.Zero;
        try {
            if(!OpenProcessToken(process, 8, out token)) throw new Win32Exception(Marshal.GetLastWin32Error(), "OpenProcessToken");
            if(!AuthzInitializeResourceManager(1, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, "Xonar permission verification", out manager)) throw new Win32Exception();
            if(!AuthzInitializeContextFromToken(0, token, manager, IntPtr.Zero, new Luid(), IntPtr.Zero, out context)) throw new Win32Exception();
            buffer=Marshal.AllocHGlobal(12);
            var reply=new AuthzReply { Count=1, Granted=buffer, Sacl=IntPtr.Add(buffer,4), Error=IntPtr.Add(buffer,8) };
            var request=new AuthzRequest { Desired=desired };
            if(!AuthzAccessCheck(0, context, ref request, IntPtr.Zero, descriptor, IntPtr.Zero, 0, ref reply, IntPtr.Zero)) throw new Win32Exception();
            return Marshal.ReadInt32(reply.Error) == 0 && ((uint)Marshal.ReadInt32(reply.Granted) & desired) == desired;
        } finally {
            if(buffer != IntPtr.Zero) Marshal.FreeHGlobal(buffer);
            if(context != IntPtr.Zero) AuthzFreeContext(context);
            if(manager != IntPtr.Zero) AuthzFreeResourceManager(manager);
            if(token != IntPtr.Zero) CloseHandle(token);
            CloseHandle(process);
        }
    }
}
