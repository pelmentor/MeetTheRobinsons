"""Find process(es) holding a handle to a named mutex.

Strategy:
  1. Create our own mutex with a unique probe name. Its handle gives us a
     reference Mutant kernel object so we can identify the Mutant TypeIndex.
  2. Walk SystemExtendedHandleInformation, filter to entries whose
     ObjectTypeIndex matches Mutant. This drops 139k handles down to a few
     hundred and avoids ever DuplicateHandle'ing pipes/ALPC ports that hang.
  3. For each Mutant handle: DuplicateHandle into our process (with a hard
     per-call timeout via worker thread), NtQueryObject(name), match.

Usage: python tools\\find_mutex_holder.py [name]
       default name: Disney_s_Meet_The_Robinsons
"""

from __future__ import annotations
import ctypes
import ctypes.wintypes as wt
import sys
import threading
import time
import os

# -------- NT API bindings --------

ntdll    = ctypes.WinDLL("ntdll",    use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi    = ctypes.WinDLL("psapi",    use_last_error=True)

NTSTATUS = ctypes.c_long
ULONG_PTR = ctypes.c_size_t

STATUS_INFO_LENGTH_MISMATCH = 0xC0000004 - (1 << 32)  # signed equiv
STATUS_SUCCESS              = 0

SystemExtendedHandleInformation = 0x40
ObjectNameInformation           = 1
ObjectTypeInformation           = 2

PROCESS_DUP_HANDLE              = 0x0040
PROCESS_QUERY_LIMITED_INFO      = 0x1000
DUPLICATE_SAME_ACCESS           = 0x0002

class SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX(ctypes.Structure):
    _fields_ = [
        ("Object",                ctypes.c_void_p),
        ("UniqueProcessId",       ULONG_PTR),
        ("HandleValue",           ULONG_PTR),
        ("GrantedAccess",         wt.ULONG),
        ("CreatorBackTraceIndex", wt.USHORT),
        ("ObjectTypeIndex",       wt.USHORT),
        ("HandleAttributes",      wt.ULONG),
        ("Reserved",              wt.ULONG),
    ]

class UNICODE_STRING(ctypes.Structure):
    _fields_ = [
        ("Length",        wt.USHORT),
        ("MaximumLength", wt.USHORT),
        ("Buffer",        ctypes.c_void_p),
    ]

class OBJECT_NAME_INFORMATION(ctypes.Structure):
    _fields_ = [("Name", UNICODE_STRING)]

class OBJECT_TYPE_INFORMATION(ctypes.Structure):
    _fields_ = [("TypeName", UNICODE_STRING)]
    # rest of struct unused

ntdll.NtQuerySystemInformation.argtypes = [wt.ULONG, ctypes.c_void_p, wt.ULONG, ctypes.POINTER(wt.ULONG)]
ntdll.NtQuerySystemInformation.restype  = NTSTATUS
ntdll.NtQueryObject.argtypes            = [wt.HANDLE, wt.ULONG, ctypes.c_void_p, wt.ULONG, ctypes.POINTER(wt.ULONG)]
ntdll.NtQueryObject.restype             = NTSTATUS

kernel32.OpenProcess.argtypes      = [wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.OpenProcess.restype       = wt.HANDLE
kernel32.DuplicateHandle.argtypes  = [wt.HANDLE, wt.HANDLE, wt.HANDLE, ctypes.POINTER(wt.HANDLE), wt.DWORD, wt.BOOL, wt.DWORD]
kernel32.DuplicateHandle.restype   = wt.BOOL
kernel32.CloseHandle.argtypes      = [wt.HANDLE]
kernel32.CloseHandle.restype       = wt.BOOL
kernel32.GetCurrentProcess.argtypes = []
kernel32.GetCurrentProcess.restype  = wt.HANDLE
kernel32.CreateMutexW.argtypes     = [ctypes.c_void_p, wt.BOOL, wt.LPCWSTR]
kernel32.CreateMutexW.restype      = wt.HANDLE

psapi.GetProcessImageFileNameW.argtypes = [wt.HANDLE, wt.LPWSTR, wt.DWORD]
psapi.GetProcessImageFileNameW.restype  = wt.DWORD

# -------- helpers --------

def query_system_handles():
    size = 0x100000
    while True:
        buf = (ctypes.c_byte * size)()
        ret = wt.ULONG(0)
        st = ntdll.NtQuerySystemInformation(SystemExtendedHandleInformation, buf, size, ctypes.byref(ret))
        if st == STATUS_INFO_LENGTH_MISMATCH:
            size *= 2
            if size > 0x10000000:
                raise RuntimeError("buffer growth exceeded 256 MiB")
            continue
        if st != STATUS_SUCCESS:
            raise OSError(f"NtQuerySystemInformation failed: 0x{st & 0xffffffff:08X}")
        addr = ctypes.addressof(buf)
        n_handles = ULONG_PTR.from_address(addr).value
        ptr_size = ctypes.sizeof(ULONG_PTR)
        first_entry_offset = 2 * ptr_size
        return buf, addr, n_handles, first_entry_offset

def get_handle_typename(h: int, timeout_s: float = 0.3):
    """NtQueryObject(ObjectTypeInformation) with a thread timeout."""
    result = {"name": None, "ok": False}
    def worker():
        try:
            sz = 0x1000
            b = (ctypes.c_byte * sz)()
            ret = wt.ULONG(0)
            st = ntdll.NtQueryObject(h, ObjectTypeInformation, b, sz, ctypes.byref(ret))
            if st == STATUS_SUCCESS:
                ti = OBJECT_TYPE_INFORMATION.from_address(ctypes.addressof(b))
                if ti.TypeName.Length > 0 and ti.TypeName.Buffer:
                    raw = ctypes.string_at(ti.TypeName.Buffer, ti.TypeName.Length)
                    result["name"] = raw.decode("utf-16-le", errors="replace")
                    result["ok"] = True
        except Exception:
            pass
    t = threading.Thread(target=worker, daemon=True)
    t.start()
    t.join(timeout_s)
    return result["name"] if result["ok"] else None

def get_handle_objectname(h: int, timeout_s: float = 0.2):
    result = {"name": None, "ok": False}
    def worker():
        try:
            sz = 0x2000
            b = (ctypes.c_byte * sz)()
            ret = wt.ULONG(0)
            st = ntdll.NtQueryObject(h, ObjectNameInformation, b, sz, ctypes.byref(ret))
            if st == STATUS_SUCCESS:
                ni = OBJECT_NAME_INFORMATION.from_address(ctypes.addressof(b))
                if ni.Name.Length > 0 and ni.Name.Buffer:
                    raw = ctypes.string_at(ni.Name.Buffer, ni.Name.Length)
                    result["name"] = raw.decode("utf-16-le", errors="replace")
                    result["ok"] = True
        except Exception:
            pass
    t = threading.Thread(target=worker, daemon=True)
    t.start()
    t.join(timeout_s)
    return result["name"] if result["ok"] else None

def proc_image_name(pid: int) -> str:
    h = kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFO, False, pid)
    if not h:
        return f"<pid {pid}>"
    try:
        buf = ctypes.create_unicode_buffer(1024)
        n = psapi.GetProcessImageFileNameW(h, buf, 1024)
        if n == 0:
            return f"<pid {pid}>"
        return buf.value
    finally:
        kernel32.CloseHandle(h)

def find_mutant_type_index() -> int:
    # 1. Create a probe mutex that we own.
    probe_name = f"mtr_probe_{os.getpid()}_{int(time.time())}"
    probe_handle = kernel32.CreateMutexW(None, False, probe_name)
    if not probe_handle:
        raise OSError("CreateMutexW(probe) failed")
    try:
        # 2. Walk handle table for our pid, find the entry with HandleValue == probe_handle.
        my_pid = os.getpid()
        buf, addr, n, base_off = query_system_handles()
        entry_size = ctypes.sizeof(SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX)
        for i in range(n):
            e = SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX.from_address(addr + base_off + i * entry_size)
            if e.UniqueProcessId == my_pid and e.HandleValue == probe_handle:
                return e.ObjectTypeIndex
        raise RuntimeError("could not locate probe mutex in handle table")
    finally:
        kernel32.CloseHandle(probe_handle)

# -------- main --------

def main():
    needle = sys.argv[1] if len(sys.argv) > 1 else "Disney_s_Meet_The_Robinsons"
    print(f"target: *{needle}*")

    print("locating Mutant TypeIndex via probe mutex ...")
    mutant_idx = find_mutant_type_index()
    print(f"  Mutant TypeIndex = {mutant_idx}")

    buf, addr, n, base_off = query_system_handles()
    print(f"system has {n} handles total")
    entry_size = ctypes.sizeof(SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX)

    # filter to mutants
    mutant_entries = []
    for i in range(n):
        e = SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX.from_address(addr + base_off + i * entry_size)
        if e.ObjectTypeIndex == mutant_idx:
            pid = int(e.UniqueProcessId)
            if pid in (0, 4):
                continue
            mutant_entries.append((pid, int(e.HandleValue)))
    print(f"  {len(mutant_entries)} Mutant handles to inspect")

    me = kernel32.GetCurrentProcess()
    open_proc_cache: dict[int, int] = {}

    def get_proc(pid: int) -> int:
        if pid in open_proc_cache:
            return open_proc_cache[pid]
        h = kernel32.OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFO, False, pid)
        open_proc_cache[pid] = h or 0
        return open_proc_cache[pid]

    holders = []
    inspected = 0
    for pid, hv in mutant_entries:
        ph = get_proc(pid)
        if not ph:
            continue
        dup = wt.HANDLE(0)
        ok = kernel32.DuplicateHandle(ph, hv, me, ctypes.byref(dup), 0, False, DUPLICATE_SAME_ACCESS)
        if not ok or not dup.value:
            continue
        try:
            name = get_handle_objectname(dup.value)
            inspected += 1
            if name and needle.lower() in name.lower():
                img = proc_image_name(pid)
                holders.append((pid, hv, name, img))
        finally:
            kernel32.CloseHandle(dup.value)

    for h in open_proc_cache.values():
        if h:
            kernel32.CloseHandle(h)

    print(f"  inspected {inspected}/{len(mutant_entries)} (others timed out or denied)")
    print()

    if not holders:
        print(f"NO process holds a Mutant matching '*{needle}*'")
        print("(if mutex still appears alive: a TOCTOU race or the holder is a kernel/system pid)")
    else:
        print(f"HOLDERS of '*{needle}*':")
        print(f"{'PID':>7}  {'HANDLE':>10}  {'IMAGE':<60}  NAME")
        for pid, hv, name, img in holders:
            print(f"{pid:>7}  {hv:>10X}  {img:<60}  {name}")

if __name__ == "__main__":
    main()
