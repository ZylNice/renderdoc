/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2026 Baldur Karlsson
 * Copyright (c) 2014 Crytek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

// must be separate so that it's included first and not sorted by clang-format
#include <windows.h>

#include <Psapi.h>
#include <tchar.h>
#include <tlhelp32.h>
#include "common/formatting.h"
#include "core/core.h"
#include "os/os_specific.h"
#include "strings/string_utils.h"

#include <string>

static rdcarray<EnvironmentModification> &GetEnvModifications()
{
  static rdcarray<EnvironmentModification> envCallbacks;
  return envCallbacks;
}

struct InsensitiveComparison
{
  bool operator()(const rdcstr &a, const rdcstr &b) const { return strlower(a) < strlower(b); }
};

typedef std::map<rdcstr, rdcstr, InsensitiveComparison> EnvMap;

static EnvMap EnvStringToEnvMap(const wchar_t *envstring)
{
  EnvMap ret;

  const wchar_t *e = envstring;

  while(*e)
  {
    const wchar_t *equals = wcschr(e, L'=');

    rdcstr name = StringFormat::Wide2UTF8(rdcwstr(e, equals - e));
    rdcstr value = StringFormat::Wide2UTF8(equals + 1);

    ret[name] = value;

    // jump to \0 and past it
    e += wcslen(e) + 1;
  }

  return ret;
}

void Process::RegisterEnvironmentModification(const EnvironmentModification &modif)
{
  GetEnvModifications().push_back(modif);
}

static void ApplyEnvModifications(EnvMap &envValues,
                                  const rdcarray<EnvironmentModification> &modifications,
                                  bool setToSystem)
{
  for(size_t i = 0; i < modifications.size(); i++)
  {
    const EnvironmentModification &m = modifications[i];

    rdcstr value;

    auto it = envValues.find(m.name);
    if(it != envValues.end())
      value = it->second;

    switch(m.mod)
    {
      case EnvMod::Set: value = m.value.c_str(); break;
      case EnvMod::Append:
      {
        if(!value.empty())
        {
          if(m.sep == EnvSep::Platform || m.sep == EnvSep::SemiColon)
            value += ";";
          else if(m.sep == EnvSep::Colon)
            value += ":";
        }
        value += m.value.c_str();
        break;
      }
      case EnvMod::Prepend:
      {
        if(!value.empty())
        {
          rdcstr prep = m.value;
          if(m.sep == EnvSep::Platform || m.sep == EnvSep::SemiColon)
            prep += ";";
          else if(m.sep == EnvSep::Colon)
            prep += ":";
          value = prep + value;
        }
        else
        {
          value = m.value.c_str();
        }
        break;
      }
    }

    envValues[m.name] = value;

    if(setToSystem)
      SetEnvironmentVariableW(StringFormat::UTF82Wide(m.name).c_str(),
                              StringFormat::UTF82Wide(value).c_str());
  }
}

// on windows we apply environment changes here, after process initialisation
// but before any real work (in RenderDoc::Initialise) so that we support
// injecting the dll into processes we didn't launch (ie didn't control the
// starting environment for), or even the application loading the dll itself
// without any interaction with our replay app.
void Process::ApplyEnvironmentModification()
{
  // turn environment string to a UTF-8 map
  LPWCH envStrings = GetEnvironmentStringsW();
  EnvMap envValues = EnvStringToEnvMap(envStrings);
  FreeEnvironmentStringsW(envStrings);
  rdcarray<EnvironmentModification> &modifications = GetEnvModifications();

  ApplyEnvModifications(envValues, modifications, true);

  // these have been applied to the current process
  modifications.clear();
}

rdcstr Process::GetEnvVariable(const rdcstr &name)
{
  DWORD len = GetEnvironmentVariableA(name.c_str(), NULL, 0);
  if(len == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
    return rdcstr();

  rdcstr ret;
  ret.resize(len + 1);

  GetEnvironmentVariableA(name.c_str(), ret.data(), len);
  ret.trim();
  return ret;
}

uint64_t Process::GetMemoryUsage()
{
  HANDLE proc = GetCurrentProcess();

  if(proc == NULL)
  {
    RDCERR("Couldn't open process: %d", GetLastError());
    return 0;
  }

  PROCESS_MEMORY_COUNTERS memInfo = {};

  uint64_t ret = 0;

  if(GetProcessMemoryInfo(proc, &memInfo, sizeof(memInfo)))
  {
    ret = memInfo.WorkingSetSize;
  }
  else
  {
    RDCERR("Couldn't get process memory info: %d", GetLastError());
  }

  return ret;
}

// helpers for various shims and dlls etc, not part of the public API
extern "C" __declspec(dllexport) void __cdecl INTERNAL_GetTargetControlIdent(uint32_t *ident)
{
  if(ident)
    *ident = RenderDoc::Inst().GetTargetControlIdent();
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetCaptureOptions(CaptureOptions *opts)
{
  if(opts)
    RenderDoc::Inst().SetCaptureOptions(*opts);
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetCaptureFile(const char *capfile)
{
  if(capfile)
    RenderDoc::Inst().SetCaptureFileTemplate(capfile);
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_SetDebugLogFile(const char *logfile)
{
  RENDERDOC_SetDebugLogFile(logfile ? logfile : rdcstr());
}

static EnvironmentModification tempEnvMod;

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvModName(const char *name)
{
  if(name)
    tempEnvMod.name = name;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvModValue(const char *value)
{
  if(value)
    tempEnvMod.value = value;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvSep(EnvSep *sep)
{
  if(sep)
    tempEnvMod.sep = *sep;
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_EnvMod(EnvMod *mod)
{
  if(mod)
  {
    tempEnvMod.mod = *mod;
    Process::RegisterEnvironmentModification(tempEnvMod);
  }
}

extern "C" __declspec(dllexport) void __cdecl INTERNAL_ApplyEnvMods(void *ignored)
{
  Process::ApplyEnvironmentModification();
}

// ------------------------------------------------------------------------
// Thread-context based injection.
//
// CreateRemoteThread is a well-known injection vector which is commonly
// monitored or blocked by anti-cheat and crash-reporting systems. For a
// freshly launched (CREATE_SUSPENDED) process we can instead hijack the main
// thread: point its context at a small stub which calls a function, parks in
// a spin loop until we suspend it again, and finally restore the original
// context so the process starts up as normal.
// ------------------------------------------------------------------------

struct HijackState
{
  CONTEXT origCtx;
  bool saved = false;
};

static bool WriteStubCall(HANDLE hProcess, uintptr_t funcAddr, uintptr_t argAddr,
                          bool x86CalleePops, void **stubMemOut, DWORD **flagOut)
{
#if ENABLED(RDOC_X64)
  // x64 stub (single calling convention, caller provides shadow space):
  //   and rsp, -0x10            48 83 E4 F0
  //   sub rsp, 0x20             48 83 EC 20
  //   mov rcx, <arg>            48 B9 imm64
  //   mov rax, <func>           48 B8 imm64
  //   call rax                  FF D0
  //   mov rdx, <flag>           48 BA imm64
  //   mov dword ptr [rdx], 1    C7 02 01 00 00 00
  //   jmp $                     EB FE
  const size_t stubSize = 64;
  byte stub[64] = {0};
  size_t o = 0;

  stub[o++] = 0x48;
  stub[o++] = 0x83;
  stub[o++] = 0xE4;
  stub[o++] = 0xF0;

  stub[o++] = 0x48;
  stub[o++] = 0x83;
  stub[o++] = 0xEC;
  stub[o++] = 0x20;

  stub[o++] = 0x48;
  stub[o++] = 0xB9;
  memcpy(stub + o, &argAddr, sizeof(argAddr));
  o += sizeof(argAddr);

  stub[o++] = 0x48;
  stub[o++] = 0xB8;
  memcpy(stub + o, &funcAddr, sizeof(funcAddr));
  o += sizeof(funcAddr);

  stub[o++] = 0xFF;
  stub[o++] = 0xD0;

  // mov rdx, imm64   48 BA <imm64>
  stub[o++] = 0x48;
  stub[o++] = 0xBA;
  // the imm64 is patched later - record where it lives in the stub
  const size_t flagOperandOffset = o;
  o += sizeof(uintptr_t);

  // mov dword ptr [rdx], 1   C7 02 01 00 00 00
  stub[o++] = 0xC7;
  stub[o++] = 0x02;
  stub[o++] = 0x01;
  stub[o++] = 0x00;
  stub[o++] = 0x00;
  stub[o++] = 0x00;

  stub[o++] = 0xEB;
  stub[o++] = 0xFE;
#else
  // x86 stub:
  //   push <arg>                68 imm32
  //   mov eax, <func>           B8 imm32
  //   call eax                  FF D0
  //   add esp, 4                83 C4 04   (cdecl only, omitted for stdcall)
  //   mov dword ptr [<flag>], 1 C7 05 imm32 01 00 00 00
  //   jmp $                     EB FE
  const size_t stubSize = 48;
  byte stub[48] = {0};
  size_t o = 0;

  stub[o++] = 0x68;
  memcpy(stub + o, &argAddr, sizeof(argAddr));
  o += sizeof(argAddr);

  stub[o++] = 0xB8;
  memcpy(stub + o, &funcAddr, sizeof(funcAddr));
  o += sizeof(funcAddr);

  stub[o++] = 0xFF;
  stub[o++] = 0xD0;

  if(!x86CalleePops)
  {
    stub[o++] = 0x83;
    stub[o++] = 0xC4;
    stub[o++] = 0x04;
  }

  // mov dword ptr [imm32],], 1   C7 05 <imm32> 01 00 00 00
  stub[o++] = 0xC7;
  stub[o++] = 0x05;
  // the imm32 is patched later - record where it lives in the stub
  const size_t flagOperandOffset = o;
  o += sizeof(uint32_t);

  stub[o++] = 0x01;
  stub[o++] = 0x00;
  stub[o++] = 0x00;
  stub[o++] = 0x00;

  stub[o++] = 0xEB;
  stub[o++] = 0xFE;
#endif

  void *stubMem = VirtualAllocEx(hProcess, NULL, stubSize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);

  if(stubMem == NULL)
    return false;

  // the completion flag lives in the last 4 bytes of the stub allocation
  uintptr_t flagAddr = (uintptr_t)stubMem + stubSize - sizeof(DWORD);
  memcpy(stub + flagOperandOffset, &flagAddr, sizeof(flagAddr));

  SIZE_T numWritten = 0;

  if(!WriteProcessMemory(hProcess, stubMem, stub, stubSize, &numWritten) || numWritten != stubSize)
  {
    VirtualFreeEx(hProcess, stubMem, 0, MEM_RELEASE);
    return false;
  }

  *stubMemOut = stubMem;
  *flagOut = (DWORD *)flagAddr;
  return true;
}

// Runs func(arg) on a hijacked thread. The thread must currently be suspended.
// It is resumed to execute the stub, then re-suspended once the completion flag
// is set (or a timeout occurs). On the first call the original context is
// saved into hj so it can later be restored with RestoreHijackedThread.
static bool HijackedExec(HANDLE hProcess, HANDLE hThread, HijackState &hj, uintptr_t funcAddr,
                         uintptr_t argAddr, bool x86CalleePops, uint32_t timeoutMS)
{
  void *stubMem = NULL;
  DWORD *flagRemote = NULL;

  if(!WriteStubCall(hProcess, funcAddr, argAddr, x86CalleePops, &stubMem, &flagRemote))
  {
    RDCERR("Couldn't write remote stub for hijacked call");
    return false;
  }

  CONTEXT ctx;
  RDCEraseEl(ctx);
  ctx.ContextFlags = CONTEXT_FULL;

  if(!GetThreadContext(hThread, &ctx))
  {
    RDCERR("GetThreadContext failed: %u", GetLastError());
    VirtualFreeEx(hProcess, stubMem, 0, MEM_RELEASE);
    return false;
  }

  if(!hj.saved)
  {
    hj.origCtx = ctx;
    hj.saved = true;
  }

#if ENABLED(RDOC_X64)
  ctx.Rip = (DWORD64)(uintptr_t)stubMem;
#else
  ctx.Eip = (DWORD)(uintptr_t)stubMem;
#endif

  if(!SetThreadContext(hThread, &ctx))
  {
    RDCERR("SetThreadContext failed: %u", GetLastError());
    VirtualFreeEx(hProcess, stubMem, 0, MEM_RELEASE);
    return false;
  }

  if(ResumeThread(hThread) == (DWORD)-1)
  {
    RDCERR("ResumeThread failed: %u", GetLastError());
    VirtualFreeEx(hProcess, stubMem, 0, MEM_RELEASE);
    return false;
  }

  bool done = false;
  uint32_t elapsed = 0;

  while(elapsed < timeoutMS)
  {
    Sleep(10);
    elapsed += 10;

    // bail out early if the process died while we were waiting
    if(WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0)
    {
      RDCERR("Process exited while waiting for hijacked call");
      return false;
    }

    DWORD flag = 0;
    SIZE_T numRead = 0;

    if(ReadProcessMemory(hProcess, flagRemote, &flag, sizeof(flag), &numRead) && flag == 1)
    {
      done = true;
      break;
    }
  }

  // re-suspend the thread. It is either spinning in the stub's jmp $ loop, or
  // (in the timeout case) still somewhere inside the called function.
  SuspendThread(hThread);

  if(done)
  {
    VirtualFreeEx(hProcess, stubMem, 0, MEM_RELEASE);
  }
  else
  {
    // don't free the stub on timeout - the thread may still execute it
    RDCERR("Timed out after %ums waiting for hijacked call", timeoutMS);
  }

  return done;
}

// If the process was freshly created suspended (or is otherwise suspended
// early in startup), returns a handle to its main thread ready for hijacking.
// A freshly created process only has a single thread, so we enumerate the
// process's threads and only consider hijacking when exactly one exists and
// it is suspended. Note that the initial thread ID is NOT the same as the
// process ID on Windows. Returns NULL if the thread can't be opened or is
// running, in which case the classic CreateRemoteThread paths must be used.
static HANDLE GetSuspendedMainThread(DWORD pid)
{
  // enumerate all threads belonging to this process. The snapshot pid
  // parameter is ignored for TH32CS_SNAPTHREAD, we filter by owner ourselves.
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

  if(snap == INVALID_HANDLE_VALUE)
    return NULL;

  THREADENTRY32 te;
  RDCEraseEl(te);
  te.dwSize = sizeof(te);

  DWORD mainTid = 0;
  int threadCount = 0;

  if(Thread32First(snap, &te))
  {
    do
    {
      if(te.th32OwnerProcessID == pid)
      {
        threadCount++;
        mainTid = te.th32ThreadID;
      }
    } while(Thread32Next(snap, &te));
  }

  CloseHandle(snap);

  // more than one thread means the process is up and running (or was created
  // with extra threads) - not safe to hijack
  if(threadCount != 1 || mainTid == 0)
    return NULL;

  HANDLE hThread = OpenThread(
      THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION,
      FALSE, mainTid);

  if(hThread == NULL)
    return NULL;

  // suspend (incrementing the count), check whether the thread was already
  // suspended, then undo our suspension to restore the previous state
  DWORD prev = SuspendThread(hThread);

  if(prev == 0)
  {
    // thread was running - not safe to hijack
    ResumeThread(hThread);
    CloseHandle(hThread);
    return NULL;
  }

  ResumeThread(hThread);

  return hThread;
}

// Restores the original context saved in hj, leaving the thread suspended.
static void RestoreHijackedThread(HANDLE hThread, HijackState &hj)
{
  if(!hj.saved)
    return;

  CONTEXT ctx = hj.origCtx;
  ctx.ContextFlags = CONTEXT_FULL;

  if(!SetThreadContext(hThread, &ctx))
    RDCERR("Failed to restore hijacked thread context: %u", GetLastError());
}

// Reads the remote EXE entry point from the process PEB + PE headers.
static bool GetRemoteEntryPoint(HANDLE hProcess, uintptr_t &entryPoint)
{
  typedef LONG(NTAPI * pNtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);

  static pNtQueryInformationProcess ntQueryInfo = NULL;

  if(ntQueryInfo == NULL)
  {
    ntQueryInfo = (pNtQueryInformationProcess)GetProcAddress(
        GetModuleHandleA("ntdll.dll"), "NtQueryInformationProcess");

    if(ntQueryInfo == NULL)
    {
      RDCERR("Couldn't find NtQueryInformationProcess in ntdll.dll");
      return false;
    }
  }

  // minimal PROCESS_BASIC_INFORMATION, so we don't need winternl.h
  struct RemoteProcessBasicInformation
  {
    void *exitStatus;
    void *pebBaseAddress;
    void *reserved2[2];
    uintptr_t uniqueProcessId;
    void *reserved3;
  };

  RemoteProcessBasicInformation pbi;
  RDCEraseEl(pbi);

  if(ntQueryInfo(hProcess, 0 /* ProcessBasicInformation */, &pbi, sizeof(pbi), NULL) != 0)
  {
    RDCERR("NtQueryInformationProcess failed: %u", GetLastError());
    return false;
  }

  // PEB->ImageBaseAddress sits at offset 0x10 on x64 (0x8 holds the Mutant
  // handle there) and at offset 0x8 on x86
  void *imageBase = NULL;

#if ENABLED(RDOC_X64)
  const size_t pebImageBaseOffset = 0x10;
#else
  const size_t pebImageBaseOffset = 0x8;
#endif

  if(!ReadProcessMemory(hProcess, (char *)pbi.pebBaseAddress + pebImageBaseOffset, &imageBase,
                        sizeof(imageBase), NULL))
  {
    RDCERR("Couldn't read remote PEB image base: %u", GetLastError());
    return false;
  }

  DWORD e_lfanew = 0;
  if(!ReadProcessMemory(hProcess, (char *)imageBase + 0x3c, &e_lfanew, sizeof(e_lfanew), NULL))
  {
    RDCERR("Couldn't read remote DOS header: %u", GetLastError());
    return false;
  }

  // IMAGE_NT_HEADERS is: Signature (4 bytes) + IMAGE_FILE_HEADER (20 bytes) +
  // IMAGE_OPTIONAL_HEADER. AddressOfEntryPoint sits at offset 16 in the
  // optional header and is a DWORD in both PE32 and PE32+.
  DWORD entryRVA = 0;
  if(!ReadProcessMemory(hProcess, (char *)imageBase + e_lfanew + 4 + 20 + 16, &entryRVA,
                        sizeof(entryRVA), NULL))
  {
    RDCERR("Couldn't read remote PE header: %u", GetLastError());
    return false;
  }

  entryPoint = (uintptr_t)imageBase + entryRVA;
  return true;
}

// As InjectDLL, but loads the library by hijacking the (suspended) main
// thread instead of via CreateRemoteThread.
//
// A CREATE_SUSPENDED process is suspended *before* LdrInitializeThunk, i.e.
// before its statically imported DLLs (potentially including the graphics API)
// are loaded. Hijacking at that point would let those imports load through
// ntdll's internal loader path, bypassing the LoadLibrary hooks entirely, so
// no graphics API would ever be detected. Instead we patch the EXE entry point
// with a 2-byte spin loop, resume the thread to let it complete process
// initialisation, wait for it to arrive at the entry point, then unpatch and
// hijack from there - by which point all statically imported DLLs are loaded
// and can be hooked directly.
static bool InjectDLLThreadContext(HANDLE hProcess, HANDLE hThread, HijackState &hj,
                                   rdcwstr libName)
{
  uintptr_t entryPoint = 0;

  if(!GetRemoteEntryPoint(hProcess, entryPoint))
  {
    RDCERR("Couldn't locate remote EXE entry point");
    return false;
  }

  RDCLOG("Hijack: EXE entry point located at 0x%llx", (unsigned long long)entryPoint);

  BYTE origBytes[2] = {0, 0};
  SIZE_T numBytes = 0;

  if(!ReadProcessMemory(hProcess, (void *)entryPoint, origBytes, sizeof(origBytes), &numBytes) ||
     numBytes != sizeof(origBytes))
  {
    RDCERR("Couldn't read remote entry point bytes: %u", GetLastError());
    return false;
  }

  DWORD oldProtect = 0;

  if(!VirtualProtectEx(hProcess, (void *)entryPoint, sizeof(origBytes), PAGE_EXECUTE_READWRITE,
                       &oldProtect))
  {
    RDCERR("Couldn't make remote entry point writable: %u", GetLastError());
    return false;
  }

  // jmp $ - a 2 byte infinite loop
  const BYTE spin[2] = {0xEB, 0xFE};
  SIZE_T numWritten = 0;

  if(!WriteProcessMemory(hProcess, (void *)entryPoint, spin, sizeof(spin), &numWritten) ||
     numWritten != sizeof(spin))
  {
    RDCERR("Couldn't patch remote entry point: %u", GetLastError());
    VirtualProtectEx(hProcess, (void *)entryPoint, sizeof(origBytes), oldProtect, &oldProtect);
    return false;
  }

  // let the thread run - it completes process initialisation (loading all
  // statically imported DLLs) then spins forever at the entry point
  if(ResumeThread(hThread) == (DWORD)-1)
  {
    RDCERR("ResumeThread failed: %u", GetLastError());
    WriteProcessMemory(hProcess, (void *)entryPoint, origBytes, sizeof(origBytes), &numWritten);
    VirtualProtectEx(hProcess, (void *)entryPoint, sizeof(origBytes), oldProtect, &oldProtect);
    return false;
  }

  RDCLOG("Hijack: entry point patched, waiting for process initialisation to complete");

  // wait until the thread reaches the entry point spin loop
  bool reachedEntry = false;
  CONTEXT ctx;
  uint32_t elapsed = 0;

  while(elapsed < 60000)
  {
    Sleep(10);
    elapsed += 10;

    RDCEraseEl(ctx);
    ctx.ContextFlags = CONTEXT_FULL;

    if(!GetThreadContext(hThread, &ctx))
      break;

#if ENABLED(RDOC_X64)
    uintptr_t rip = (uintptr_t)ctx.Rip;
#else
    uintptr_t rip = (uintptr_t)ctx.Eip;
#endif

    if(rip == entryPoint || rip == entryPoint + 2)
    {
      reachedEntry = true;
      break;
    }
  }

  SuspendThread(hThread);

  // restore the original entry point bytes
  WriteProcessMemory(hProcess, (void *)entryPoint, origBytes, sizeof(origBytes), &numWritten);
  VirtualProtectEx(hProcess, (void *)entryPoint, sizeof(origBytes), oldProtect, &oldProtect);

  if(!reachedEntry)
  {
    RDCERR("Timed out waiting for process to reach its entry point");
    // the thread is suspended with its context and entry point untouched, so
    // the process can still run on normally (or a fallback can inject)
    return false;
  }

  RDCLOG("Hijack: process reached its entry point after %ums, initialisation complete", elapsed);

  // normalise the instruction pointer onto the entry point itself - the thread
  // may be suspended at entryPoint+2, inside the (now removed) spin loop
#if ENABLED(RDOC_X64)
  ctx.Rip = (DWORD64)entryPoint;
#else
  ctx.Eip = (DWORD)entryPoint;
#endif

  if(!SetThreadContext(hThread, &ctx))
  {
    RDCERR("Couldn't normalise thread context on the entry point: %u", GetLastError());
    return false;
  }

  wchar_t dllPath[MAX_PATH + 1] = {0};
  wcscpy_s(dllPath, libName.c_str());

  static HMODULE kernel32 = GetModuleHandleA("kernel32.dll");

  if(kernel32 == NULL)
  {
    RDCERR("Couldn't get handle for kernel32.dll");
    return false;
  }

  uintptr_t loadLibraryW = (uintptr_t)GetProcAddress(kernel32, "LoadLibraryW");

  if(loadLibraryW == 0)
  {
    RDCERR("Couldn't find LoadLibraryW in kernel32.dll");
    return false;
  }

  void *remotePath = VirtualAllocEx(hProcess, NULL, sizeof(dllPath), MEM_COMMIT, PAGE_READWRITE);

  if(remotePath == NULL)
  {
    RDCERR("Couldn't allocate remote memory for DLL '%ls': %u", libName.c_str(), GetLastError());
    return false;
  }

  BOOL success = WriteProcessMemory(hProcess, remotePath, dllPath, sizeof(dllPath), &numWritten);

  if(!success)
  {
    RDCERR("Couldn't write remote memory %p with dllPath '%ls': %u", remotePath, dllPath,
           GetLastError());
  }
  else
  {
    // LoadLibraryW is stdcall on x86, so the callee pops the argument
    success =
        HijackedExec(hProcess, hThread, hj, loadLibraryW, (uintptr_t)remotePath, true, 30000);
  }

  VirtualFreeEx(hProcess, remotePath, 0, MEM_RELEASE);

  return success != FALSE;
}

// As InjectFunctionCall, but runs the call on the hijacked main thread instead
// of via CreateRemoteThread. The thread must be suspended when called, and is
// left suspended on return.
static void InjectFunctionCallThreadContext(HANDLE hProcess, HANDLE hThread, HijackState &hj,
                                            uintptr_t renderdoc_remote, const char *funcName,
                                            void *data, const size_t dataLen)
{
  if(dataLen == 0)
  {
    RDCERR("Invalid function call injection attempt");
    return;
  }

  RDCDEBUG("Injecting call to %s", funcName);

  HMODULE renderdoc_local = GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll");

  uintptr_t func_local = (uintptr_t)GetProcAddress(renderdoc_local, funcName);

  // we've found the function in our local instance of the module, now
  // calculate the offset and so get the function in the remote module (which
  // might be loaded at a different base address)
  uintptr_t func_remote = func_local + renderdoc_remote - (uintptr_t)renderdoc_local;

  void *remoteMem = VirtualAllocEx(hProcess, NULL, dataLen, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  SIZE_T numWritten;
  WriteProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten);

  // INTERNAL_* functions are __cdecl on x86, so the caller pops the argument
  HijackedExec(hProcess, hThread, hj, func_remote, (uintptr_t)remoteMem, false, 10000);

  ReadProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten);

  VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
}

void InjectDLL(HANDLE hProcess, rdcwstr libName)
{
  wchar_t dllPath[MAX_PATH + 1] = {0};
  wcscpy_s(dllPath, libName.c_str());

  static HMODULE kernel32 = GetModuleHandleA("kernel32.dll");

  if(kernel32 == NULL)
  {
    RDCERR("Couldn't get handle for kernel32.dll");
    return;
  }

  void *remoteMem =
      VirtualAllocEx(hProcess, NULL, sizeof(dllPath), MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if(remoteMem)
  {
    BOOL success = WriteProcessMemory(hProcess, remoteMem, (void *)dllPath, sizeof(dllPath), NULL);
    if(success)
    {
      HANDLE hThread = CreateRemoteThread(
          hProcess, NULL, 1024 * 1024U,
          (LPTHREAD_START_ROUTINE)GetProcAddress(kernel32, "LoadLibraryW"), remoteMem, 0, NULL);
      if(hThread)
      {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
      }
      else
      {
        RDCERR("Couldn't create remote thread for LoadLibraryW: %u", GetLastError());
      }
    }
    else
    {
      RDCERR("Couldn't write remote memory %p with dllPath '%ls': %u", remoteMem, dllPath,
             GetLastError());
    }

    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
  }
  else
  {
    RDCERR("Couldn't allocate remote memory for DLL '%ls': %u", libName.c_str(), GetLastError());
  }
}

uintptr_t FindRemoteDLL(DWORD pid, rdcstr libName)
{
  HANDLE hModuleSnap = INVALID_HANDLE_VALUE;

  rdcwstr wlibName = StringFormat::UTF82Wide(strlower(libName));

  // up to 10 retries
  for(int i = 0; i < 10; i++)
  {
    hModuleSnap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);

    if(hModuleSnap == INVALID_HANDLE_VALUE)
    {
      DWORD err = GetLastError();

      RDCWARN("CreateToolhelp32Snapshot(%u) -> 0x%08x", pid, err);

      // retry if error is ERROR_BAD_LENGTH
      if(err == ERROR_BAD_LENGTH)
        continue;
    }

    // didn't retry, or succeeded
    break;
  }

  if(hModuleSnap == INVALID_HANDLE_VALUE)
  {
    RDCERR("Couldn't create toolhelp dump of modules in process %u", pid);
    return 0;
  }

  MODULEENTRY32 me32;
  RDCEraseEl(me32);
  me32.dwSize = sizeof(MODULEENTRY32);

  BOOL success = Module32First(hModuleSnap, &me32);

  if(success == FALSE)
  {
    DWORD err = GetLastError();

    RDCERR("Couldn't get first module in process %u: 0x%08x", pid, err);
    CloseHandle(hModuleSnap);
    return 0;
  }

  uintptr_t ret = 0;

  int numModules = 0;

  do
  {
    wchar_t modnameLower[MAX_MODULE_NAME32 + 1];
    RDCEraseEl(modnameLower);
    wcsncpy_s(modnameLower, me32.szModule, MAX_MODULE_NAME32);

    wchar_t *wc = &modnameLower[0];
    while(*wc)
    {
      *wc = towlower(*wc);
      wc++;
    }

    numModules++;

    if(wcsstr(modnameLower, wlibName.c_str()) == modnameLower)
    {
      ret = (uintptr_t)me32.modBaseAddr;
    }
  } while(ret == 0 && Module32Next(hModuleSnap, &me32));

  if(ret == 0)
  {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);

    DWORD exitCode = 0;

    if(h)
      GetExitCodeProcess(h, &exitCode);

    if(h == NULL || exitCode != STILL_ACTIVE)
    {
      RDCERR(
          "Error injecting into remote process with PID %u which is no longer available.\n"
          "Possibly the process has crashed during early startup, or is missing DLLs to run?",
          pid);
    }
    else
    {
      RDCERR("Couldn't find module '%s' among %d modules", libName.c_str(), numModules);
    }

    if(h)
      CloseHandle(h);
  }

  CloseHandle(hModuleSnap);

  return ret;
}

void InjectFunctionCall(HANDLE hProcess, uintptr_t renderdoc_remote, const char *funcName,
                        void *data, const size_t dataLen)
{
  if(dataLen == 0)
  {
    RDCERR("Invalid function call injection attempt");
    return;
  }

  RDCDEBUG("Injecting call to %s", funcName);

  HMODULE renderdoc_local = GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll");

  uintptr_t func_local = (uintptr_t)GetProcAddress(renderdoc_local, funcName);

  // we've found SetCaptureOptions in our local instance of the module, now calculate the offset and
  // so get the function
  // in the remote module (which might be loaded at a different base address
  uintptr_t func_remote = func_local + renderdoc_remote - (uintptr_t)renderdoc_local;

  void *remoteMem = VirtualAllocEx(hProcess, NULL, dataLen, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  SIZE_T numWritten;
  WriteProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten);

  HANDLE hThread =
      CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)func_remote, remoteMem, 0, NULL);
  WaitForSingleObject(hThread, INFINITE);

  ReadProcessMemory(hProcess, remoteMem, data, dataLen, &numWritten);

  CloseHandle(hThread);
  VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
}

void Process::FixBundledCRTForTarget(const wchar_t *appPath)
{
  if(!appPath || !*appPath)
    return;

  static const wchar_t *crtNames[] = {
      L"msvcp140.dll",    L"msvcp140_1.dll",  L"msvcp140_2.dll",
      L"vcruntime140.dll", L"vcruntime140_1.dll", L"ucrtbase.dll",
  };

  wchar_t exeDir[MAX_PATH] = {0};
  wcsncpy_s(exeDir, appPath, MAX_PATH - 1);
  wchar_t *slash = wcsrchr(exeDir, L'\\');
  if(slash)
    *slash = 0;

  wchar_t systemDir[MAX_PATH] = {0};
  GetSystemDirectoryW(systemDir, MAX_PATH);

  // check the exe's own directory and a "runtime" subdirectory (NTE launcher layout)
  for(int d = 0; d < 2; d++)
  {
    wchar_t dir[MAX_PATH] = {0};
    if(d == 0)
      swprintf_s(dir, MAX_PATH, L"%s", exeDir);
    else
      swprintf_s(dir, MAX_PATH, L"%s\\runtime", exeDir);

    wchar_t probe[MAX_PATH] = {0};
    swprintf_s(probe, MAX_PATH, L"%s\\msvcp140.dll", dir);
    if(GetFileAttributesW(probe) == INVALID_FILE_ATTRIBUTES)
      continue;

    // only replace when the bundled CRT is older than what rendertest.dll can initialise against
    bool replace = true;
    DWORD verHandle = 0;
    DWORD verSize = GetFileVersionInfoSizeW(probe, &verHandle);
    if(verSize)
    {
      rdcarray<BYTE> verBuf;
      verBuf.resize(verSize);
      if(GetFileVersionInfoW(probe, verHandle, verSize, verBuf.data()))
      {
        VS_FIXEDFILEINFO *ffi = NULL;
        UINT ffiLen = 0;
        if(VerQueryValueW(verBuf.data(), L"\\", (LPVOID *)&ffi, &ffiLen) && ffi)
        {
          DWORD major = HIWORD(ffi->dwFileVersionMS);
          DWORD minor = LOWORD(ffi->dwFileVersionMS);
          replace = (major < 14) || (major == 14 && minor < 40);
        }
      }
    }
    if(!replace)
      continue;

    wchar_t backupDir[MAX_PATH] = {0};
    swprintf_s(backupDir, MAX_PATH, L"%s\\crt_backup", dir);
    if(GetFileAttributesW(backupDir) == INVALID_FILE_ATTRIBUTES)
      CreateDirectoryW(backupDir, NULL);

    for(size_t i = 0; i < ARRAY_COUNT(crtNames); i++)
    {
      wchar_t dst[MAX_PATH] = {0};
      swprintf_s(dst, MAX_PATH, L"%s\\%s", dir, crtNames[i]);
      wchar_t bak[MAX_PATH] = {0};
      swprintf_s(bak, MAX_PATH, L"%s\\%s", backupDir, crtNames[i]);
      wchar_t src[MAX_PATH] = {0};
      swprintf_s(src, MAX_PATH, L"%s\\%s", systemDir, crtNames[i]);

      if(GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES &&
         GetFileAttributesW(bak) == INVALID_FILE_ATTRIBUTES)
        CopyFileW(dst, bak, TRUE);
      if(GetFileAttributesW(src) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(src, dst, FALSE);
    }

    RDCLOG("FixBundledCRT: replaced bundled CRT in %ls with system CRT", dir);
  }
}

static PROCESS_INFORMATION RunProcess(const rdcstr &app, const rdcstr &workingDir,
                                      const rdcstr &cmdLine,
                                      const rdcarray<EnvironmentModification> &env, bool internal,
                                      HANDLE *phChildStdOutput_Rd, HANDLE *phChildStdError_Rd)
{
  PROCESS_INFORMATION pi;
  STARTUPINFO si;
  SECURITY_ATTRIBUTES pSec;
  SECURITY_ATTRIBUTES tSec;

  RDCEraseEl(pi);
  RDCEraseEl(si);
  RDCEraseEl(pSec);
  RDCEraseEl(tSec);

  si.cb = sizeof(si);

  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  rdcwstr workdir = L"";

  if(!workingDir.empty())
    workdir = StringFormat::UTF82Wide(workingDir);
  else
    workdir = StringFormat::UTF82Wide(get_dirname(app));

  wchar_t *paramsAlloc = NULL;

  rdcwstr wapp = StringFormat::UTF82Wide(app);

  // CreateProcessW can modify the params, need space.
  size_t len = wapp.length() + 10;

  rdcwstr wcmd = L"";

  if(!cmdLine.empty())
  {
    wcmd = StringFormat::UTF82Wide(cmdLine);
    len += wcmd.length();
  }

  paramsAlloc = new wchar_t[len];

  RDCEraseMem(paramsAlloc, len * sizeof(wchar_t));

  wcscpy_s(paramsAlloc, len, L"\"");
  wcscat_s(paramsAlloc, len, wapp.c_str());
  wcscat_s(paramsAlloc, len, L"\"");

  if(!cmdLine.empty())
  {
    wcscat_s(paramsAlloc, len, L" ");
    wcscat_s(paramsAlloc, len, wcmd.c_str());
  }

  bool inheritHandles = false;

  HANDLE hChildStdOutput_Wr = 0, hChildStdError_Wr = 0;
  if(phChildStdOutput_Rd)
  {
    RDCASSERT(phChildStdError_Rd);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    if(!CreatePipe(phChildStdOutput_Rd, &hChildStdOutput_Wr, &sa, 0))
      RDCERR("Could not create pipe to read stdout");
    if(!SetHandleInformation(*phChildStdOutput_Rd, HANDLE_FLAG_INHERIT, 0))
      RDCERR("Could not set pipe handle information");

    if(!CreatePipe(phChildStdError_Rd, &hChildStdError_Wr, &sa, 0))
      RDCERR("Could not create pipe to read stdout");
    if(!SetHandleInformation(*phChildStdError_Rd, HANDLE_FLAG_INHERIT, 0))
      RDCERR("Could not set pipe handle information");

    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hChildStdOutput_Wr;
    si.hStdError = hChildStdError_Wr;

    // Need to inherit handles in CreateProcess for ReadFile to read stdout
    inheritHandles = true;
  }

  // if it's a utility launch, hide the command prompt window from showing
  if(phChildStdOutput_Rd || internal)
    si.dwFlags |= STARTF_USESHOWWINDOW;

  if(!internal)
    RDCLOG("Running process %s", app.c_str());

  // turn environment string to a UTF-8 map
  std::wstring envString;

  if(!env.empty())
  {
    LPWCH envStrings = GetEnvironmentStringsW();
    EnvMap envValues = EnvStringToEnvMap(envStrings);
    FreeEnvironmentStringsW(envStrings);

    ApplyEnvModifications(envValues, env, false);

    for(auto it = envValues.begin(); it != envValues.end(); ++it)
    {
      envString += StringFormat::UTF82Wide(it->first).c_str();
      envString += L"=";
      envString += StringFormat::UTF82Wide(it->second).c_str();
      envString.push_back(0);
    }
  }

  // Some games/launchers bundle an old CRT next to the exe (or in a runtime subdirectory) that
  // rendertest.dll cannot initialise against. Replace it with the system CRT before creating the
  // process so the DLL loads no matter which path resolved the CRT.
  {
    rdcwstr appWide = StringFormat::UTF82Wide(app);
    Process::FixBundledCRTForTarget(appWide.c_str());
  }

  BOOL retValue = CreateProcessW(
      NULL, paramsAlloc, &pSec, &tSec, inheritHandles, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
      envString.empty() ? NULL : (void *)envString.data(), workdir.c_str(), &si, &pi);

  DWORD err = GetLastError();

  if(phChildStdOutput_Rd)
  {
    CloseHandle(hChildStdOutput_Wr);
    CloseHandle(hChildStdError_Wr);
  }

  SAFE_DELETE_ARRAY(paramsAlloc);

  if(!retValue)
  {
    if(!internal)
      RDCWARN("Process %s could not be loaded (error %d).", app.c_str(), err);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    RDCEraseEl(pi);
  }

  return pi;
}

rdcpair<RDResult, uint32_t> Process::InjectIntoProcess(uint32_t pid,
                                                       const rdcarray<EnvironmentModification> &env,
                                                       const rdcstr &capturefile,
                                                       const CaptureOptions &opts, bool waitForExit)
{
  rdcwstr wcapturefile = StringFormat::UTF82Wide(capturefile);

  HANDLE hProcess =
      OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                      PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                  FALSE, pid);

  if(opts.delayForDebugger > 0)
  {
    RDCDEBUG("Waiting for debugger attach to %lu", pid);
    uint32_t timeout = 0;

    BOOL debuggerAttached = FALSE;

    while(!debuggerAttached)
    {
      CheckRemoteDebuggerPresent(hProcess, &debuggerAttached);

      Sleep(10);
      timeout += 10;

      if(timeout > opts.delayForDebugger * 1000)
        break;
    }

    if(debuggerAttached)
      RDCDEBUG("Debugger attach detected after %.2f s", float(timeout) / 1000.0f);
    else
      RDCDEBUG("Timed out waiting for debugger, gave up after %u s", opts.delayForDebugger);
  }

  RDCLOG("Injecting renderdoc into process %lu", pid);

  wchar_t renderdocPath[MAX_PATH] = {0};
  GetModuleFileNameW(GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll"), &renderdocPath[0],
                                      MAX_PATH - 1);

  wchar_t renderdocPathLower[MAX_PATH] = {0};
  memcpy(renderdocPathLower, renderdocPath, MAX_PATH * sizeof(wchar_t));
  for(size_t i = 0; i < MAX_PATH && renderdocPathLower[i]; i++)
  {
    // lowercase
    if(renderdocPathLower[i] >= 'A' && renderdocPathLower[i] <= 'Z')
      renderdocPathLower[i] = 'a' + char(renderdocPathLower[i] - 'A');

    // normalise paths
    if(renderdocPathLower[i] == '/')
      renderdocPathLower[i] = '\\';
  }

  BOOL isWow64 = FALSE;
  BOOL success = IsWow64Process(hProcess, &isWow64);

  if(!success)
  {
    DWORD err = GetLastError();
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                     "Couldn't determine bitness of process, err: %08x", err);
    CloseHandle(hProcess);
    return {result, 0};
  }

  bool capalt = false;

#if DISABLED(RDOC_X64)
  BOOL selfWow64 = FALSE;

  HANDLE hSelfProcess = GetCurrentProcess();

  // check to see if we're a WoW64 process
  success = IsWow64Process(hSelfProcess, &selfWow64);

  CloseHandle(hSelfProcess);

  if(!success)
  {
    DWORD err = GetLastError();
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                     "Couldn't determine bitness of self, err: %08x", err);
    CloseHandle(hProcess);
    return {result, 0};
  }

  // we know we're 32-bit, so if the target process is not wow64
  // and we are, it's 64-bit. If we're both not wow64 then we're
  // running on 32-bit windows, and if we're both wow64 then we're
  // both 32-bit on 64-bit windows.
  //
  // We don't support capturing 64-bit programs from a 32-bit install
  // because it's pointless - a 64-bit install will work for all in
  // that case. But we do want to handle the case of:
  // 64-bit renderdoc -> 32-bit program (via 32-bit renderdoccmd)
  //    -> 64-bit program (going back to 64-bit renderdoccmd).
  // so we try to see if we're an x86 invoked renderdoccmd in an
  // otherwise 64-bit install, and 'promote' back to 64-bit.
  if(selfWow64 && !isWow64)
  {
    wchar_t *slash = wcsrchr(renderdocPath, L'\\');

    if(slash && slash > renderdocPath + 4)
    {
      slash -= 4;

      if(slash && !wcsncmp(slash, L"\\x86", 4))
      {
        RDCDEBUG("Promoting back to 64-bit");
        capalt = true;
      }
    }

    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    if(!capalt)
    {
      const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\win32\\development\\");
      if(!devLocation)
        devLocation = wcsstr(renderdocPathLower, L"\\win32\\release\\");

      if(devLocation)
      {
        RDCDEBUG("Promoting back to 64-bit");
        capalt = true;
      }
    }

    // if we couldn't promote, then bail out.
    if(!capalt)
    {
      RDCDEBUG("Running from %ls", renderdocPathLower);

      CloseHandle(hProcess);
      RDResult result;
      SET_ERROR_RESULT(result, ResultCode::IncompatibleProcess,
                       "Can't capture 64-bit program with 32-bit build of RenderDoc. Please run a "
                       "64-bit build of RenderDoc");
      return {result, 0};
    }
  }
#else
  // farm off to alternate bitness renderdoccmd.exe

  // if the target process is 'wow64' that means it's 32-bit.
  capalt = (isWow64 == TRUE);
#endif

  if(capalt)
  {
#if ENABLED(RDOC_X64)
    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\x64\\development\\");
    if(devLocation)
    {
      size_t idx = devLocation - renderdocPathLower;

      renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\Win32\\Development\\rendertestcmd.exe");
    }

    if(!devLocation)
    {
      devLocation = wcsstr(renderdocPathLower, L"\\x64\\release\\");

      if(devLocation)
      {
        size_t idx = devLocation - renderdocPathLower;

        renderdocPath[idx] = 0;

        wcscat_s(renderdocPath, L"\\Win32\\Release\\rendertestcmd.exe");
      }
    }

    if(!devLocation)
    {
      // look in a subfolder for x86.

      // remove the filename from the path
      wchar_t *slash = wcsrchr(renderdocPath, L'\\');

      if(slash)
        *slash = 0;

      // append path
      wcscat_s(renderdocPath, L"\\x86\\rendertestcmd.exe");
    }
#else
    // if it looks like we're in the development environment, look for the alternate bitness in the
    // corresponding folder
    const wchar_t *devLocation = wcsstr(renderdocPathLower, L"\\win32\\development\\");
    if(devLocation)
    {
      size_t idx = devLocation - renderdocPathLower;

      renderdocPath[idx] = 0;

      wcscat_s(renderdocPath, L"\\x64\\Development\\rendertestcmd.exe");
    }

    if(!devLocation)
    {
      devLocation = wcsstr(renderdocPathLower, L"\\win32\\release\\");

      if(devLocation)
      {
        size_t idx = devLocation - renderdocPathLower;

        renderdocPath[idx] = 0;

        wcscat_s(renderdocPath, L"\\x64\\Release\\rendertestcmd.exe");
      }
    }

    if(!devLocation)
    {
      // look upwards on 32-bit to find the parent renderdoccmd.
      wchar_t *slash = wcsrchr(renderdocPath, L'\\');

      // remove the filename
      if(slash)
        *slash = 0;

      // remove the \\x86
      slash = wcsrchr(renderdocPath, L'\\');

      if(slash)
        *slash = 0;

      // append path
      wcscat_s(renderdocPath, L"\\rendertestcmd.exe");
    }
#endif

    PROCESS_INFORMATION pi;
    STARTUPINFO si;
    SECURITY_ATTRIBUTES pSec;
    SECURITY_ATTRIBUTES tSec;

    RDCEraseEl(pi);
    RDCEraseEl(si);
    RDCEraseEl(pSec);
    RDCEraseEl(tSec);

    // hide the console window
    si.cb = sizeof(si);
    si.dwFlags |= STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    pSec.nLength = sizeof(pSec);
    tSec.nLength = sizeof(tSec);

    // serialise to string with two chars per byte
    rdcstr optstr = opts.EncodeAsString();

    wchar_t *paramsAlloc = new wchar_t[2048];

    rdcstr debugLogfile = RDCGETLOGFILE();
    rdcwstr wdebugLogfile = StringFormat::UTF82Wide(debugLogfile);

    _snwprintf_s(
        paramsAlloc, 2047, 2047,
        L"\"%ls\" capaltbit --pid=%u --capfile=\"%ls\" --debuglog=\"%ls\" --capopts=\"%hs\"",
        renderdocPath, pid, wcapturefile.c_str(), wdebugLogfile.c_str(), optstr.c_str());

    RDCDEBUG("params %ls", paramsAlloc);

    paramsAlloc[2047] = 0;

    wchar_t *commandLine = paramsAlloc;

    std::wstring cmdWithEnv;

    if(!env.empty())
    {
      cmdWithEnv = paramsAlloc;

      for(const EnvironmentModification &e : env)
      {
        rdcstr name = e.name.trimmed();
        rdcstr value = e.value;

        if(name == "")
          break;

        cmdWithEnv += L" +env-";
        switch(e.mod)
        {
          case EnvMod::Set: cmdWithEnv += L"replace"; break;
          case EnvMod::Append: cmdWithEnv += L"append"; break;
          case EnvMod::Prepend: cmdWithEnv += L"prepend"; break;
        }

        if(e.mod != EnvMod::Set)
        {
          switch(e.sep)
          {
            case EnvSep::Platform: cmdWithEnv += L"-platform"; break;
            case EnvSep::SemiColon: cmdWithEnv += L"-semicolon"; break;
            case EnvSep::Colon: cmdWithEnv += L"-colon"; break;
            case EnvSep::NoSep: break;
          }
        }

        cmdWithEnv += L" ";

        // escape the parameters
        for(size_t it = 0; it < name.size(); it++)
        {
          if(name[it] == '"')
          {
            name.insert(it, '\\');
            it++;
          }
        }

        for(size_t it = 0; it < value.size(); it++)
        {
          if(value[it] == '"')
          {
            value.insert(it, '\\');
            it++;
          }
        }

        if(name.back() == '\\')
          name += "\\";

        if(value.back() == '\\')
          value += "\\";

        cmdWithEnv += L"\"" + std::wstring(StringFormat::UTF82Wide(name).c_str()) + L"\" ";
        cmdWithEnv += L"\"" + std::wstring(StringFormat::UTF82Wide(value).c_str()) + L"\" ";
      }

      commandLine = (wchar_t *)cmdWithEnv.c_str();
    }

    BOOL retValue = CreateProcessW(NULL, commandLine, &pSec, &tSec, false,
                                   CREATE_NEW_CONSOLE | CREATE_SUSPENDED, NULL, NULL, &si, &pi);

    SAFE_DELETE_ARRAY(paramsAlloc);

    if(!retValue)
    {
      RDResult result;
#if RENDERDOC_OFFICIAL_BUILD
      SET_ERROR_RESULT(result, ResultCode::InternalError,
                       "Can't run 32-bit renderdoccmd to capture 32-bit program.");
#else
      SET_ERROR_RESULT(
          result, ResultCode::InternalError,
          "Can't run 32-bit renderdoccmd to capture 32-bit program."
          "If this is a locally built RenderDoc you must build both 32-bit and 64-bit versions.");
#endif
      CloseHandle(hProcess);
      return {result, 0};
    }

    ResumeThread(pi.hThread);
    WaitForSingleObject(pi.hThread, INFINITE);
    CloseHandle(pi.hThread);

    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    if(waitForExit)
      WaitForSingleObject(hProcess, INFINITE);

    CloseHandle(hProcess);

    if(exitCode == 0)
    {
      RDResult result;
      SET_ERROR_RESULT(result, ResultCode::UnknownError,
                       "Encountered error while launching target 32-bit program.");
      return {result, 0};
    }

    if(exitCode < RenderDoc_FirstTargetControlPort)
    {
      ResultCode code = (ResultCode)exitCode;

      RDResult result;
      SET_ERROR_RESULT(result, code, "32-bit renderdoccmd returned '%s'", ToStr(code).c_str());
      return {code, 0};
    }

    return {ResultCode::Succeeded, (uint32_t)exitCode};
  }

  // If the target process was freshly created and is still suspended (the
  // normal case when launching a program from RenderDoc), hijack its main
  // thread to perform the injection via SetThreadContext instead of
  // CreateRemoteThread, which is a well-known injection vector commonly
  // detected and blocked by anti-cheat or crash reporting systems.
  HANDLE hMainThread = GetSuspendedMainThread(pid);
  HijackState hijackState = {};

  RDCLOG("Process %lu: main thread is %s", pid,
         hMainThread != NULL ? "suspended - hijack injection will be used" :
                               "not suspended - CreateRemoteThread injection will be used");

  if(hMainThread != NULL)
  {
    if(!InjectDLLThreadContext(hProcess, hMainThread, hijackState, renderdocPath))
    {
      // hijacking failed - restore the thread to a safe suspended state and
      // fall back to the classic CreateRemoteThread injection
      RDCLOG("Process %lu: hijack injection FAILED, falling back to CreateRemoteThread", pid);
      RestoreHijackedThread(hMainThread, hijackState);
      CloseHandle(hMainThread);
      hMainThread = NULL;
    }
  }

  if(hMainThread == NULL)
    InjectDLL(hProcess, renderdocPath);

  const char *rdoc_dll = STRINGIZE(RDOC_BASE_NAME);

  uintptr_t loc = FindRemoteDLL(pid, STRINGIZE(RDOC_BASE_NAME) ".dll");

  RDCLOG("Process %lu: %s.dll %s", pid, rdoc_dll,
         loc != 0 ? StringFormat::Fmt("loaded at 0x%llx", (unsigned long long)loc).c_str() :
                    "was NOT found in the process");

  auto RunInjectedCall = [&](const char *funcName, void *data, size_t dataLen) {
    if(hMainThread != NULL)
      InjectFunctionCallThreadContext(hProcess, hMainThread, hijackState, loc, funcName, data,
                                      dataLen);
    else
      InjectFunctionCall(hProcess, loc, funcName, data, dataLen);
  };

  rdcpair<RDResult, uint32_t> result = {ResultCode::Succeeded, 0};

  if(loc == 0)
  {
    SET_ERROR_RESULT(
        result.first, ResultCode::InjectionFailed,
        "Failed to inject %s.dll into process. Check that the process did not crash or exit "
        "early in initialisation, e.g. if the working directory is incorrectly set.",
        rdoc_dll);
  }
  else
  {
    // safe to cast away the const as we know these functions don't modify the parameters

    if(!capturefile.empty())
      RunInjectedCall("INTERNAL_SetCaptureFile", (void *)capturefile.c_str(),
                         capturefile.size() + 1);

    rdcstr debugLogfile = RDCGETLOGFILE();

    RunInjectedCall("INTERNAL_SetDebugLogFile", (void *)debugLogfile.c_str(),
                       debugLogfile.size() + 1);

    RunInjectedCall("INTERNAL_SetCaptureOptions", (CaptureOptions *)&opts,
                       sizeof(CaptureOptions));

    RunInjectedCall("INTERNAL_GetTargetControlIdent", &result.second,
                       sizeof(result.second));

    if(!env.empty())
    {
      for(const EnvironmentModification &e : env)
      {
        rdcstr name = e.name.trimmed();
        rdcstr value = e.value;
        EnvMod mod = e.mod;
        EnvSep sep = e.sep;

        if(name == "")
          break;

        RunInjectedCall("INTERNAL_EnvModName", (void *)name.c_str(),
                           name.size() + 1);
        RunInjectedCall("INTERNAL_EnvModValue", (void *)value.c_str(),
                           value.size() + 1);
        RunInjectedCall("INTERNAL_EnvSep", &sep, sizeof(sep));
        RunInjectedCall("INTERNAL_EnvMod", &mod, sizeof(mod));
      }

      // parameter is unused
      void *dummy = NULL;
      RunInjectedCall("INTERNAL_ApplyEnvMods", &dummy, sizeof(dummy));
    }
  }

  if(hMainThread != NULL)
  {
    // restore the original thread context. The thread remains suspended here -
    // it is resumed by the caller (e.g. LaunchAndInjectIntoProcess) as normal.
    RestoreHijackedThread(hMainThread, hijackState);
    CloseHandle(hMainThread);
  }

  if(waitForExit)
    WaitForSingleObject(hProcess, INFINITE);

  CloseHandle(hProcess);

  return result;
}

uint32_t Process::LaunchProcess(const rdcstr &app, const rdcstr &workingDir, const rdcstr &cmdLine,
                                bool internal, ProcessResult *result)
{
  HANDLE hChildStdOutput_Rd = NULL, hChildStdError_Rd = NULL;

  rdcstr appPath = app;
  size_t len = appPath.length();
  rdcstr ext;
  if(len > 4)
    ext = strlower(appPath.substr(len - 4));
  if(ext != ".exe")
    appPath += ".exe";

  PROCESS_INFORMATION pi =
      RunProcess(appPath, workingDir, cmdLine, {}, internal, result ? &hChildStdOutput_Rd : NULL,
                 result ? &hChildStdError_Rd : NULL);

  if(pi.dwProcessId == 0)
  {
    if(!internal)
      RDCWARN("Couldn't launch process '%s'", appPath.c_str());

    if(hChildStdError_Rd != NULL)
      CloseHandle(hChildStdError_Rd);
    if(hChildStdOutput_Rd != NULL)
      CloseHandle(hChildStdOutput_Rd);

    return 0;
  }

  if(!internal)
    RDCLOG("Launched process '%s' with '%s'", appPath.c_str(), cmdLine.c_str());

  ResumeThread(pi.hThread);

  if(result)
  {
    result->strStdout = "";
    result->strStderror = "";

    char chBuf[4096];
    DWORD dwOutputRead, dwErrorRead;
    BOOL success = FALSE;
    rdcstr s;
    for(;;)
    {
      success = ReadFile(hChildStdOutput_Rd, chBuf, sizeof(chBuf), &dwOutputRead, NULL);
      s = rdcstr(chBuf, dwOutputRead);
      result->strStdout += s;

      if(!success && !dwOutputRead)
        break;
    }

    for(;;)
    {
      success = ReadFile(hChildStdError_Rd, chBuf, sizeof(chBuf), &dwErrorRead, NULL);
      s = rdcstr(chBuf, dwErrorRead);
      result->strStderror += s;

      if(!success && !dwErrorRead)
        break;
    }

    CloseHandle(hChildStdOutput_Rd);
    CloseHandle(hChildStdError_Rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, (LPDWORD)&result->retCode);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  return pi.dwProcessId;
}

uint32_t Process::LaunchScript(const rdcstr &script, const rdcstr &workingDir,
                               const rdcstr &argList, bool internal, ProcessResult *result)
{
  // Change parameters to invoke command interpreter
  rdcstr args = "/C " + script + " " + argList;

  return LaunchProcess("cmd.exe", workingDir, args, internal, result);
}

rdcpair<RDResult, uint32_t> Process::LaunchAndInjectIntoProcess(
    const rdcstr &app, const rdcstr &workingDir, const rdcstr &cmdLine,
    const rdcarray<EnvironmentModification> &env, const rdcstr &capturefile,
    const CaptureOptions &opts, bool waitForExit)
{
  void *func =
      GetProcAddress(GetModuleHandleA(STRINGIZE(RDOC_BASE_NAME) ".dll"), "INTERNAL_SetCaptureFile");

  if(func == NULL)
  {
    const char *rdoc_dll = STRINGIZE(RDOC_BASE_NAME);
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::InternalError,
                     "Can't find required export function in %s.dll - corrupted/missing file?",
                     rdoc_dll);
    return {result, 0};
  }

  if(get_basename(app) == "explorer.exe" || get_basename(app) == "dllhost.exe")
  {
    RDResult result;
    SET_ERROR_RESULT(
        result, ResultCode::InjectionFailed,
        "For safety reasons RenderDoc does not support capturing executables with a "
        "reserved system filename such as '%s'. Please rename your executable to capture.",
        get_basename(app).c_str());
    return {result, 0};
  }

  PROCESS_INFORMATION pi = RunProcess(app, workingDir, cmdLine, env, false, NULL, NULL);

  if(pi.dwProcessId == 0)
  {
    RDResult result;
    SET_ERROR_RESULT(result, ResultCode::InjectionFailed, "Failed to launch process.");
    return {result, 0};
  }

  rdcpair<RDResult, uint32_t> ret = InjectIntoProcess(pi.dwProcessId, {}, capturefile, opts, false);

  CloseHandle(pi.hProcess);
  ResumeThread(pi.hThread);
  ResumeThread(pi.hThread);

  if(ret.second == 0 || ret.first != ResultCode::Succeeded)
  {
    CloseHandle(pi.hThread);
    return ret;
  }

  if(waitForExit)
    WaitForSingleObject(pi.hThread, INFINITE);

  CloseHandle(pi.hThread);

  return ret;
}

bool Process::CanGlobalHook()
{
  // all we need is admin rights and it's the caller's responsibility to ensure that.
  return true;
}

// to simplify the below code, rather than splitting by 32-bit/64-bit we split by native and Wow32.
// This means that for 32-bit code (whether it's on 32-bit OS or not) we just have native, and the
// Wow32 stuff is empty/unused. For 64-bit we use both. Thus the native registry key is always the
// same path regardless of the bitness we're running as and we don't have to move things around or
// have conditionals all over

struct GlobalHookData
{
  struct
  {
    HANDLE pipe = NULL;
    DWORD appinitEnabled = 0;
    rdcwstr appinitDLLs;
  } dataNative, dataWow32;

  int32_t finished = 0;
  Threading::ThreadHandle pipeThread = 0;
};

// utility function to close the registry keys, print an error, and quit
static RDResult HandleRegError(HKEY keyNative, HKEY keyWow32, LSTATUS ret, const char *msg)
{
  if(keyNative)
    RegCloseKey(keyNative);

  if(keyWow32)
    RegCloseKey(keyWow32);

  RDCLOG("Error with AppInit registry keys - %s (%d)", msg, ret);

  RETURN_ERROR_RESULT(ResultCode::InjectionFailed,
                      "Error updating registry to enable global hook.\n"
                      "Check that RenderDoc is correctly running as administrator.");
}

#define REG_CHECK(msg)                                    \
  if(ret != ERROR_SUCCESS)                                \
  {                                                       \
    return HandleRegError(keyNative, keyWow32, ret, msg); \
  }

// function to backup the previous settings for AppInit, then enable it and write our own paths.
RDResult BackupAndChangeRegistry(GlobalHookData &hookdata, const rdcstr &shimpathWow32,
                                 const rdcstr &shimpathNative)
{
  HKEY keyNative = NULL;
  HKEY keyWow32 = NULL;

  // AppInit_DLLs requires short paths, but short paths can be disabled globally or on a per-volume
  // level. If short paths are disabled we'll get the long path back, we *always* expect the path to
  // get shorter because the shim filename is bigger than 8.3.

  DWORD nativeShortSize = GetShortPathNameW(StringFormat::UTF82Wide(shimpathNative).c_str(), NULL,
                                            (DWORD)shimpathNative.length());
  if(nativeShortSize == (DWORD)shimpathNative.length() + 1)
  {
    RETURN_ERROR_RESULT(
        ResultCode::FileIOFailed,
        "RenderDoc is installed on a volume or system that has short paths disabled.\n"
        "For the global hook, short paths must be enabled where RenderDoc is installed.");
  }

  if(!shimpathWow32.empty())
  {
    DWORD wow32ShortSize = GetShortPathNameW(StringFormat::UTF82Wide(shimpathWow32).c_str(), NULL,
                                             (DWORD)shimpathWow32.length());

    if(wow32ShortSize == (DWORD)shimpathWow32.length() + 1)
    {
      RETURN_ERROR_RESULT(
          ResultCode::FileIOFailed,
          "RenderDoc is installed on a volume or system that has short paths disabled.\n"
          "For the global hook, short paths must be enabled where RenderDoc is installed.");
    }
  }

  // open the native key
  LSTATUS ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, NULL,
                                0, KEY_READ | KEY_WRITE, NULL, &keyNative, NULL);

  REG_CHECK("Could not open AppInit key");

  // if we are doing Wow32, open that key as well
  if(!shimpathWow32.empty())
  {
    ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                          "SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows",
                          0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &keyWow32, NULL);

    REG_CHECK("Could not open AppInit key");
  }

  const DWORD one = 1;

  // fetch the previous data for LoadAppInit_DLLs and AppInit_DLLs
  DWORD sz = 4;
  ret = RegGetValueA(keyNative, NULL, "LoadAppInit_DLLs", RRF_RT_REG_DWORD, NULL,
                     (void *)&hookdata.dataNative.appinitEnabled, &sz);
  REG_CHECK("Could not fetch LoadAppInit_DLLs");

  sz = 0;
  ret = RegGetValueW(keyNative, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL, NULL, &sz);
  if(ret == ERROR_MORE_DATA || ret == ERROR_SUCCESS)
  {
    hookdata.dataNative.appinitDLLs = rdcwstr(sz / sizeof(wchar_t));
    ret = RegGetValueW(keyNative, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL,
                       hookdata.dataNative.appinitDLLs.data(), &sz);
  }
  REG_CHECK("Could not fetch AppInit_DLLs");

  // set DWORD:1 for LoadAppInit_DLLs and convert our path to a short path then set it
  ret = RegSetValueExA(keyNative, "LoadAppInit_DLLs", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
  REG_CHECK("Could not set LoadAppInit_DLLs");

  rdcwstr shortpath(shimpathNative.size());
  GetShortPathNameW(StringFormat::UTF82Wide(shimpathNative).c_str(), shortpath.data(),
                    (DWORD)shortpath.length());

  ret = RegSetValueExW(keyNative, L"AppInit_DLLs", 0, REG_SZ, (const BYTE *)shortpath.data(),
                       DWORD(shortpath.length() * sizeof(wchar_t)));
  REG_CHECK("Could not set AppInit_DLLs");

  // if we're doing Wow32, repeat the process for those keys
  if(keyWow32)
  {
    sz = 4;
    ret = RegGetValueA(keyWow32, NULL, "LoadAppInit_DLLs", RRF_RT_REG_DWORD, NULL,
                       (void *)&hookdata.dataWow32.appinitEnabled, &sz);
    REG_CHECK("Could not fetch LoadAppInit_DLLs");

    sz = 0;
    ret = RegGetValueW(keyWow32, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL, NULL, &sz);
    if(ret == ERROR_MORE_DATA || ret == ERROR_SUCCESS)
    {
      hookdata.dataWow32.appinitDLLs = rdcwstr(sz / sizeof(wchar_t));
      ret = RegGetValueW(keyWow32, NULL, L"AppInit_DLLs", RRF_RT_ANY, NULL,
                         hookdata.dataWow32.appinitDLLs.data(), &sz);
    }
    REG_CHECK("Could not fetch AppInit_DLLs");

    ret = RegSetValueExA(keyWow32, "LoadAppInit_DLLs", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
    REG_CHECK("Could not set LoadAppInit_DLLs");

    shortpath = rdcwstr(shimpathWow32.size());
    GetShortPathNameW(StringFormat::UTF82Wide(shimpathWow32).c_str(), shortpath.data(),
                      (DWORD)shortpath.length());

    ret = RegSetValueExW(keyWow32, L"AppInit_DLLs", 0, REG_SZ, (const BYTE *)shortpath.data(),
                         DWORD(shortpath.length() * sizeof(wchar_t)));
    REG_CHECK("Could not set AppInit_DLLs");
  }

  std::wstring backup;

  // write a .reg file that contains the previous settings, so that if all else fails the user can
  // manually insert it back into the registry to restore everything.
  backup += L"Windows Registry Editor Version 5.00\n";
  backup += L"\n";
  backup += L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows]\n";
  backup += L"\"LoadAppInit_DLLs\"=dword:0000000";
  backup += (hookdata.dataNative.appinitEnabled ? L"1\n" : L"0\n");
  backup += L"\"AppInit_DLLs\"=\"";
  // we append with the C string so we don't add trailing NULLs into the text.
  backup += hookdata.dataNative.appinitDLLs.c_str();
  backup += L"\"\n";
  if(keyWow32)
  {
    backup += L"\n";
    backup +=
        L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Wow6432Node\\Microsoft\\"
        L"Windows NT\\CurrentVersion\\Windows]\n";
    backup += L"\"LoadAppInit_DLLs\"=dword:0000000";
    backup += (hookdata.dataWow32.appinitEnabled ? L"1\n" : L"0\n");
    backup += L"\"AppInit_DLLs\"=\"";
    backup += hookdata.dataWow32.appinitDLLs.c_str();
    backup += L"\"\n";
  }

  if(keyNative)
    RegCloseKey(keyNative);

  if(keyWow32)
    RegCloseKey(keyWow32);

  keyNative = keyWow32 = NULL;

  // write it to disk but don't fail if we can't, just print it to the log and keep going.
  wchar_t reg_backup[MAX_PATH];
  GetTempPathW(MAX_PATH, reg_backup);
  wcscat_s(reg_backup, L"RenderDoc_RestoreGlobalHook.reg");

  FILE *f = NULL;
  _wfopen_s(&f, reg_backup, L"w");
  if(f)
  {
    fputws(backup.c_str(), f);
    fclose(f);
  }
  else
  {
    RDCERR("Error opening registry backup file %ls", reg_backup);
    RDCERR("Backup registry data is:\n\n%ls\n\n", backup.c_str());
  }

  return RDResult();
}

// switch error-handling to print-and-continue, as we can't really do anything about it at this
// point and we want to continue restoring in case only one thing failed.
#undef REG_CHECK
#define REG_CHECK(msg)                                                      \
  if(ret != ERROR_SUCCESS)                                                  \
  {                                                                         \
    HandleRegError(keyNative, keyWow32, ret, "Could not open AppInit key"); \
  }

void RestoreRegistry(const GlobalHookData &hookdata)
{
  HKEY keyNative = NULL;
  HKEY keyWow32 = NULL;
  LSTATUS ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0, NULL,
                                0, KEY_READ | KEY_WRITE, NULL, &keyNative, NULL);

  REG_CHECK("Could not open AppInit key");

#if ENABLED(RDOC_X64)
  ret = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                        "SOFTWARE\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Windows", 0,
                        NULL, 0, KEY_READ | KEY_WRITE, NULL, &keyWow32, NULL);

  REG_CHECK("Could not open AppInit key");
#endif

  // set the native values back to where they were
  ret = RegSetValueExA(keyNative, "LoadAppInit_DLLs", 0, REG_DWORD,
                       (const BYTE *)&hookdata.dataNative.appinitEnabled,
                       sizeof(hookdata.dataNative.appinitEnabled));
  REG_CHECK("Could not set LoadAppInit_DLLs");

  ret = RegSetValueExW(keyNative, L"AppInit_DLLs", 0, REG_SZ,
                       (const BYTE *)hookdata.dataNative.appinitDLLs.c_str(),
                       DWORD(hookdata.dataNative.appinitDLLs.length() * sizeof(wchar_t)));
  REG_CHECK("Could not set AppInit_DLLs");

  // if we opened it, restore the Wow32 values as well
  if(keyWow32)
  {
    ret = RegSetValueExA(keyWow32, "LoadAppInit_DLLs", 0, REG_DWORD,
                         (const BYTE *)&hookdata.dataWow32.appinitEnabled,
                         sizeof(hookdata.dataWow32.appinitEnabled));
    REG_CHECK("Could not set LoadAppInit_DLLs");

    ret = RegSetValueExW(keyWow32, L"AppInit_DLLs", 0, REG_SZ,
                         (const BYTE *)hookdata.dataWow32.appinitDLLs.c_str(),
                         DWORD(hookdata.dataWow32.appinitDLLs.length() * sizeof(wchar_t)));
    REG_CHECK("Could not set AppInit_DLLs");
  }
}

static GlobalHookData *globalHook = NULL;

// a thread we run in the background just to keep the pipes open and wait until we're ready to stop
// the global hook.
static void GlobalHookThread()
{
  Threading::SetCurrentThreadName("GlobalHookThread");

  // keep looping doing an atomic compare-exchange to check that finished is still 0
  while(Atomic::CmpExch32(&globalHook->finished, 0, 0) == 0)
  {
    // wake every quarter of a second to test again
    Threading::Sleep(250);
  }

  char exitData[32] = "exit";

  // write some data into the pipe and close it. The data is (currently) unimportant, just that it
  // causes the blocking read on the other end to succeed and close the program.
  DWORD dummy = 0;
  if(globalHook->dataNative.pipe)
  {
    WriteFile(globalHook->dataNative.pipe, exitData, (DWORD)sizeof(exitData), &dummy, NULL);
    CloseHandle(globalHook->dataNative.pipe);
  }

  if(globalHook->dataWow32.pipe)
  {
    WriteFile(globalHook->dataWow32.pipe, exitData, (DWORD)sizeof(exitData), &dummy, NULL);
    CloseHandle(globalHook->dataWow32.pipe);
  }
}

RDResult Process::StartGlobalHook(const rdcstr &pathmatch, const rdcstr &capturefile,
                                  const CaptureOptions &opts)
{
  if(pathmatch.empty())
  {
    RETURN_ERROR_RESULT(ResultCode::InvalidParameter,
                        "Invalid global hook parameter, empty path to match");
  }

  rdcstr renderdocPath;
  FileIO::GetLibraryFilename(renderdocPath);

  renderdocPath = get_dirname(renderdocPath);

  // the native renderdoccmd.exe is always next to the dll. Wow32 will be somewhere else
  rdcstr cmdpathNative = renderdocPath + "\\rendertestcmd.exe";
  rdcstr cmdpathWow32;

  rdcstr shimpathNative = renderdocPath;
  rdcstr shimpathWow32;

#if ENABLED(RDOC_X64)

  // native shim is just rendertestshim64.dll
  shimpathNative = renderdocPath + "\\rendertestshim64.dll";

  // if it looks like we're in the development environment, look for the alternate bitness in the
  // corresponding folder
  int devLocation = renderdocPath.find("\\x64\\Development");
  if(devLocation >= 0)
  {
    renderdocPath.erase(devLocation, ~0U);

    shimpathWow32 = renderdocPath + "\\Win32\\Development\\rendertestshim32.dll";
    cmdpathWow32 = renderdocPath + "\\Win32\\Development\\rendertestcmd.exe";
  }
  else
  {
    devLocation = renderdocPath.find("\\x64\\Release");

    if(devLocation >= 0)
    {
      renderdocPath.erase(devLocation, ~0U);

      shimpathWow32 = renderdocPath + "\\Win32\\Release\\rendertestshim32.dll";
      cmdpathWow32 = renderdocPath + "\\Win32\\Release\\rendertestcmd.exe";
    }
  }

  // if we're not in the dev environment, assume it's under a x86\ subfolder
  if(devLocation < 0)
  {
    shimpathWow32 = renderdocPath + "\\x86\\rendertestshim32.dll";
    cmdpathWow32 = renderdocPath + "\\x86\\rendertestcmd.exe";
  }

#else

  // nothing fancy to do here for 32-bit, just point the shim next to our dll.
  shimpathNative = renderdocPath + "\\rendertestshim32.dll";

#endif

  GlobalHookData hookdata;

  // try to backup and change the registry settings to start loading our shim dlls. If that fails,
  // we bail out immediately
  RDResult regStatus = BackupAndChangeRegistry(hookdata, shimpathWow32, shimpathNative);
  if(regStatus != ResultCode::Succeeded)
    return regStatus;

  PROCESS_INFORMATION pi = {0};
  STARTUPINFO si = {0};
  SECURITY_ATTRIBUTES pSec = {0};
  SECURITY_ATTRIBUTES tSec = {0};
  pSec.nLength = sizeof(pSec);
  tSec.nLength = sizeof(tSec);

  si.cb = sizeof(si);

  // serialise to string with two chars per byte
  rdcstr optstr = opts.EncodeAsString();
  rdcstr debugLogfile = RDCGETLOGFILE();

  rdcstr params = StringFormat::Fmt(
      "\"%s\" globalhook --match \"%s\" --capfile \"%s\" --debuglog \"%s\" --capopts \"%s\"",
      cmdpathNative.c_str(), pathmatch.c_str(), capturefile.c_str(), debugLogfile.c_str(),
      optstr.c_str());

  rdcwstr paramsAlloc = StringFormat::UTF82Wide(params);

  // we'll be setting stdin
  si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;

  // hide the console window
  si.wShowWindow = SW_HIDE;

  // this is the end of the pipe that the child will inherit and use as stdin
  HANDLE childEnd = NULL;

  DWORD err;

  // create a pipe with the writing end for us, and the reading end as the child process's stdin
  {
    SECURITY_ATTRIBUTES pipeSec;
    pipeSec.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSec.bInheritHandle = TRUE;
    pipeSec.lpSecurityDescriptor = NULL;

    BOOL res;
    res = CreatePipe(&childEnd, &hookdata.dataNative.pipe, &pipeSec, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError, "Could not create 32-bit stdin pipe (err %u)",
                          err);
    }

    // we don't want the child process to inherit our end
    res = SetHandleInformation(hookdata.dataNative.pipe, HANDLE_FLAG_INHERIT, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError,
                          "Could not make 32-bit stdin pipe inheritable (err %u)", err);
    }

    si.hStdInput = childEnd;
  }

  // launch the process
  BOOL retValue = CreateProcessW(NULL, &paramsAlloc[0], &pSec, &tSec, true, CREATE_NEW_CONSOLE,
                                 NULL, NULL, &si, &pi);

  err = GetLastError();

  // we don't need this end anymore, the child has it
  CloseHandle(childEnd);

  if(retValue == FALSE)
  {
    CloseHandle(hookdata.dataNative.pipe);
    RestoreRegistry(hookdata);
    RETURN_ERROR_RESULT(ResultCode::InternalError, "Can't launch renderdoccmd from '%s' (err %u)",
                        cmdpathNative.c_str(), err);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);

  RDCEraseEl(pi);

// repeat the process for the Wow32 renderdoccmd
#if ENABLED(RDOC_X64)
  params = StringFormat::Fmt(
      "\"%s\" globalhook --match \"%s\" --capfile \"%s\" --debuglog \"%s\" --capopts \"%s\"",
      cmdpathWow32.c_str(), pathmatch.c_str(), capturefile.c_str(), debugLogfile.c_str(),
      optstr.c_str());

  paramsAlloc = StringFormat::UTF82Wide(params);

  {
    SECURITY_ATTRIBUTES pipeSec;
    pipeSec.nLength = sizeof(SECURITY_ATTRIBUTES);
    pipeSec.bInheritHandle = TRUE;
    pipeSec.lpSecurityDescriptor = NULL;

    BOOL res;
    res = CreatePipe(&childEnd, &hookdata.dataWow32.pipe, &pipeSec, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError, "Could not create 64-bit stdin pipe (err %u)",
                          err);
    }

    res = SetHandleInformation(hookdata.dataWow32.pipe, HANDLE_FLAG_INHERIT, 0);

    if(!res)
    {
      err = GetLastError();
      RestoreRegistry(hookdata);
      RETURN_ERROR_RESULT(ResultCode::InternalError,
                          "Could not make 64-bit stdin pipe inheritable (err %u)", err);
    }

    si.hStdInput = childEnd;
  }

  retValue = CreateProcessW(NULL, &paramsAlloc[0], &pSec, &tSec, true, CREATE_NEW_CONSOLE, NULL,
                            NULL, &si, &pi);

  err = GetLastError();

  // we don't need this end anymore
  CloseHandle(childEnd);

  if(retValue == FALSE)
  {
    CloseHandle(hookdata.dataNative.pipe);
    CloseHandle(hookdata.dataWow32.pipe);
    RestoreRegistry(hookdata);
    RETURN_ERROR_RESULT(ResultCode::InternalError, "Can't launch renderdoccmd from '%s' (err %u)",
                        cmdpathWow32.c_str(), err);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
#endif

  // set static global pointer with our data, and launch the thread
  globalHook = new GlobalHookData;
  *globalHook = hookdata;

  globalHook->pipeThread = Threading::CreateThread(&GlobalHookThread);

  return RDResult();
}

bool Process::IsGlobalHookActive()
{
  return globalHook != NULL;
}
void Process::StopGlobalHook()
{
  if(!globalHook)
    return;

  // set the finished flag and join to the thread so it closes the pipes (and so the child
  // processes)
  Atomic::Inc32(&globalHook->finished);

  Threading::JoinThread(globalHook->pipeThread);
  Threading::CloseThread(globalHook->pipeThread);

  // restore the registry settings from before we started
  RestoreRegistry(*globalHook);

  delete globalHook;
  globalHook = NULL;
}

bool Process::IsModuleLoaded(const rdcstr &module)
{
  return GetModuleHandleA(module.c_str()) != NULL;
}

void *Process::LoadModule(const rdcstr &module)
{
  HMODULE mod = GetModuleHandleA(module.c_str());
  if(mod != NULL)
    return mod;

  return LoadLibraryA(module.c_str());
}

void *Process::GetFunctionAddress(void *module, const rdcstr &function)
{
  if(module == NULL)
    return NULL;

  return (void *)GetProcAddress((HMODULE)module, function.c_str());
}

uint32_t Process::GetCurrentPID()
{
  return (uint32_t)GetCurrentProcessId();
}

void Process::Shutdown()
{
  // nothing to do
}
