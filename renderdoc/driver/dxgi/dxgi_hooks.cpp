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

#include "core/core.h"
#include "hooks/hooks.h"
#include "hooks/inline_hook.h"
#include "dxgi_wrapped.h"

typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY)(REFIID, void **);
typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY2)(UINT, REFIID, void **);
typedef HRESULT(WINAPI *PFN_GET_DEBUG_INTERFACE)(REFIID, void **);
typedef HRESULT(WINAPI *PFN_GET_DEBUG_INTERFACE1)(UINT, REFIID, void **);



MIDL_INTERFACE("9F251514-9D4D-4902-9D60-18988AB7D4B5")
IDXGraphicsAnalysis : public IUnknown
{
  virtual void STDMETHODCALLTYPE BeginCapture() = 0;
  virtual void STDMETHODCALLTYPE EndCapture() = 0;
};

struct RenderDocAnalysis : IDXGraphicsAnalysis
{
  // IUnknown boilerplate
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef()
  {
    InterlockedIncrement(&m_iRefcount);
    return m_iRefcount;
  }
  ULONG STDMETHODCALLTYPE Release() { return InterlockedDecrement(&m_iRefcount); }
  unsigned int m_iRefcount = 0;

  // IDXGraphicsAnalysis
  void STDMETHODCALLTYPE BeginCapture()
  {
    DeviceOwnedWindow devWnd;
    RenderDoc::Inst().GetActiveWindow(devWnd);

    RenderDoc::Inst().StartFrameCapture(devWnd);
  }

  void STDMETHODCALLTYPE EndCapture()
  {
    DeviceOwnedWindow devWnd;
    RenderDoc::Inst().GetActiveWindow(devWnd);

    RenderDoc::Inst().EndFrameCapture(devWnd);
  }
};

struct DummyDXGIInfoQueue : public IDXGIInfoQueue
{
public:
  // IUnknown boilerplate
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) { return E_NOINTERFACE; }
  ULONG STDMETHODCALLTYPE AddRef()
  {
    InterlockedIncrement(&m_iRefcount);
    return m_iRefcount;
  }
  ULONG STDMETHODCALLTYPE Release() { return InterlockedDecrement(&m_iRefcount); }
  unsigned int m_iRefcount = 0;
  // IDXGIInfoQueue
  virtual HRESULT STDMETHODCALLTYPE SetMessageCountLimit(DXGI_DEBUG_ID Producer,
                                                         UINT64 MessageCountLimit)
  {
    return S_OK;
  }

  virtual void STDMETHODCALLTYPE ClearStoredMessages(DXGI_DEBUG_ID Producer) { return; }
  virtual HRESULT STDMETHODCALLTYPE GetMessage(DXGI_DEBUG_ID Producer, UINT64 MessageIndex,
                                               _Out_writes_bytes_opt_(*pMessageByteLength)
                                                   DXGI_INFO_QUEUE_MESSAGE *pMessage,
                                               _Inout_ SIZE_T *pMessageByteLength)
  {
    return S_OK;
  }

  virtual UINT64 STDMETHODCALLTYPE GetNumStoredMessagesAllowedByRetrievalFilters(DXGI_DEBUG_ID Producer)
  {
    return 0;
  }

  virtual UINT64 STDMETHODCALLTYPE GetNumStoredMessages(DXGI_DEBUG_ID Producer) { return 0; }
  virtual UINT64 STDMETHODCALLTYPE GetNumMessagesDiscardedByMessageCountLimit(DXGI_DEBUG_ID Producer)
  {
    return 0;
  }

  virtual UINT64 STDMETHODCALLTYPE GetMessageCountLimit(DXGI_DEBUG_ID Producer) { return 0; }
  virtual UINT64 STDMETHODCALLTYPE GetNumMessagesAllowedByStorageFilter(DXGI_DEBUG_ID Producer)
  {
    return 0;
  }

  virtual UINT64 STDMETHODCALLTYPE GetNumMessagesDeniedByStorageFilter(DXGI_DEBUG_ID Producer)
  {
    return 0;
  }

  virtual HRESULT STDMETHODCALLTYPE AddStorageFilterEntries(DXGI_DEBUG_ID Producer,
                                                            DXGI_INFO_QUEUE_FILTER *pFilter)
  {
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE GetStorageFilter(DXGI_DEBUG_ID Producer,
                                                     _Out_writes_bytes_opt_(*pFilterByteLength)
                                                         DXGI_INFO_QUEUE_FILTER *pFilter,
                                                     _Inout_ SIZE_T *pFilterByteLength)
  {
    return S_OK;
  }

  virtual void STDMETHODCALLTYPE ClearStorageFilter(DXGI_DEBUG_ID Producer) { return; }
  virtual HRESULT STDMETHODCALLTYPE PushEmptyStorageFilter(DXGI_DEBUG_ID Producer) { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE PushDenyAllStorageFilter(DXGI_DEBUG_ID Producer)
  {
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE PushCopyOfStorageFilter(DXGI_DEBUG_ID Producer) { return S_OK; }
  virtual HRESULT STDMETHODCALLTYPE PushStorageFilter(DXGI_DEBUG_ID Producer,
                                                      DXGI_INFO_QUEUE_FILTER *pFilter)
  {
    return S_OK;
  }

  virtual void STDMETHODCALLTYPE PopStorageFilter(DXGI_DEBUG_ID Producer) { return; }
  virtual UINT STDMETHODCALLTYPE GetStorageFilterStackSize(DXGI_DEBUG_ID Producer) { return 0; }
  virtual HRESULT STDMETHODCALLTYPE AddRetrievalFilterEntries(DXGI_DEBUG_ID Producer,
                                                              DXGI_INFO_QUEUE_FILTER *pFilter)
  {
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE GetRetrievalFilter(DXGI_DEBUG_ID Producer,
                                                       _Out_writes_bytes_opt_(*pFilterByteLength)
                                                           DXGI_INFO_QUEUE_FILTER *pFilter,
                                                       _Inout_ SIZE_T *pFilterByteLength)
  {
    return S_OK;
  }

  virtual void STDMETHODCALLTYPE ClearRetrievalFilter(DXGI_DEBUG_ID Producer) { return; }
  virtual HRESULT STDMETHODCALLTYPE PushEmptyRetrievalFilter(DXGI_DEBUG_ID Producer)
  {
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE PushDenyAllRetrievalFilter(DXGI_DEBUG_ID Producer)
  {
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE PushCopyOfRetrievalFilter(DXGI_DEBUG_ID Producer)
  {
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE PushRetrievalFilter(DXGI_DEBUG_ID Producer,
                                                        DXGI_INFO_QUEUE_FILTER *pFilter)
  {
    return S_OK;
  }

  virtual void STDMETHODCALLTYPE PopRetrievalFilter(DXGI_DEBUG_ID Producer) { return; }
  virtual UINT STDMETHODCALLTYPE GetRetrievalFilterStackSize(DXGI_DEBUG_ID Producer) { return 0; }
  virtual HRESULT STDMETHODCALLTYPE AddMessage(DXGI_DEBUG_ID Producer,
                                               DXGI_INFO_QUEUE_MESSAGE_CATEGORY Category,
                                               DXGI_INFO_QUEUE_MESSAGE_SEVERITY Severity,
                                               DXGI_INFO_QUEUE_MESSAGE_ID ID, LPCSTR pDescription)
  {
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE AddApplicationMessage(DXGI_INFO_QUEUE_MESSAGE_SEVERITY Severity,
                                                          LPCSTR pDescription)
  {
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE SetBreakOnCategory(DXGI_DEBUG_ID Producer,
                                                       DXGI_INFO_QUEUE_MESSAGE_CATEGORY Category,
                                                       BOOL bEnable)
  {
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE SetBreakOnSeverity(DXGI_DEBUG_ID Producer,
                                                       DXGI_INFO_QUEUE_MESSAGE_SEVERITY Severity,
                                                       BOOL bEnable)
  {
    return S_OK;
  }

  virtual HRESULT STDMETHODCALLTYPE SetBreakOnID(DXGI_DEBUG_ID Producer,
                                                 DXGI_INFO_QUEUE_MESSAGE_ID ID, BOOL bEnable)
  {
    return S_OK;
  }

  virtual BOOL STDMETHODCALLTYPE GetBreakOnCategory(DXGI_DEBUG_ID Producer,
                                                    DXGI_INFO_QUEUE_MESSAGE_CATEGORY Category)
  {
    return FALSE;
  }

  virtual BOOL STDMETHODCALLTYPE GetBreakOnSeverity(DXGI_DEBUG_ID Producer,
                                                    DXGI_INFO_QUEUE_MESSAGE_SEVERITY Severity)
  {
    return FALSE;
  }

  virtual BOOL STDMETHODCALLTYPE GetBreakOnID(DXGI_DEBUG_ID Producer, DXGI_INFO_QUEUE_MESSAGE_ID ID)
  {
    return FALSE;
  }

  virtual void STDMETHODCALLTYPE SetMuteDebugOutput(DXGI_DEBUG_ID Producer, BOOL bMute) { return; }
  virtual BOOL STDMETHODCALLTYPE GetMuteDebugOutput(DXGI_DEBUG_ID Producer) { return FALSE; }
};

class DXGIHook : LibraryHook
{
public:
  void RegisterHooks()
  {
    RDCLOG("Registering DXGI hooks");

    LibraryHooks::RegisterLibraryHook("dxgi.dll", &DXGIHook::OnDxgiLoaded);

    CreateDXGIFactory.Register("dxgi.dll", "CreateDXGIFactory", CreateDXGIFactory_hook);
    CreateDXGIFactory1.Register("dxgi.dll", "CreateDXGIFactory1", CreateDXGIFactory1_hook);
    CreateDXGIFactory2.Register("dxgi.dll", "CreateDXGIFactory2", CreateDXGIFactory2_hook);
    GetDebugInterface.Register("dxgi.dll", "DXGIGetDebugInterface", DXGIGetDebugInterface_hook);
    GetDebugInterface1.Register("dxgi.dll", "DXGIGetDebugInterface1", DXGIGetDebugInterface1_hook);

    HMODULE dxgiModule = GetModuleHandleA("dxgi.dll");
    RDCLOG("DXGI diagnostics: hook functions registered, dxgi.dll currently %sloaded at %p",
           dxgiModule ? "" : "not ", dxgiModule);
  }

private:
  static DXGIHook dxgihooks;

  static bool InlineActive()
  {
#if defined(_WIN64)
    return s_InlineHooksInstalled;
#else
    return false;
#endif
  }

  // Some games (notably those with anti-cheat/anti-debug protection) resolve
  // CreateDXGIFactory1/2 via their own export-walking code and call the raw function pointer
  // directly, bypassing the IAT hooks. The game then gets a REAL IDXGIFactory while the D3D12
  // device/command-queue are wrapped, so CreateSwapChainForHwnd fails with E_NOINTERFACE when
  // handed a wrapped queue. Install inline hooks (x64 only, like the D3D12 hooks) so every
  // resolution path returns a wrapped factory. On x86 these pointers stay NULL and the IAT
  // hooks alone are used, identical to previous behaviour.
#if defined(_WIN64)
  static bool s_InlineHooksInstalled;
  static PFN_CREATE_DXGI_FACTORY s_TrampCreateFactory;
  static PFN_CREATE_DXGI_FACTORY s_TrampCreateFactory1;
  static PFN_CREATE_DXGI_FACTORY2 s_TrampCreateFactory2;

  static void OnDxgiLoaded(void *mod, const char *name)
  {
    if(s_InlineHooksInstalled)
      return;
    s_InlineHooksInstalled = true;

    void *pCreate = (void *)GetProcAddress((HMODULE)mod, "CreateDXGIFactory");
    void *pCreate1 = (void *)GetProcAddress((HMODULE)mod, "CreateDXGIFactory1");
    void *pCreate2 = (void *)GetProcAddress((HMODULE)mod, "CreateDXGIFactory2");

    if(pCreate)
      s_TrampCreateFactory =
          (PFN_CREATE_DXGI_FACTORY)InlineHook::Install(pCreate, (void *)&CreateDXGIFactory_inline);
    if(pCreate1)
      s_TrampCreateFactory1 =
          (PFN_CREATE_DXGI_FACTORY)InlineHook::Install(pCreate1, (void *)&CreateDXGIFactory1_inline);
    if(pCreate2)
      s_TrampCreateFactory2 = (PFN_CREATE_DXGI_FACTORY2)InlineHook::Install(
          pCreate2, (void *)&CreateDXGIFactory2_inline);

    RDCLOG("DXGI diagnostics: inline hooks on dxgi.dll - CreateFactory %p (tramp %p), "
           "CreateFactory1 %p (tramp %p), CreateFactory2 %p (tramp %p)",
           pCreate, (void *)s_TrampCreateFactory, pCreate1, (void *)s_TrampCreateFactory1, pCreate2,
           (void *)s_TrampCreateFactory2);
  }

  // The inline detours call the real dxgi.dll implementation via the saved trampoline, then wrap.
  // We must NOT route through the IAT-hooked function pointers (CreateDXGIFactory() etc) because
  // those resolve back to our IAT hook / this detour and would recurse or double-wrap. The IAT
  // hooks below skip their own wrapping whenever the inline hook is active.
  static HRESULT WINAPI CreateDXGIFactory_inline(REFIID riid, void **ppFactory)
  {
    if(ppFactory)
      *ppFactory = NULL;
    HRESULT ret = s_TrampCreateFactory(riid, ppFactory);
    if(SUCCEEDED(ret))
      RefCountDXGIObject::HandleWrap("CreateDXGIFactory", riid, ppFactory);
    return ret;
  }

  static HRESULT WINAPI CreateDXGIFactory1_inline(REFIID riid, void **ppFactory)
  {
    if(ppFactory)
      *ppFactory = NULL;
    HRESULT ret = s_TrampCreateFactory1(riid, ppFactory);
    if(SUCCEEDED(ret))
      RefCountDXGIObject::HandleWrap("CreateDXGIFactory1", riid, ppFactory);
    return ret;
  }

  static HRESULT WINAPI CreateDXGIFactory2_inline(UINT Flags, REFIID riid, void **ppFactory)
  {
    if(ppFactory)
      *ppFactory = NULL;
    HRESULT ret = s_TrampCreateFactory2(Flags, riid, ppFactory);
    if(SUCCEEDED(ret))
      RefCountDXGIObject::HandleWrap("CreateDXGIFactory2", riid, ppFactory);
    return ret;
  }
#else
  static void OnDxgiLoaded(void *mod, const char *name) {}
#endif

  RenderDocAnalysis m_RenderDocAnalysis;
  DummyDXGIInfoQueue m_DummyInfoQueue;

  HookedFunction<PFN_CREATE_DXGI_FACTORY> CreateDXGIFactory;
  HookedFunction<PFN_CREATE_DXGI_FACTORY> CreateDXGIFactory1;
  HookedFunction<PFN_CREATE_DXGI_FACTORY2> CreateDXGIFactory2;
  HookedFunction<PFN_GET_DEBUG_INTERFACE> GetDebugInterface;
  HookedFunction<PFN_GET_DEBUG_INTERFACE1> GetDebugInterface1;

  static HRESULT WINAPI CreateDXGIFactory_hook(__in REFIID riid, __out void **ppFactory)
  {
    RDCLOG("DXGI diagnostics: CreateDXGIFactory hook entered, interface %s", ToStr(riid).c_str());
    if(ppFactory)
      *ppFactory = NULL;
    HRESULT ret = dxgihooks.CreateDXGIFactory()(riid, ppFactory);

    // When the inline hook is active the call above went through the detour which already
    // wrapped the factory, so only wrap here on the IAT-only path.
    if(SUCCEEDED(ret) && !InlineActive())
      RefCountDXGIObject::HandleWrap("CreateDXGIFactory", riid, ppFactory);

    RDCLOG("DXGI diagnostics: CreateDXGIFactory returned %s, factory %p", ToStr(ret).c_str(),
           ppFactory ? *ppFactory : NULL);
    return ret;
  }

  static HRESULT WINAPI CreateDXGIFactory1_hook(__in REFIID riid, __out void **ppFactory)
  {
    RDCLOG("DXGI diagnostics: CreateDXGIFactory1 hook entered, interface %s", ToStr(riid).c_str());
    if(ppFactory)
      *ppFactory = NULL;
    HRESULT ret = dxgihooks.CreateDXGIFactory1()(riid, ppFactory);

    if(SUCCEEDED(ret) && !InlineActive())
      RefCountDXGIObject::HandleWrap("CreateDXGIFactory1", riid, ppFactory);

    RDCLOG("DXGI diagnostics: CreateDXGIFactory1 returned %s, factory %p", ToStr(ret).c_str(),
           ppFactory ? *ppFactory : NULL);
    return ret;
  }

  static HRESULT WINAPI CreateDXGIFactory2_hook(UINT Flags, REFIID riid, void **ppFactory)
  {
    RDCLOG("DXGI diagnostics: CreateDXGIFactory2 hook entered, flags %u, interface %s", Flags,
           ToStr(riid).c_str());
    if(ppFactory)
      *ppFactory = NULL;
    HRESULT ret = dxgihooks.CreateDXGIFactory2()(Flags, riid, ppFactory);

    if(SUCCEEDED(ret) && !InlineActive())
      RefCountDXGIObject::HandleWrap("CreateDXGIFactory2", riid, ppFactory);

    RDCLOG("DXGI diagnostics: CreateDXGIFactory2 returned %s, factory %p", ToStr(ret).c_str(),
           ppFactory ? *ppFactory : NULL);
    return ret;
  }

  static HRESULT WINAPI DXGIGetDebugInterface_hook(REFIID riid, void **ppDebug)
  {
    if(ppDebug)
      *ppDebug = NULL;

    if(riid == __uuidof(IDXGraphicsAnalysis))
    {
      dxgihooks.m_RenderDocAnalysis.AddRef();
      if(ppDebug)
        *ppDebug = &dxgihooks.m_RenderDocAnalysis;
      return S_OK;
    }
    if(riid == __uuidof(IDXGIInfoQueue))
    {
      RDCWARN(
          "Returning a dummy IDXGIInfoQueue that does nothing. RenderDoc takes control of the "
          "debug layer.");
      dxgihooks.m_DummyInfoQueue.AddRef();
      if(ppDebug)
        *ppDebug = &dxgihooks.m_DummyInfoQueue;
      return S_OK;
    }

    // IDXGIDebug and IDXGIDebug1 can come through here, but we don't need to wrap them.

    if(dxgihooks.GetDebugInterface())
      return dxgihooks.GetDebugInterface()(riid, ppDebug);
    else
      return E_NOINTERFACE;
  }

  static HRESULT WINAPI DXGIGetDebugInterface1_hook(UINT Flags, REFIID riid, void **ppDebug)
  {
    if(ppDebug)
      *ppDebug = NULL;

    if(riid == __uuidof(IDXGraphicsAnalysis))
    {
      dxgihooks.m_RenderDocAnalysis.AddRef();
      if(ppDebug)
        *ppDebug = &dxgihooks.m_RenderDocAnalysis;
      return S_OK;
    }
    if(riid == __uuidof(IDXGIInfoQueue))
    {
      RDCWARN(
          "Returning a dummy IDXGIInfoQueue that does nothing. RenderDoc takes control of the "
          "debug layer.");
      dxgihooks.m_DummyInfoQueue.AddRef();
      if(ppDebug)
        *ppDebug = &dxgihooks.m_DummyInfoQueue;
      return S_OK;
    }

    // IDXGIDebug and IDXGIDebug1 can come through here, but we don't need to wrap them.

    if(dxgihooks.GetDebugInterface1())
      return dxgihooks.GetDebugInterface1()(Flags, riid, ppDebug);
    else
      return E_NOINTERFACE;
  }
};

DXGIHook DXGIHook::dxgihooks;

#if defined(_WIN64)
bool DXGIHook::s_InlineHooksInstalled = false;
PFN_CREATE_DXGI_FACTORY DXGIHook::s_TrampCreateFactory = NULL;
PFN_CREATE_DXGI_FACTORY DXGIHook::s_TrampCreateFactory1 = NULL;
PFN_CREATE_DXGI_FACTORY2 DXGIHook::s_TrampCreateFactory2 = NULL;
#endif
