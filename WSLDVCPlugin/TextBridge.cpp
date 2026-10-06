// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.




//









#include "pch.h"
#include "TextBridge.h"
#include "utils.h"

#include <cstdio>
#include <cwchar>
#include <set>
#include <thread>
#include <imm.h>
#include <unknwn.h>
#include <msctf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.RemoteDesktop.Input.h>

#pragma comment(lib, "imm32.lib")

using namespace winrt::Windows::System::RemoteDesktop::Input;


void
BridgeLog(const wchar_t* format, ...);

namespace
{
    constexpr char c_c2sChannelName[] = "WSL::TextBridge::ClientToServer";
    constexpr char c_s2cChannelName[] = "WSL::TextBridge::ServerToClient";

    // ------------------------------------------------------------------











    // ------------------------------------------------------------------
    struct BridgeConfig
    {
        bool useTextStore = false;
        bool keyFeed = false;
        bool useSink = true;

        bool useCaret = true;

        bool useOwner = true;

        int geomFix = 0;
        int syncGeom = 0;
        bool verbose = false;
    };

    BridgeConfig g_cfg;

    void
    LoadConfig()
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, L"C:\\ProgramData\\wsltextbridge.cfg",
                      L"r, ccs=UTF-8") != 0 || !f)
        {
            return;
        }

        wchar_t line[256];
        while (fgetws(line, sizeof(line) / sizeof(line[0]), f))
        {
            std::wstring s(line);
            size_t hash = s.find(L'#');
            if (hash != std::wstring::npos)
            {
                s.erase(hash);
            }
            size_t eq = s.find(L'=');
            if (eq == std::wstring::npos)
            {
                continue;
            }
            std::wstring key = s.substr(0, eq);
            std::wstring val = s.substr(eq + 1);
            auto trim = [](std::wstring& x)
            {
                size_t b = x.find_first_not_of(L" \t\r\n");
                size_t e = x.find_last_not_of(L" \t\r\n");
                x = (b == std::wstring::npos) ? std::wstring() : x.substr(b, e - b + 1);
            };
            trim(key);
            trim(val);

            const bool on = (val == L"1" || val == L"true" || val == L"on");
            if (key == L"store")
            {
                g_cfg.useTextStore = on;
            }
            else if (key == L"keyfeed")
            {
                g_cfg.keyFeed = on;
            }
            else if (key == L"sink")
            {
                g_cfg.useSink = on;
            }
            else if (key == L"caret")
            {
                g_cfg.useCaret = on;
            }
            else if (key == L"owner")
            {
                g_cfg.useOwner = on;
            }
            else if (key == L"geomfix")
            {
                g_cfg.geomFix = _wtoi(val.c_str());
            }
            else if (key == L"syncgeom")
            {
                g_cfg.syncGeom = _wtoi(val.c_str());
            }
            else if (key == L"verbose")
            {
                g_cfg.verbose = on;
            }
        }
        fclose(f);
    }



    std::wstring
    EscapeText(const std::wstring& in, size_t maxChars = 48)
    {
        std::wstring out;
        const size_t n = in.size() < maxChars ? in.size() : maxChars;
        wchar_t buf[16];
        for (size_t i = 0; i < n; ++i)
        {
            const wchar_t c = in[i];
            if (c >= 0x20 && c < 0x7F)
            {
                out.push_back(c);
            }
            else
            {
                swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(c));
                out += buf;
            }
        }
        if (in.size() > n)
        {
            out += L"...";
        }
        return out;
    }

    constexpr UINT32 PDU_CLIENT_ID = 1;
    constexpr UINT32 PDU_CONTROL_ID = 1;
    constexpr UINT32 PDU_HOST_ID = 1;

    enum class BridgeChannelRole
    {
        ClientToServer,
        ServerToClient,
    };

    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    RemoteTextConnection g_connection{ nullptr };
    winrt::com_ptr<IWTSVirtualChannel> g_spC2SChannel;
    winrt::com_ptr<IWTSVirtualChannel> g_spS2CChannel;
    ITfContext* g_context = nullptr;
    winrt::com_ptr<ITfComposition> g_activeComposition;
    winrt::com_ptr<ITfCompositionView> g_activeCompositionView;
    std::wstring g_lastCompositionText;
    std::wstring g_pendingCommitText;
    TfClientId g_clientId = 0;
    ITfThreadMgr* g_threadMgr = nullptr;
    ITfDocumentMgr* g_docMgr = nullptr;
    ITfKeystrokeMgr* g_keystrokeMgr = nullptr;
    HWND g_railHwnd = nullptr;
    HHOOK g_tsfHook = nullptr;
    bool g_tsfActivated = false;
bool g_ownerAdvised = false;
ITfDocumentMgr* g_appDocMgr = nullptr;
    bool g_sinkAdvised = false;
    bool g_watchdogStop = false;
    std::thread g_windowWatchdog;
    std::set<DWORD> g_registeredThreads;
    uint32_t g_pduOpId = 0;


    bool g_compActive = false;
    bool g_compEntered = false;
    DWORD g_lastReregister = 0;
    DWORD g_lastUpdateMode = 0;
    DWORD g_lastUiProbe = 0;
    DWORD g_lastGeomFix = 0;
    int g_geomPatchLogged = 0;
    volatile LONG g_needCaretBounds = 0;
    int g_imePosApplied = 0;
    int g_imePosFailed = 0;

    volatile LONG g_textExtValid = 0;
    volatile LONG g_gotGeom = 0;          // syncgeom: geometry received for this client
    volatile LONG g_geomCaretR = 0;       // caret right, taken from GEOMETRY_CHANGED only
    volatile LONG g_geomCaretB = 0;       // caret bottom, taken from GEOMETRY_CHANGED only
    volatile LONG g_docLen = 0;
    volatile LONG g_textExtL = 0, g_textExtT = 0, g_textExtR = 0, g_textExtB = 0;
    int g_textExtCalls = 0;
    std::wstring g_lastPreedit;
    std::wstring g_baseDoc;
    std::wstring g_lastCommitDoc;
    std::wstring g_lastLoggedDoc;
    bool g_docValid = false;
    bool g_gtpuProbed = false;
    DWORD g_compGoneTick = 0;
    uint32_t g_preeditSent = 0;
    uint32_t g_commitSent = 0;






    bool g_winFocused = false;
    int g_keyStreak = 0;
    int g_reactivations = 0;
    DWORD g_tsfThreadId = 0;







    volatile LONG g_pendingCaretX = 0;
    volatile LONG g_pendingCaretY = 0;
    volatile LONG g_pendingCaret = 0;
    bool g_caretCreated = false;
    int g_caretApplied = 0;


    void
    SetImeKeyboardState(const wchar_t* why)
    {
        ITfCompartmentMgr* cm = nullptr;

        if (!g_threadMgr || !g_clientId)
        {
            return;
        }
        if (FAILED(g_threadMgr->QueryInterface(
                IID_ITfCompartmentMgr, reinterpret_cast<void**>(&cm))) ||
            !cm)
        {
            return;
        }

        ITfCompartment* c = nullptr;
        if (SUCCEEDED(cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                         &c)) &&
            c)
        {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = 1;   // keyboard open
            HRESULT hrSet = c->SetValue(g_clientId, &v);
            VARIANT r;
            VariantInit(&r);
            HRESULT hrGet = c->GetValue(&r);
            BridgeLog(L"TextBridge: ime keyboard-open set hr=%x read=%d (%s)\n",
                      hrSet,
                      (SUCCEEDED(hrGet) && r.vt == VT_I4) ? r.lVal : -1, why);
            VariantClear(&r);
            c->Release();
        }

        c = nullptr;
        if (SUCCEEDED(cm->GetCompartment(
                GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &c)) &&
            c)
        {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = 1;
            HRESULT hrSet = c->SetValue(g_clientId, &v);
            VARIANT r;
            VariantInit(&r);
            HRESULT hrGet = c->GetValue(&r);
            BridgeLog(L"TextBridge: ime conversion set hr=%x read=%d (%s)\n",
                      hrSet,
                      (SUCCEEDED(hrGet) && r.vt == VT_I4) ? r.lVal : -1, why);
            VariantClear(&r);
            c->Release();
        }
        cm->Release();
    }



    bool
    ReadImeMode(int* open, int* conversion)
    {
        ITfCompartmentMgr* cm = nullptr;
        bool got = false;

        *open = -1;
        *conversion = -1;
        if (!g_threadMgr || FAILED(g_threadMgr->QueryInterface(
                IID_ITfCompartmentMgr, reinterpret_cast<void**>(&cm))) ||
            !cm)
        {
            return false;
        }

        ITfCompartment* c = nullptr;
        if (SUCCEEDED(cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                         &c)) &&
            c)
        {
            VARIANT r;
            VariantInit(&r);
            if (SUCCEEDED(c->GetValue(&r)) && r.vt == VT_I4)
            {
                *open = r.lVal;
                got = true;
            }
            VariantClear(&r);
            c->Release();
        }
        c = nullptr;
        if (SUCCEEDED(cm->GetCompartment(
                GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION, &c)) &&
            c)
        {
            VARIANT r;
            VariantInit(&r);
            if (SUCCEEDED(c->GetValue(&r)) && r.vt == VT_I4)
            {
                *conversion = r.lVal;
                got = true;
            }
            VariantClear(&r);
            c->Release();
        }
        cm->Release();
        return got;
    }

    bool RailWindowFocused();
    void ReactivateTsf();
    void ReactivateInputProfile(const wchar_t* why);




    void
    ReactivateInputProfile(const wchar_t* why)
    {
        ITfInputProcessorProfiles* ipp = nullptr;
        ITfInputProcessorProfileMgr* ppm = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                      CLSCTX_INPROC_SERVER,
                                      IID_ITfInputProcessorProfiles,
                                      reinterpret_cast<void**>(&ipp));
        if (FAILED(hr) || !ipp)
        {
            BridgeLog(L"TextBridge: profile reactivate (%s): create hr=%x\n",
                      why, hr);
            return;
        }
        hr = ipp->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                 reinterpret_cast<void**>(&ppm));
        ipp->Release();
        if (FAILED(hr) || !ppm)
        {
            BridgeLog(L"TextBridge: profile reactivate (%s): qi hr=%x\n", why,
                      hr);
            return;
        }

        TF_INPUTPROCESSORPROFILE prof{};
        hr = ppm->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &prof);
        if (FAILED(hr))
        {
            BridgeLog(L"TextBridge: profile reactivate (%s): get hr=%x\n", why,
                      hr);
            ppm->Release();
            return;
        }

        if (prof.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR)
        {
            HRESULT hrAct = ppm->ActivateProfile(
                prof.dwProfileType, prof.langid, prof.clsid, prof.guidProfile,
                nullptr, 0);
            BridgeLog(L"TextBridge: profile reactivate (%s): TIP langid=%04x "
                      L"clsid=%08x-%04x-%04x act hr=%x\n", why, prof.langid,
                      (unsigned)prof.clsid.Data1, (unsigned)prof.clsid.Data2,
                      (unsigned)prof.clsid.Data3, hrAct);
        }
        else
        {
            IEnumTfInputProcessorProfiles* en = nullptr;
            HRESULT hrEnum = ppm->EnumProfiles(prof.langid, &en);
            BridgeLog(L"TextBridge: profile reactivate (%s): active type=%u is "
                      L"not a TIP (enum hr=%x)\n", why, prof.dwProfileType,
                      hrEnum);
            if (SUCCEEDED(hrEnum) && en)
            {
                TF_INPUTPROCESSORPROFILE p2{};
                ULONG fetched = 0;
                while (en->Next(1, &p2, &fetched) == S_OK && fetched == 1)
                {
                    if (p2.dwProfileType != TF_PROFILETYPE_INPUTPROCESSOR)
                    {
                        continue;
                    }
                    HRESULT hrAct = ppm->ActivateProfile(
                        p2.dwProfileType, p2.langid, p2.clsid, p2.guidProfile,
                        nullptr, 0);
                    BridgeLog(L"TextBridge: profile reactivate (%s): picked TIP "
                              L"langid=%04x clsid=%08x act hr=%x\n", why,
                              p2.langid, (unsigned)p2.clsid.Data1, hrAct);
                    break;
                }
                en->Release();
            }
        }
        ppm->Release();
    }


    bool
    RailWindowFocused()
    {
        GUITHREADINFO gti;

        if (!g_railHwnd)
        {
            return false;
        }
        memset(&gti, 0, sizeof(gti));
        gti.cbSize = sizeof(gti);
        if (!GetGUIThreadInfo(GetCurrentThreadId(), &gti))
        {
            return false;
        }
        if (gti.hwndFocus == g_railHwnd || gti.hwndActive == g_railHwnd)
        {
            return true;
        }
        if (gti.hwndFocus && GetAncestor(gti.hwndFocus, GA_ROOT) == g_railHwnd)
        {
            return true;
        }
        if (gti.hwndActive && GetAncestor(gti.hwndActive, GA_ROOT) == g_railHwnd)
        {
            return true;
        }
        return false;
    }

    HWND CurrentAppWindow();

    bool
    TsfFocusIsOurs()
    {
        ITfDocumentMgr* cur = nullptr;
        bool ours;

        if (!g_threadMgr || !g_docMgr)
        {
            return false;
        }
        if (FAILED(g_threadMgr->GetFocus(&cur)))
        {
            return false;
        }
        ours = (cur == g_docMgr);
        if (cur)
        {
            cur->Release();
        }
        return ours;
    }




    void
    EnsureImeFocus(const wchar_t* why)
    {
        const bool focused = RailWindowFocused();
        const bool tsfOurs = TsfFocusIsOurs();
        const bool transition = (focused != g_winFocused);

        g_winFocused = focused;

        if (!focused || (!transition && tsfOurs))
        {
            return;
        }

        BOOL threadFocus = FALSE;
        if (g_threadMgr)
        {
            g_threadMgr->IsThreadFocus(&threadFocus);
        }
        BridgeLog(L"TextBridge: ime state restore (%s) transition=%d "
                  L"tsfFocusOurs=%d threadFocus=%d\n",
                  why, transition ? 1 : 0, tsfOurs ? 1 : 0,
                  threadFocus ? 1 : 0);

        if (g_threadMgr && g_docMgr && g_railHwnd)
        {
            ITfDocumentMgr* prev = nullptr;

            if (SUCCEEDED(g_threadMgr->AssociateFocus(g_railHwnd, g_docMgr,
                                                &prev)))
            {
                BridgeLog(L"TextBridge: re-associated focus hwnd=%p (%s)\\n",
                          (void*)g_railHwnd, why);
            }

            if (prev)
            {
                prev->Release();
            }

            g_threadMgr->SetFocus(g_docMgr);
        }
        SetImeKeyboardState(why);
        ReactivateInputProfile(why);
        g_keyStreak = 0;
    }


    bool
    IsPrintableKey(WPARAM vk)
    {
        if (vk >= '0' && vk <= '9')
        {
            return true;
        }
        if (vk >= 'A' && vk <= 'Z')
        {
            return true;
        }
        if (vk >= 0x60 && vk <= 0x6F)
        {
            return true;
        }
        return vk == VK_SPACE || vk == VK_OEM_1 || vk == VK_OEM_2 ||
               vk == VK_OEM_3 || vk == VK_OEM_4 || vk == VK_OEM_5 ||
               vk == VK_OEM_6 || vk == VK_OEM_7;
    }



    void
    PhysicalToLogical(LONG& x, LONG& y)
    {
        (void)x;
        (void)y;
    }

    void
    StashCaretFromBounds(const uint8_t* b, const wchar_t* src)
    {
        int32_t left, top, right, bottom;
        RECT wr = { 0, 0, 0, 0 };
        bool isWindowFallback = false;

        if (!g_cfg.useCaret || !b)
        {
            return;
        }
        left = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
        top = (int32_t)((uint32_t)b[4] | ((uint32_t)b[5] << 8) |
                        ((uint32_t)b[6] << 16) | ((uint32_t)b[7] << 24));
        right = (int32_t)((uint32_t)b[8] | ((uint32_t)b[9] << 8) |
                          ((uint32_t)b[10] << 16) | ((uint32_t)b[11] << 24));
        bottom = (int32_t)((uint32_t)b[12] | ((uint32_t)b[13] << 8) |
                           ((uint32_t)b[14] << 16) | ((uint32_t)b[15] << 24));


        PhysicalToLogical((LONG&)left, (LONG&)top);
        PhysicalToLogical((LONG&)right, (LONG&)bottom);

        if (right <= left || bottom < top || left < -32000 || top < -32000)
        {
            return;
        }


        const HWND app = CurrentAppWindow();

        if (app && GetWindowRect(app, &wr))
        {

            int w = wr.right - wr.left;
            int h = wr.bottom - wr.top;
            int bw = right - left;
            int bh = bottom - top;
            if (bw >= w - 8 && bw <= w + 8 && bh >= h - 8 && bh <= h + 8)
            {
                isWindowFallback = true;
            }
        }
        if (isWindowFallback)
        {
            BridgeLog(L"TextBridge: caret bounds (%s) %ld,%ld,%ld,%ld is the "
                      L"whole-window fallback, asking for a refresh\n", src,
                      (long)left, (long)top, (long)right, (long)bottom);
            InterlockedExchange(&g_needCaretBounds, 1);
            return;
        }

        InterlockedExchange(&g_pendingCaretX, (LONG)left);
        InterlockedExchange(&g_pendingCaretY, (LONG)bottom);
        InterlockedExchange(&g_pendingCaret, 1);
        InterlockedExchange(&g_needCaretBounds, 0);

        InterlockedExchange(&g_textExtL, (LONG)left);
        InterlockedExchange(&g_textExtT, (LONG)top);
        InterlockedExchange(&g_textExtR, (LONG)right);
        InterlockedExchange(&g_textExtB, (LONG)bottom);
        InterlockedExchange(&g_textExtValid, 1);
        (void)src;
    }



    //




    //



    void
    ApplyImeWindowPos(int sx, int sy)
    {
        HIMC himc;
        CANDIDATEFORM cand;
        COMPOSITIONFORM comp;
        BOOL okCand, okComp;

        himc = ImmGetContext(g_railHwnd);
        if (!himc)
        {
            if (g_imePosFailed < 4)
            {
                ++g_imePosFailed;
                BridgeLog(L"TextBridge: ImmGetContext failed err=%lu\n",
                          (unsigned long)GetLastError());
            }
            return;
        }

        memset(&cand, 0, sizeof(cand));
        cand.dwIndex = 0;
        cand.dwStyle = CFS_CANDIDATEPOS;
        cand.ptCurrentPos.x = sx;
        cand.ptCurrentPos.y = sy;
        okCand = ImmSetCandidateWindow(himc, &cand);

        memset(&comp, 0, sizeof(comp));
        comp.dwStyle = CFS_POINT;
        comp.ptCurrentPos.x = sx;
        comp.ptCurrentPos.y = sy;
        okComp = ImmSetCompositionWindow(himc, &comp);

        ImmReleaseContext(g_railHwnd, himc);

        if (g_imePosApplied < 8 || g_cfg.verbose)
        {
            ++g_imePosApplied;
            BridgeLog(L"TextBridge: IME window pos %d,%d (cand=%d comp=%d) #%d\n",
                      sx, sy, (int)okCand, (int)okComp, g_imePosApplied);
        }
    }

    void
    ApplyPendingCaret()
    {
        LONG x, y;
        int sx, sy;
        RECT wr = { 0, 0, 0, 0 };

        if (!g_railHwnd)
        {
            return;
        }
        if (!g_cfg.useCaret)
        {
            InterlockedExchange(&g_pendingCaret, 0);
            if (g_caretCreated)
            {
                DestroyCaret();
                g_caretCreated = false;
                BridgeLog(L"TextBridge: caret destroyed (caret=0)\n");
            }
            return;
        }
        if (InterlockedExchange(&g_pendingCaret, 0) == 0)
        {
            return;
        }
        x = InterlockedCompareExchange(&g_pendingCaretX, 0, 0);
        y = InterlockedCompareExchange(&g_pendingCaretY, 0, 0);


        sx = (int)x;
        sy = (int)y;
        if (GetWindowRect(g_railHwnd, &wr))
        {
            int altx = sx - wr.left, alty = sy - wr.top;
            bool baseIn = (sx >= wr.left && sx <= wr.right &&
                           sy >= wr.top && sy <= wr.bottom);
            bool altIn = (altx >= wr.left && altx <= wr.right &&
                          alty >= wr.top && alty <= wr.bottom);
            if (!baseIn && altIn)
            {
                sx = altx;
                sy = alty;
            }
        }

        if (GetWindowRect(g_railHwnd, &wr))
        {
            if (sx < wr.left) sx = wr.left;
            if (sx > wr.right - 8) sx = wr.right - 8;
            if (sy < wr.top) sy = wr.top;
            if (sy > wr.bottom - 8) sy = wr.bottom - 8;
        }


        ApplyImeWindowPos(sx, sy);

        if (!g_caretCreated)
        {
            if (!CreateCaret(g_railHwnd, nullptr, 1, 1))
            {
                BridgeLog(L"TextBridge: CreateCaret failed err=%lu\n",
                          (unsigned long)GetLastError());
                return;
            }
            g_caretCreated = true;
        }
        if (!SetCaretPos(sx, sy))
        {
            DWORD err = GetLastError();

            DestroyCaret();
            g_caretCreated = false;
            if (CreateCaret(g_railHwnd, nullptr, 1, 1) &&
                SetCaretPos(sx, sy))
            {
                g_caretCreated = true;
                BridgeLog(L"TextBridge: caret recreated at %d,%d (raw %ld,%ld)\n",
                          sx, sy, x, y);
                return;
            }
            BridgeLog(L"TextBridge: SetCaretPos(%d,%d) failed err=%lu (raw "
                      L"%ld,%ld)\n", sx, sy, (unsigned long)err, x, y);
            return;
        }
        if (g_caretApplied < 8 || g_cfg.verbose)
        {
            ++g_caretApplied;
            BridgeLog(L"TextBridge: caret %d,%d (raw %ld,%ld, window "
                      L"%ld,%ld,%ld,%ld) #%d\n", sx, sy, x, y, wr.left,
                      wr.top, wr.right, wr.bottom, g_caretApplied);
        }
    }






    HRESULT
    ReadContextText(TfEditCookie ec, std::wstring& out)
    {
        out.clear();
        if (!g_context)
        {
            return E_FAIL;
        }

        ITfRange* start = nullptr;
        ITfRange* end = nullptr;
        HRESULT hr = g_context->GetStart(ec, &start);
        if (FAILED(hr) || !start)
        {
            return hr;
        }
        hr = g_context->GetEnd(ec, &end);
        if (FAILED(hr) || !end)
        {
            start->Release();
            return hr;
        }

        ITfRange* whole = nullptr;
        hr = start->Clone(&whole);
        if (SUCCEEDED(hr) && whole)
        {
            hr = whole->ShiftEndToRange(ec, end, TF_ANCHOR_END);
            if (SUCCEEDED(hr))
            {
                std::wstring prev;
                LONG off = 0;
                for (int i = 0; i < 16; ++i)
                {
                    wchar_t buf[1024];
                    ULONG got = 0;
                    if (FAILED(whole->GetText(ec, off, buf, 1023, &got)) ||
                        got == 0)
                    {
                        break;
                    }
                    std::wstring chunk(buf, got);
                    if (chunk == prev)
                    {
                        break;
                    }
                    prev = chunk;
                    out += chunk;
                    off += (LONG)got;
                    if (got < 1023)
                    {
                        break;
                    }
                }
            }
            whole->Release();
        }
        end->Release();
        start->Release();
        return hr;
    }

    // ------------------------------------------------------------------


    // ------------------------------------------------------------------
    void
    SendPdu(UINT16 pduId, const std::vector<uint8_t>& payload)
    {
        if (!g_spC2SChannel)
        {
            BridgeLog(L"TextBridge: SendPdu(0x%04x) dropped, channel not ready\n", pduId);
            return;
        }
        UINT32 inner = 2 + static_cast<UINT32>(payload.size());
        UINT32 outer = 4 + inner;
        std::vector<uint8_t> pdu;
        pdu.reserve(outer);
        auto put32 = [&pdu](UINT32 v) {
            pdu.push_back((uint8_t)(v));
            pdu.push_back((uint8_t)(v >> 8));
            pdu.push_back((uint8_t)(v >> 16));
            pdu.push_back((uint8_t)(v >> 24));
        };
        auto put16 = [&pdu](UINT16 v) {
            pdu.push_back((uint8_t)(v));
            pdu.push_back((uint8_t)(v >> 8));
        };
        put32(outer);
        put32(inner);
        put16(pduId);
        pdu.insert(pdu.end(), payload.begin(), payload.end());

        HRESULT hr = g_spC2SChannel->Write(
            static_cast<ULONG>(pdu.size()), pdu.data(), nullptr);
        if (FAILED(hr))
        {
            BridgeLog(L"TextBridge: SendPdu(0x%04x) write failed hr=%x\n", pduId, hr);
        }
    }



    //   0x01 EnterComposition / 0x02 LeaveComposition / 0x03 UpdateComposition



    constexpr uint8_t COMPOSITION_ENTER = 0x01;
    constexpr uint8_t COMPOSITION_LEAVE = 0x02;
    constexpr uint8_t COMPOSITION_UPDATE = 0x03;

    void
    SendComposition(const std::wstring& text, uint8_t action)
    {
        std::vector<uint8_t> payload;
        auto put32 = [&payload](UINT32 v) {
            payload.push_back((uint8_t)(v));
            payload.push_back((uint8_t)(v >> 8));
            payload.push_back((uint8_t)(v >> 16));
            payload.push_back((uint8_t)(v >> 24));
        };
        put32(PDU_CLIENT_ID);
        put32(PDU_CONTROL_ID);
        put32(PDU_HOST_ID);
        put32(++g_pduOpId);
        payload.push_back(action);
        put32(1);                               // clausesCount = 1
        put32((UINT32)text.size());
        for (wchar_t c : text)
        {
            payload.push_back((uint8_t)c);
            payload.push_back((uint8_t)((uint16_t)c >> 8));
        }
        put32(0);                               // clause range.begin
        put32(0);                               // clause range.end
        BridgeLog(L"TextBridge: -> UPDATE_COMPOSITION action=%u len=%u\n",
                  action, (UINT32)text.size());
        SendPdu(0x0204, payload);
    }


    void
    SendUpdateText(const std::wstring& text)
    {
        std::vector<uint8_t> payload;
        auto put32 = [&payload](UINT32 v) {
            payload.push_back((uint8_t)(v));
            payload.push_back((uint8_t)(v >> 8));
            payload.push_back((uint8_t)(v >> 16));
            payload.push_back((uint8_t)(v >> 24));
        };
        put32(PDU_CLIENT_ID);
        put32(PDU_CONTROL_ID);
        put32(PDU_HOST_ID);
        put32(++g_pduOpId);
        put32(0);                               // replaceBegin
        put32(0);                               // replaceEnd
        put32((UINT32)text.size());
        for (wchar_t c : text)
        {
            payload.push_back((uint8_t)c);
            payload.push_back((uint8_t)((uint16_t)c >> 8));
        }
        BridgeLog(L"TextBridge: -> UPDATE_TEXT len=%u\n", (UINT32)text.size());
        SendPdu(0x0200, payload);
    }


    constexpr UINT32 RDPTXT_FEATURE_PREDICTION_MODE = 0x00000001;
    constexpr UINT32 RDPTXT_FEATURE_LAYOUT_CHANGE_TRACKING = 0x00000004;
    constexpr UINT32 RDPTXT_FEATURE_SELECTION_TRACKING = 0x00000008;


    //   hostId(4) clientId(4) controlId(4) features(4) enabled(1)
    //   customRange(8) predictionModeTriggerLength(4) triggers(2n) operationId(4)
    //




    void
    SendUpdateMode(UINT32 features, bool enabled)
    {
        std::vector<uint8_t> payload;
        auto put32 = [&payload](UINT32 v) {
            payload.push_back((uint8_t)(v));
            payload.push_back((uint8_t)(v >> 8));
            payload.push_back((uint8_t)(v >> 16));
            payload.push_back((uint8_t)(v >> 24));
        };
        put32(PDU_HOST_ID);
        put32(PDU_CLIENT_ID);
        put32(PDU_CONTROL_ID);
        put32(features);
        payload.push_back(enabled ? 1 : 0);
        put32(0);                    // customRange begin
        put32(0);                    // customRange end
        put32(0);                    // predictionModeTriggerLength
        put32(++g_pduOpId);          // operationId
        SendPdu(0x0209, payload);
    }






    int g_uiProbeLogged = 0;

    void
    ProbeUiElements(const wchar_t* why)
    {
        ITfUIElementMgr* mgr = nullptr;
        IEnumTfUIElements* en = nullptr;

        if (!g_threadMgr)
        {
            return;
        }
        if (FAILED(g_threadMgr->QueryInterface(IID_ITfUIElementMgr,
                                               (void**)&mgr)) || !mgr)
        {
            if (g_uiProbeLogged++ < 2)
            {
                BridgeLog(L"TextBridge: [ui] ITfUIElementMgr unavailable (%s)\n",
                          why);
            }
            return;
        }
        if (SUCCEEDED(mgr->EnumUIElements(&en)) && en)
        {
            ITfUIElement* el = nullptr;
            ULONG fetched = 0;
            int n = 0;
            while (en->Next(1, &el, &fetched) == S_OK && el && fetched == 1)
            {
                BSTR desc = nullptr;
                BOOL shown = FALSE;
                GUID g = {};
                el->GetDescription(&desc);
                el->IsShown(&shown);
                el->GetGUID(&g);
                if (g_uiProbeLogged < 12)
                {
                    ++g_uiProbeLogged;
                    BridgeLog(L"TextBridge: [ui] element shown=%d "
                              L"guid={%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X"
                              L"%02X%02X} desc=\"%s\"\n",
                              (int)shown, (unsigned)g.Data1, (unsigned)g.Data2,
                              (unsigned)g.Data3, g.Data4[0], g.Data4[1],
                              g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5],
                              g.Data4[6], g.Data4[7], desc ? desc : L"");
                }
                if (desc)
                {
                    SysFreeString(desc);
                }
                el->Release();
                el = nullptr;
                if (++n >= 6)
                {
                    break;
                }
            }
            en->Release();
        }
        mgr->Release();
    }






    volatile LONG g_lastGoodCtrl[4] = { 0, 0, 0, 0 };

    bool
    GuardEmptyControlBounds(const uint8_t* src, ULONG cbSize,
                            std::vector<uint8_t>& out)
    {
        if (cbSize < 6 + 4 + 4 + 16)
        {
            return false;
        }

        const UINT16 pid = (UINT16)(src[4] | ((UINT16)src[5] << 8));
        size_t off;

        if (pid == 0x0308)
        {
            off = 6 + 4;
        }
        else if (pid == 0x030F)
        {
            off = 6 + 4 + 4;
        }
        else
        {
            return false;
        }

        if (cbSize < off + 16)
        {
            return false;
        }

        LONG c[4];

        for (int i = 0; i < 4; ++i)
        {
            const uint8_t* b = src + off + i * 4;

            c[i] = (LONG)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                          ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24));
        }

        if (c[2] <= c[0] || c[3] <= c[1])
        {
            BridgeLog(L"TextBridge: [spec] dropped 0x%04x with empty controlBounds\n",
                      pid);

            out.clear();

            return true;
        }

        RECT wr = {};

        const HWND app = CurrentAppWindow();

        if (!app || !GetWindowRect(app, &wr))
        {
            return false;
        }

        out.assign(src, src + cbSize);

        /* controlBounds left as sent by weston (caret-centred) */


        /* inputSettings left as sent by weston (no bottom-edge alignment) */

        if (pid == 0x0308 && cbSize >= 6 + 20 + 36)
        if (pid == 0x0308 && cbSize >= 6 + 20 + 36)
        {
            out[6 + 20 + 8] = 0;
            out[6 + 20 + 9] = 0;
            out[6 + 20 + 10] = 0;
            out[6 + 20 + 11] = 0;
        }

        BridgeLog(L"TextBridge: [spec] 0x%04x controlBounds -> window %ld,%ld,%ld,%ld\n",
                  pid, (long)wr.left, (long)wr.top, (long)wr.right,
                  (long)wr.bottom);

        return true;
    }


    bool
    PatchGeometryBounds(const uint8_t* src, ULONG cbSize,
                        std::vector<uint8_t>& out)
    {
        RECT wr = {};
        UINT16 pid;
        size_t off;


        LoadConfig();

        out.assign(src, src + cbSize);
        if (cbSize < 6 + 4 + 16 || !g_railHwnd || !GetWindowRect(g_railHwnd, &wr))
        {
            return false;
        }
        pid = (UINT16)(out[4] | ((UINT16)out[5] << 8));
        if (pid == 0x030F)
        {
            off = 6 + 4 + 4;             // clientId4 + controlId4
        }
        else if (pid == 0x0308)
        {
            off = 6 + 4;                 // clientId4
        }
        else
        {
            return false;
        }
        if (cbSize < off + 16)
        {
            return false;
        }




        if (g_cfg.geomFix == 20 || g_cfg.geomFix == 21 ||
            g_cfg.geomFix == 22 || g_cfg.geomFix == 23 ||
            g_cfg.geomFix == 24)
        {
            LONG cx2;
            LONG cy2;
            LONG wv[4];
            LONG halfw = 800;    /* 20: width 1600 (proven good) */


            if ((g_cfg.geomFix == 21 && pid != 0x030F) ||
                (g_cfg.geomFix == 22 && pid != 0x0308))
            {
                return false;
            }
            cx2 = InterlockedCompareExchange(&g_geomCaretR, 0, 0);
            cy2 = InterlockedCompareExchange(&g_geomCaretB, 0, 0);
            if (cx2 <= 0 || cy2 <= 0)
            {

                return false;
            }

            if (g_cfg.geomFix == 23)
            {
                halfw = 600;
            }
            else if (g_cfg.geomFix == 24)
            {
                halfw = (wr.right - wr.left) / 2;
                if (halfw <= 0)
                {
                    halfw = 952;
                }
            }
            wv[0] = cx2 - halfw;
            wv[1] = wr.top;
            wv[2] = cx2 + halfw;
            wv[3] = cy2;

            for (int i = 0; i < 4; ++i)
            {
                UINT32 u = (UINT32)wv[i];
                out[off + i * 4 + 0] = (uint8_t)u;
                out[off + i * 4 + 1] = (uint8_t)(u >> 8);
                out[off + i * 4 + 2] = (uint8_t)(u >> 16);
                out[off + i * 4 + 3] = (uint8_t)(u >> 24);
            }
            if (g_cfg.verbose && g_geomPatchLogged < 40)
            {
                ++g_geomPatchLogged;
                BridgeLog(L"TextBridge: [geomfix%d] 0x%04x ctrl=%ld,%ld,%ld,%ld\n",
                          g_cfg.geomFix, pid, (long)wv[0], (long)wv[1],
                          (long)wv[2], (long)wv[3]);
            }
            return true;
        }

        if (g_cfg.geomFix == 11 || g_cfg.geomFix == 12 ||
            g_cfg.geomFix == 13 || g_cfg.geomFix == 14 ||
            g_cfg.geomFix == 15 || g_cfg.geomFix == 16 ||
            g_cfg.geomFix == 17)
        {
            LONG wv[4] = { wr.left, wr.top, wr.right, wr.bottom };
            LONG n = InterlockedCompareExchange(&g_docLen, 0, 0);

            LONG rend = (g_cfg.geomFix == 15) ? 0 :
                        (g_cfg.geomFix == 16) ? 14 :
                        (g_cfg.geomFix == 17) ? 6 : n;
            LONG rbeg = (g_cfg.geomFix == 13 || g_cfg.geomFix == 14) ? 0 : rend;

            for (int i = 0; i < 4; ++i)
            {
                UINT32 u = (UINT32)wv[i];
                out[off + i * 4 + 0] = (uint8_t)u;
                out[off + i * 4 + 1] = (uint8_t)(u >> 8);
                out[off + i * 4 + 2] = (uint8_t)(u >> 16);
                out[off + i * 4 + 3] = (uint8_t)(u >> 24);
            }
            if (pid == 0x030F && cbSize >= off + 16 + 8)
            {
                UINT32 r0 = (UINT32)rbeg;
                UINT32 r1 = (UINT32)rend;

                out[off + 16] = (uint8_t)r0;
                out[off + 17] = (uint8_t)(r0 >> 8);
                out[off + 18] = (uint8_t)(r0 >> 16);
                out[off + 19] = (uint8_t)(r0 >> 24);
                out[off + 20] = (uint8_t)r1;
                out[off + 21] = (uint8_t)(r1 >> 8);
                out[off + 22] = (uint8_t)(r1 >> 16);
                out[off + 23] = (uint8_t)(r1 >> 24);
            }
            if ((g_cfg.geomFix == 12 || g_cfg.geomFix == 13) &&
                pid == 0x030F && cbSize >= off + 16 + 8 + 16)
            {
                LONG cx2 = InterlockedCompareExchange(&g_textExtR, 0, 0);
                LONG cy1 = InterlockedCompareExchange(&g_textExtT, 0, 0);
                LONG cy2 = InterlockedCompareExchange(&g_textExtB, 0, 0);
                LONG rv[4] = { wr.left, cy1, cx2, cy2 };
                size_t roff = off + 16 + 8;

                for (int i = 0; i < 4; ++i)
                {
                    UINT32 u = (UINT32)rv[i];
                    out[roff + i * 4 + 0] = (uint8_t)u;
                    out[roff + i * 4 + 1] = (uint8_t)(u >> 8);
                    out[roff + i * 4 + 2] = (uint8_t)(u >> 16);
                    out[roff + i * 4 + 3] = (uint8_t)(u >> 24);
                }
            }
            if (g_cfg.verbose && g_geomPatchLogged < 40)
            {
                ++g_geomPatchLogged;
                BridgeLog(L"TextBridge: [geomfix%d] 0x%04x ctrl=%ld,%ld,%ld,%ld "
                          L"range=%ld,%ld\n", g_cfg.geomFix, pid, (long)wv[0],
                          (long)wv[1], (long)wv[2], (long)wv[3],
                          (long)rbeg, (long)rend);
            }
            return true;
        }

        if (g_cfg.geomFix == 10)
        {
            UINT32 n;
            size_t roff = off + 16;

            if (pid != 0x030F || cbSize < roff + 8)
            {
                return false;
            }
            n = (UINT32)InterlockedCompareExchange(&g_docLen, 0, 0);
            out[roff + 0] = (uint8_t)n;
            out[roff + 1] = (uint8_t)(n >> 8);
            out[roff + 2] = (uint8_t)(n >> 16);
            out[roff + 3] = (uint8_t)(n >> 24);
            out[roff + 4] = (uint8_t)n;
            out[roff + 5] = (uint8_t)(n >> 8);
            out[roff + 6] = (uint8_t)(n >> 16);
            out[roff + 7] = (uint8_t)(n >> 24);
            if (g_cfg.verbose && g_geomPatchLogged < 40)
            {
                ++g_geomPatchLogged;
                BridgeLog(L"TextBridge: [geomfix10] 0x030f range -> %u,%u "
                          L"(rects untouched)\n", n, n);
            }
            return true;
        }

        LONG vals[4] = { wr.left, wr.top, wr.right, wr.bottom };


        if (g_cfg.geomFix >= 4 && pid == 0x030F)
        {
            LONG cx1 = InterlockedCompareExchange(&g_textExtL, 0, 0);
            LONG cy1 = InterlockedCompareExchange(&g_textExtT, 0, 0);
            LONG cy2 = InterlockedCompareExchange(&g_textExtB, 0, 0);
            LONG h = (cy2 > cy1) ? (cy2 - cy1) : 20;

            vals[0] = cx1;
            vals[1] = cy1;
            vals[2] = cx1 + h;
            vals[3] = cy2;
        }

        {
            LONG ccx1 = InterlockedCompareExchange(&g_textExtL, 0, 0);
            LONG ccy1 = InterlockedCompareExchange(&g_textExtT, 0, 0);
            LONG ccx2 = InterlockedCompareExchange(&g_textExtR, 0, 0);
            LONG ccy2 = InterlockedCompareExchange(&g_textExtB, 0, 0);
            LONG lineH = (ccy2 > ccy1) ? (ccy2 - ccy1) : 20;

            if (pid == 0x030F && g_cfg.geomFix == 4)
            {
                vals[0] = ccx1; vals[1] = ccy1; vals[2] = ccx1 + lineH; vals[3] = ccy2;
            }
            else if (pid == 0x030F && g_cfg.geomFix == 5)
            {
                vals[0] = wr.left; vals[1] = ccy1; vals[2] = ccx2; vals[3] = ccy2;
            }
            else if (pid == 0x030F && g_cfg.geomFix == 6)
            {
                vals[0] = wr.left; vals[1] = ccy1; vals[2] = ccx2; vals[3] = ccy2 + lineH;
            }
            else if (pid == 0x030F && g_cfg.geomFix == 7)
            {
                vals[0] = ccx1 - 150; vals[1] = ccy1; vals[2] = ccx1 + 150;
                vals[3] = ccy1 + lineH * 2;
            }
            else if (g_cfg.geomFix == 9)
            {

                vals[0] = 0;
                vals[1] = 0;
                vals[2] = ccx2;
                vals[3] = ccy2;
            }
            else if (g_cfg.geomFix == 8)
            {

                LONG W = wr.right - wr.left;
                LONG H = wr.bottom - wr.top;

                if (W <= 0) { W = 800; }
                if (H <= 0) { H = 600; }
                vals[0] = ccx2 - W;
                vals[1] = ccy2 - H;
                vals[2] = ccx2;
                vals[3] = ccy2;
            }
        }
        for (int i = 0; i < 4; ++i)
        {
            UINT32 u = (UINT32)vals[i];
            out[off + i * 4 + 0] = (uint8_t)u;
            out[off + i * 4 + 1] = (uint8_t)(u >> 8);
            out[off + i * 4 + 2] = (uint8_t)(u >> 16);
            out[off + i * 4 + 3] = (uint8_t)(u >> 24);
        }
        if (pid == 0x030F && g_cfg.geomFix >= 2 &&
            cbSize >= off + 16 + 8 + 16)
        {

            size_t roff = off + 16 + 8;
            LONG rv[4];
            if (g_cfg.geomFix >= 8)
            {

                rv[0] = InterlockedCompareExchange(&g_textExtL, 0, 0);
                rv[1] = InterlockedCompareExchange(&g_textExtT, 0, 0);
                rv[2] = InterlockedCompareExchange(&g_textExtR, 0, 0);
                rv[3] = InterlockedCompareExchange(&g_textExtB, 0, 0);
            }
            else if (g_cfg.geomFix >= 3)
            {
                LONG cx1 = InterlockedCompareExchange(&g_textExtL, 0, 0);
                LONG cy1 = InterlockedCompareExchange(&g_textExtT, 0, 0);
                LONG cy2 = InterlockedCompareExchange(&g_textExtB, 0, 0);
                LONG h = (cy2 > cy1) ? (cy2 - cy1) : 20;
                rv[0] = cx1;
                rv[1] = cy1;
                rv[2] = cx1 + h;
                rv[3] = cy2;
            }
            else
            {
                rv[0] = wr.left;
                rv[1] = wr.top;
                rv[2] = wr.right;
                rv[3] = wr.bottom;
            }
            for (int i = 0; i < 4; ++i)
            {
                UINT32 u = (UINT32)rv[i];
                out[roff + i * 4 + 0] = (uint8_t)u;
                out[roff + i * 4 + 1] = (uint8_t)(u >> 8);
                out[roff + i * 4 + 2] = (uint8_t)(u >> 16);
                out[roff + i * 4 + 3] = (uint8_t)(u >> 24);
            }
        }
        if (g_cfg.verbose && g_geomPatchLogged < 40)
        {
            ++g_geomPatchLogged;
            BridgeLog(L"TextBridge: [geomfix%d] 0x%04x sent ctrl=%ld,%ld,%ld,%ld "
                      L"(caret %ld,%ld,%ld,%ld)\n",
                      g_cfg.geomFix, pid,
                      (long)vals[0], (long)vals[1], (long)vals[2], (long)vals[3],
                      (long)InterlockedCompareExchange(&g_textExtL, 0, 0),
                      (long)InterlockedCompareExchange(&g_textExtT, 0, 0),
                      (long)InterlockedCompareExchange(&g_textExtR, 0, 0),
                      (long)InterlockedCompareExchange(&g_textExtB, 0, 0));
        }
        return true;
    }

    // ------------------------------------------------------------------







    // ------------------------------------------------------------------
    void
    CaptureCompositionState(bool hasComposition,
                            const std::wstring& compositionText,
                            const std::wstring& docText,
                            const wchar_t* changedText, size_t changedLen,
                            const wchar_t* source)
    {
        if (g_cfg.verbose && docText != g_lastLoggedDoc)
        {
            g_lastLoggedDoc = docText;
            BridgeLog(L"TextBridge: doc text \"%s\" (%u chars)\n",
                      EscapeText(docText, 96).c_str(), (UINT32)docText.size());
        }

        if (hasComposition)
        {
            std::wstring pre = compositionText;
            if (pre.empty() && changedLen)
            {
                pre.assign(changedText, changedLen);
            }
            g_lastCompositionText = pre;
            g_compActive = true;
            g_compGoneTick = 0;
            g_keyStreak = 0;


            if (!docText.empty())
            {
                if (!pre.empty() && docText.size() >= pre.size() &&
                    docText.compare(docText.size() - pre.size(), pre.size(), pre) == 0)
                {
                    g_baseDoc = docText.substr(0, docText.size() - pre.size());
                }
                else
                {
                    g_baseDoc = docText;
                }
                g_docValid = true;
            }

            if (pre != g_lastPreedit)
            {
                g_lastPreedit = pre;
                ++g_preeditSent;

                SendComposition(pre, g_compEntered ? COMPOSITION_UPDATE
                                                   : COMPOSITION_ENTER);
                if (!g_compEntered)
                {
                    g_compEntered = true;
                }

                {
                    DWORD now = GetTickCount();
                    DWORD wait = g_needCaretBounds ? 500 : 1000;
                    if (now - g_lastReregister > wait)
                    {
                        g_lastReregister = now;
                        SendPdu(0x0603, std::vector<uint8_t>());
                        BridgeLog(L"TextBridge: -> REREGISTRATION_REQUEST "
                                  L"(refresh control bounds, need=%ld)\n",
                                  (long)g_needCaretBounds);
                    }

                    if (now - g_lastUpdateMode > 600)
                    {
                        g_lastUpdateMode = now;

                        SendUpdateMode(RDPTXT_FEATURE_PREDICTION_MODE |
                                       RDPTXT_FEATURE_LAYOUT_CHANGE_TRACKING |
                                       RDPTXT_FEATURE_SELECTION_TRACKING, true);
                        BridgeLog(L"TextBridge: -> UPDATE_MODE (prediction+"
                                  L"layout+selection)\n");
                    }
                    if (now - g_lastUiProbe > 400)
                    {
                        g_lastUiProbe = now;
                        ProbeUiElements(L"composing");
                    }
                }
                InterlockedExchange(&g_docLen, (LONG)docText.size());
                BridgeLog(L"TextBridge: [%s] preedit #%u \"%s\" (doc %u)\n",
                          source, g_preeditSent, EscapeText(pre).c_str(),
                          (UINT32)docText.size());
            }
            return;
        }


        if (!g_compActive && g_lastCompositionText.empty())
        {
            return;
        }

        std::wstring commit;
        const wchar_t* commitSrc = L"last-composition";
        bool docDeltaKnown = false;
        if (g_docValid && docText.size() >= g_baseDoc.size() &&
            docText.compare(0, g_baseDoc.size(), g_baseDoc) == 0)
        {
            commit = docText.substr(g_baseDoc.size());
            commitSrc = L"doc-delta";
            docDeltaKnown = true;
        }
        else if (changedLen)
        {
            commit.assign(changedText, changedLen);
            commitSrc = L"edit-record";
        }
        else
        {
            commit = g_lastCompositionText;
        }

        g_compActive = false;
        g_compGoneTick = 0;
        g_lastCompositionText.clear();
        g_lastPreedit.clear();
        g_lastCommitDoc = docText;
        g_baseDoc.clear();
        g_docValid = false;

        if (commit.empty())
        {

            BridgeLog(L"TextBridge: [%s/%s] composition ended with no new "
                      L"text (cancel), doc %u\n", source,
                      docDeltaKnown ? L"doc-delta-empty" : L"no-source",
                      (UINT32)docText.size());
            if (g_compEntered)
            {
                SendComposition(L"", COMPOSITION_LEAVE);
                g_compEntered = false;
            }
            return;
        }

        ++g_commitSent;
        SendUpdateText(commit);
        if (g_compEntered)
        {
            SendComposition(L"", COMPOSITION_LEAVE);
            g_compEntered = false;
        }
        BridgeLog(L"TextBridge: [%s/%s] commit #%u \"%s\" (%u chars, doc %u)\n",
                  source, commitSrc, g_commitSent, EscapeText(commit).c_str(),
                  (UINT32)commit.size(), (UINT32)docText.size());
    }



    void
    PollCapture(bool hasComposition, const std::wstring& compositionText,
                const std::wstring& docText)
    {
        if (hasComposition)
        {
            CaptureCompositionState(true, compositionText, docText, nullptr, 0,
                                    L"poll");
            return;
        }

        if (!g_compActive && g_lastCompositionText.empty())
        {
            return;
        }

        if (g_sinkAdvised)
        {
            if (g_compGoneTick == 0)
            {
                g_compGoneTick = GetTickCount();
                return;
            }
            if (GetTickCount() - g_compGoneTick < 200)
            {
                return;
            }
            if (g_cfg.verbose)
            {
                BridgeLog(L"TextBridge: sink missed the commit, poll fallback "
                          L"(doc %u)\n", (UINT32)docText.size());
            }
        }

        CaptureCompositionState(false, std::wstring(), docText, nullptr, 0,
                                L"poll-fallback");
    }


    std::wstring
    ReadCompositionText(TfEditCookie ec, ITfComposition* composition)
    {
        std::wstring out;
        if (!composition)
        {
            return out;
        }
        ITfRange* range = nullptr;
        HRESULT hr = composition->GetRange(&range);
        if (FAILED(hr) || !range)
        {
            BridgeLog(L"TextBridge: composition GetRange failed hr=%x\n", hr);
            return out;
        }
        wchar_t buf[1024];
        ULONG got = 0;
        hr = range->GetText(ec, 0, buf, 1023, &got);
        if (SUCCEEDED(hr))
        {
            out.assign(buf, got);
        }
        else
        {
            BridgeLog(L"TextBridge: composition GetText failed hr=%x\n", hr);
        }
        range->Release();
        return out;
    }

    // ------------------------------------------------------------------


    // ------------------------------------------------------------------
    // ------------------------------------------------------------------





    // ------------------------------------------------------------------
    bool
    CaretRectScreen(RECT* out);

    class BridgeTextStore final :
        public ITextStoreACP
    {
        LONG m_ref = 1;
        ITextStoreACPSink* m_pSink = nullptr;

    public:
        std::wstring text;
        LONG selStart = 0, selEnd = 0;
        std::wstring inserted;

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_ITextStoreACP)
                *ppv = static_cast<ITextStoreACP*>(this);
            else { *ppv = nullptr; return E_NOINTERFACE; }
            AddRef();
            return S_OK;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
        STDMETHODIMP_(ULONG) Release() override
        {
            ULONG r = InterlockedDecrement(&m_ref);
            if (r == 0) delete this;
            return r;
        }

        STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD dwMask) override
        {
            (void)riid; (void)dwMask;
            if (!punk) return E_INVALIDARG;
            if (m_pSink) { m_pSink->Release(); m_pSink = nullptr; }
            HRESULT hr = punk->QueryInterface(IID_ITextStoreACPSink,
                                              reinterpret_cast<void**>(&m_pSink));
            BridgeLog(L"TextBridge: store AdviseSink hr=%x", hr);
            return hr;
        }
        STDMETHODIMP UnadviseSink(IUnknown* punk) override
        {
            (void)punk;
            if (m_pSink) { m_pSink->Release(); m_pSink = nullptr; }
            return S_OK;
        }

        void NotifyLayoutChange()
        {
            if (m_pSink)
            {
                m_pSink->OnLayoutChange(TS_LC_CHANGE, 0);
            }
        }




        STDMETHODIMP RequestLock(DWORD dwLockFlags, HRESULT* phrSession) override
        {
            BridgeLog(L"TextBridge: store RequestLock flags=%x\n", dwLockFlags);
            if (!phrSession) return E_INVALIDARG;
            if (!m_pSink)
            {
                *phrSession = TF_E_SYNCHRONOUS;
                return S_OK;
            }

            *phrSession = m_pSink->OnLockGranted(dwLockFlags);
            return S_OK;
        }

        STDMETHODIMP GetStatus(TS_STATUS* pdyn) override
        {
            BridgeLog(L"TextBridge: store GetStatus\n");
            if (!pdyn) return E_INVALIDARG;
            memset(pdyn, 0, sizeof(TS_STATUS));
            pdyn->dwStaticFlags = TS_SS_NOHIDDENTEXT;
            return S_OK;
        }

        STDMETHODIMP QueryInsert(long acpTestStart, long acpTestEnd,
                                 ULONG cch, long* pacpResultStart,
                                 long* pacpResultEnd) override
        {
            (void)cch;
            if (acpTestStart < 0 || acpTestEnd > (long)text.size())
                return E_INVALIDARG;
            if (pacpResultStart) *pacpResultStart = acpTestStart;
            if (pacpResultEnd) *pacpResultEnd = acpTestStart + (long)cch;
            return S_OK;
        }

        STDMETHODIMP GetSelection(ULONG ulIndex, ULONG ulCount,
                                  TS_SELECTION_ACP* pSelection,
                                  ULONG* pcFetched) override
        {
            if (!pSelection || !pcFetched) return E_INVALIDARG;
            *pcFetched = 0;
            if (ulIndex != 0 || ulCount < 1) return S_OK;
            pSelection[0].acpStart = selStart;
            pSelection[0].acpEnd = selEnd;
            pSelection[0].style.ase = TS_AE_END;
            pSelection[0].style.fInterimChar = FALSE;
            *pcFetched = 1;
            return S_OK;
        }

        STDMETHODIMP SetSelection(ULONG ulCount,
                                  const TS_SELECTION_ACP* pSelection) override
        {
            BridgeLog(L"TextBridge: [store] SetSelection called\n");
            if (!pSelection || ulCount < 1) return E_INVALIDARG;
            selStart = pSelection[0].acpStart;
            selEnd = pSelection[0].acpEnd;
            return S_OK;
        }

        STDMETHODIMP GetText(LONG acpStart, LONG acpEnd, WCHAR* pchPlain,
                             ULONG cchPlainReq, ULONG* pcchPlainRet,
                             TS_RUNINFO* prgRunInfo, ULONG cRunInfoReq,
                             ULONG* pcRunInfoRet, LONG* pacpNext) override
        {
            BridgeLog(L"TextBridge: store GetText [%ld,%ld) req=%u\n",
                      acpStart, acpEnd, cchPlainReq);
            if (pcchPlainRet) *pcchPlainRet = 0;
            if (pcRunInfoRet) *pcRunInfoRet = 0;
            if (acpStart < 0) acpStart = 0;
            LONG docEnd = (LONG)text.size();
            if (acpEnd == -1 || acpEnd > docEnd) acpEnd = docEnd;
            if (acpStart > acpEnd) acpStart = acpEnd;

            ULONG copy = (ULONG)(acpEnd - acpStart);
            if (cchPlainReq && pchPlain)
            {
                if (copy > cchPlainReq) copy = cchPlainReq;
                memcpy(pchPlain, text.c_str() + acpStart,
                       copy * sizeof(WCHAR));
                if (pcchPlainRet) *pcchPlainRet = copy;
            }
            if (cRunInfoReq && prgRunInfo && copy > 0)
            {
                prgRunInfo[0].uCount = copy;
                prgRunInfo[0].type = TS_RT_PLAIN;
                if (pcRunInfoRet) *pcRunInfoRet = 1;
            }
            if (pacpNext) *pacpNext = acpStart + (long)copy;
            return S_OK;
        }

        STDMETHODIMP SetText(DWORD dwFlags, long acpStart, long acpEnd,
                             const WCHAR* pchText, ULONG cch,
                             TS_TEXTCHANGE* pChange) override
        {
            BridgeLog(L"TextBridge: store SetText [%ld,%ld) cch=%u\n",
                      acpStart, acpEnd, cch);
            (void)dwFlags;
            if (acpStart < 0) acpStart = 0;
            if (acpEnd > (long)text.size()) acpEnd = (long)text.size();
            if (acpStart > acpEnd) return E_INVALIDARG;
            if (!pchText && cch > 0) return E_INVALIDARG;

            text = text.substr(0, acpStart) + std::wstring(pchText, cch) +
                   text.substr(acpEnd);
            inserted.assign(pchText, cch);
            selStart = acpStart + (long)cch;
            selEnd = selStart;
            if (pChange)
            {
                pChange->acpStart = acpStart;
                pChange->acpOldEnd = acpEnd;
                pChange->acpNewEnd = acpStart + (long)cch;
            }
            return S_OK;
        }

        STDMETHODIMP InsertEmbedded(DWORD dwFlags, LONG acpStart, LONG acpEnd,
                                    IDataObject* pDataObject,
                                    TS_TEXTCHANGE* pChange) override
        { (void)dwFlags; (void)acpStart; (void)acpEnd; (void)pDataObject;
          (void)pChange; return E_NOTIMPL; }

        STDMETHODIMP InsertTextAtSelection(DWORD dwFlags, const WCHAR* pchText,
                                           ULONG cch, LONG* pacpStart,
                                           LONG* pacpEnd,
                                           TS_TEXTCHANGE* pChange) override
        {
            (void)dwFlags;
            if (!pchText && cch > 0) return E_INVALIDARG;
            selStart = selEnd;
            text.insert(selEnd, std::wstring(pchText, cch));
            inserted.assign(pchText, cch);
            if (pacpStart) *pacpStart = selEnd;
            selEnd += (long)cch;
            selStart = selEnd;
            if (pacpEnd) *pacpEnd = selEnd;
            if (pChange)
            {
                pChange->acpStart = selEnd - (long)cch;
                pChange->acpOldEnd = pChange->acpStart;
                pChange->acpNewEnd = selEnd;
            }
            return S_OK;
        }

        STDMETHODIMP InsertEmbeddedAtSelection(DWORD dwFlags,
                                               IDataObject* pDataObject,
                                               LONG* pacpStart, LONG* pacpEnd,
                                               TS_TEXTCHANGE* pChange) override
        { (void)dwFlags; (void)pDataObject; if (pacpStart) *pacpStart = 0;
          if (pacpEnd) *pacpEnd = 0; if (pChange) memset(pChange, 0, sizeof(TS_TEXTCHANGE));
          return E_NOTIMPL; }

        STDMETHODIMP RequestAttrsAtPosition(LONG acpPos, ULONG cFilterAttrs,
                                            const TS_ATTRID* paFilterAttrs,
                                            DWORD dwFlags) override
        { (void)acpPos; (void)cFilterAttrs; (void)paFilterAttrs; (void)dwFlags;
          return S_OK; }

        STDMETHODIMP RequestAttrsTransitioningAtPosition(
            LONG acpPos, ULONG cFilterAttrs, const TS_ATTRID* paFilterAttrs,
            DWORD dwFlags) override
        { (void)acpPos; (void)cFilterAttrs; (void)paFilterAttrs; (void)dwFlags;
          return S_OK; }

        STDMETHODIMP FindNextAttrTransition(long acpStart, long acpHalt,
                                            ULONG cFilterAttrs,
                                            const TS_ATTRID* paAttrFilter,
                                            DWORD dwFlags, LONG* pacpNext,
                                            BOOL* pfFound,
                                            LONG* plFoundOffset) override
        {
            (void)acpStart; (void)acpHalt; (void)cFilterAttrs;
            (void)paAttrFilter; (void)dwFlags;
            if (pacpNext) *pacpNext = acpHalt;
            if (pfFound) *pfFound = FALSE;
            if (plFoundOffset) *plFoundOffset = 0;
            return S_OK;
        }

        STDMETHODIMP RequestSupportedAttrs(DWORD dwFlags, ULONG cFilterAttrs,
                                           const TS_ATTRID* paAttrFilter) override
        {
            (void)dwFlags; (void)cFilterAttrs; (void)paAttrFilter;
            return S_OK;
        }

        STDMETHODIMP GetEmbedded(LONG acpPos, const GUID& rguidService,
                                 const IID& riid, IUnknown** ppunk) override
        { (void)acpPos; (void)rguidService; (void)riid; (void)ppunk;
          return E_NOTIMPL; }

        STDMETHODIMP RetrieveRequestedAttrs(ULONG ulCount,
                                            TS_ATTRVAL* paAttrVal,
                                            ULONG* pcFetched) override
        {
            if (pcFetched) *pcFetched = 0;
            (void)ulCount; (void)paAttrVal;
            return S_OK;
        }

        void Reset()
        {
            text.clear();
            selStart = selEnd = 0;
            inserted.clear();
        }



        static RECT RailWindowRect()
        {
            RECT rc = { 0, 0, 0, 0 };
            if (g_railHwnd)
                GetWindowRect(g_railHwnd, &rc);
            return rc;
        }

        STDMETHODIMP GetACPFromPoint(TsViewCookie vcView, const POINT* ptScreen,
                                     DWORD dwFlags, LONG* pacp) override
        {
            (void)vcView; (void)ptScreen; (void)dwFlags;
            if (pacp) *pacp = (LONG)text.size();
            return S_OK;
        }

        STDMETHODIMP GetActiveView(TsViewCookie* pvcView) override
        {
            if (pvcView) *pvcView = 0;
            return S_OK;
        }

        STDMETHODIMP GetEndACP(LONG* pacp) override
        {
            if (pacp) *pacp = (LONG)text.size();
            return S_OK;
        }

        STDMETHODIMP GetFormattedText(LONG acpStart, LONG acpEnd,
                                      IDataObject** ppDataObject) override
        { (void)acpStart; (void)acpEnd; (void)ppDataObject; return E_NOTIMPL; }

        STDMETHODIMP GetScreenExt(TsViewCookie vcView, RECT* prc) override
        {
            BridgeLog(L"TextBridge: IME querying GetScreenExt\n");
            (void)vcView;
            if (!prc) return E_INVALIDARG;
            *prc = RailWindowRect();
            return S_OK;
        }

        STDMETHODIMP GetTextExt(TsViewCookie vcView, LONG acpStart,
                                LONG acpEnd, RECT* prc, BOOL* pfClipped) override
        {
            RECT r;

            (void)vcView; (void)acpStart; (void)acpEnd;

            if (!prc || !pfClipped)
            {
                return E_INVALIDARG;
            }

            r.left = InterlockedCompareExchange(&g_textExtL, 0, 0);
            r.top = InterlockedCompareExchange(&g_textExtT, 0, 0);
            r.right = InterlockedCompareExchange(&g_textExtR, 0, 0);
            r.bottom = InterlockedCompareExchange(&g_textExtB, 0, 0);

            if (r.right <= r.left || r.bottom <= r.top)
            {
                GUITHREADINFO gti;
                bool have = false;

                memset(&gti, 0, sizeof(gti));
                gti.cbSize = sizeof(gti);

                if (g_railHwnd && GetGUIThreadInfo(GetWindowThreadProcessId(g_railHwnd, nullptr), &gti) &&
                    gti.hwndCaret)
                {
                    POINT tl;
                    POINT br;

                    tl.x = gti.rcCaret.left;
                    tl.y = gti.rcCaret.top;
                    br.x = gti.rcCaret.right;
                    br.y = gti.rcCaret.bottom;

                    if (ClientToScreen(gti.hwndCaret, &tl) && ClientToScreen(gti.hwndCaret, &br))
                    {
                        r.left = tl.x;
                        r.top = tl.y;
                        r.right = br.x;
                        r.bottom = br.y;
                        have = true;
                    }
                }

                if (!have)
                {
                    r = RailWindowRect();
                }
            }

            BridgeLog(L"TextBridge: store GetTextExt -> %ld,%ld,%ld,%ld\\n",
                      r.left, r.top, r.right, r.bottom);

            *prc = r;
            *pfClipped = FALSE;

            return S_OK;
        }

        STDMETHODIMP GetWnd(TsViewCookie vcView, HWND* phwnd) override
        {
            (void)vcView;
            if (phwnd) *phwnd = g_railHwnd;
            return S_OK;
        }

        STDMETHODIMP QueryInsertEmbedded(const GUID* pguidService,
                                         const FORMATETC* pFormatEtc,
                                         BOOL* pfInsertable) override
        {
            (void)pguidService; (void)pFormatEtc;
            if (pfInsertable) *pfInsertable = FALSE;
            return S_OK;
        }
    };

    BridgeTextStore* g_textStore = nullptr;




    class BridgeTsfSink final :
        public ITfTextEditSink,
        public ITfContextOwnerCompositionSink
    {
        LONG m_ref = 1;

    public:
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_ITfTextEditSink)
            {
                *ppv = static_cast<ITfTextEditSink*>(this);
            }
            else if (riid == IID_ITfContextOwnerCompositionSink)
            {
                *ppv = static_cast<ITfContextOwnerCompositionSink*>(this);
            }
            else
            {
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            AddRef();
            return S_OK;
        }
                STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
        STDMETHODIMP_(ULONG) Release() override
        {
            ULONG r = InterlockedDecrement(&m_ref);
            if (r == 0) delete this;
            return r;
        }








        STDMETHODIMP OnEndEdit(ITfContext* pic, TfEditCookie ecReadOnly,
                               ITfEditRecord* pEditRecord) override
        {
            BridgeLog(L"TextBridge: [sink] OnEndEdit fired\n");

            if (g_textStore && !g_textStore->inserted.empty())
                g_pendingCommitText = g_textStore->inserted;




            std::wstring changed;
            if (pEditRecord)
            {
                static const DWORD probeFlags[] = { 0, 1, 2, 4, 0x100 };
                const int probeCount =
                    g_gtpuProbed ? 1 : (int)(sizeof(probeFlags) / sizeof(probeFlags[0]));
                for (int pi = 0; pi < probeCount; ++pi)
                {
                    const DWORD flags = g_gtpuProbed ? 0u : probeFlags[pi];
                    IEnumTfRanges* ranges = nullptr;
                    HRESULT hrCh = pEditRecord->GetTextAndPropertyUpdates(
                        flags, nullptr, 0, &ranges);
                    ULONG nRanges = 0;
                    if (SUCCEEDED(hrCh) && ranges)
                    {
                        ITfRange* r = nullptr;
                        ULONG fetched = 0;
                        while (ranges->Next(1, &r, &fetched) == S_OK && fetched == 1)
                        {
                            ++nRanges;
                            wchar_t buf[1024];
                            ULONG got = 0;
                            if (SUCCEEDED(r->GetText(ecReadOnly, 0, buf, 1023, &got)) &&
                                got > 0 && changed.empty())
                            {
                                changed.append(buf, got);
                            }
                            r->Release();
                            r = nullptr;
                        }
                        ranges->Release();
                    }
                    if (!g_gtpuProbed)
                    {
                        BridgeLog(L"TextBridge: GetTextAndPropertyUpdates "
                                  L"flags=%x hr=%x ranges=%u\n",
                                  flags, hrCh, nRanges);
                    }
                }
                g_gtpuProbed = true;
            }

            ITfContextComposition* cc = nullptr;
            HRESULT hr = pic->QueryInterface(IID_ITfContextComposition,
                                             reinterpret_cast<void**>(&cc));
            if (FAILED(hr) || !cc)
                return S_OK;

            IEnumITfCompositionView* en = nullptr;
            hr = cc->EnumCompositions(&en);
            cc->Release();
            if (FAILED(hr) || !en)
                return S_OK;

            BOOL hasComposition = FALSE;
            std::wstring text;
            ITfCompositionView* cv = nullptr;
            ULONG fetched = 0;
            while (en->Next(1, &cv, &fetched) == S_OK && fetched == 1)
            {
                hasComposition = TRUE;
                if (text.empty())
                {
                    ITfRange* range = nullptr;
                    if (SUCCEEDED(cv->GetRange(&range)) && range)
                    {
                        wchar_t buf[1024];
                        ULONG got = 0;
                        if (SUCCEEDED(range->GetText(ecReadOnly, 0, buf,
                                                     1023, &got)))
                            text.assign(buf, got);
                        range->Release();
                    }
                }
                cv->Release();
            }
            en->Release();



            if (!hasComposition && !g_pendingCommitText.empty() &&
                changed.empty())
            {
                changed = g_pendingCommitText;
            }
            g_pendingCommitText.clear();
            if (g_textStore)
            {
                g_textStore->Reset();
            }

            std::wstring docText;
            ReadContextText(ecReadOnly, docText);

            CaptureCompositionState(hasComposition == TRUE, text, docText,
                                    changed.c_str(), changed.size(), L"sink");
            return S_OK;
        }


        STDMETHODIMP OnStartComposition(ITfCompositionView* pComposition,
                                        BOOL* pfAccepted) override
        {
            g_activeCompositionView.copy_from(pComposition);
            BridgeLog(L"TextBridge: composition started");
            if (pfAccepted) { *pfAccepted = TRUE; }
            return S_OK;
        }

        STDMETHODIMP OnUpdateComposition(ITfCompositionView* pComposition,
                                         ITfRange* pRangeNew) override
        {
            (void)pComposition; (void)pRangeNew;
            return S_OK;
        }



        STDMETHODIMP OnEndComposition(ITfCompositionView* pComposition) override
        {
            BridgeLog(L"TextBridge: composition ended (state=%d, last \"%s\")\n",
                      g_compActive ? 1 : 0,
                      EscapeText(g_lastCompositionText).c_str());
            g_activeCompositionView = nullptr;
            (void)pComposition;
            return S_OK;
        }
    };



    bool
    ForwardClientToServer(winrt::array_view<uint8_t const> const& pdu)
    {
        BridgeLog(L"TextBridge: pduForwarder %u bytes\n", (UINT32)pdu.size());
        if (pdu.size() >= 6)
        {

            UINT16 pid = (UINT16)(pdu[8] | ((UINT16)pdu[9] << 8));
            BridgeLog(L"TextBridge: C2S PDU 0x%04x\n", pid);

            if (pid == 0x0200 || pid == 0x0201 || pid == 0x0204 ||
                pid == 0x0205 || pid == 0x0203)
            {
                wchar_t hexbuf[400] = L"";
                const ULONG hn = pdu.size() < 60 ? (ULONG)pdu.size() : 60;
                for (ULONG hi = 0; hi < hn; ++hi)
                {
                    wchar_t tmp[8];
                    swprintf_s(tmp, L"%02x ", (unsigned int)pdu[hi]);
                    wcscat_s(hexbuf, tmp);
                }
                BridgeLog(L"TextBridge: C2S hex: %s\n", hexbuf);
            }
        }
        if (!g_spC2SChannel)
        {
            BridgeLog(L"TextBridge: C2S channel not ready, dropping\n");
            return true;
        }
        HRESULT hr = g_spC2SChannel->Write(
            static_cast<ULONG>(pdu.size()),
            const_cast<BYTE*>(pdu.data()),
            nullptr);
        if (FAILED(hr))
        {
            BridgeLog(L"TextBridge: C2S channel write failed hr=%x\n", hr);
        }
        return true;
    }

    // ------------------------------------------------------------------

    //





    // ------------------------------------------------------------------
    bool
    CaretRectScreen(RECT* out)
    {
        if (!out || InterlockedCompareExchange(&g_textExtValid, 1, 1) == 0)
        {
            return false;
        }
        out->left = InterlockedCompareExchange(&g_textExtL, 0, 0);
        out->top = InterlockedCompareExchange(&g_textExtT, 0, 0);
        out->right = InterlockedCompareExchange(&g_textExtR, 0, 0);
        out->bottom = InterlockedCompareExchange(&g_textExtB, 0, 0);
        if (out->bottom <= out->top)
        {

            out->bottom = out->top + 20;
        }
        if (out->right <= out->left)
        {
            out->right = out->left + 1;
        }
        return true;
    }

    class BridgeContextOwner : public ITfContextOwner
    {
    public:
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv)
            {
                return E_POINTER;
            }
            if (IsEqualIID(riid, IID_IUnknown) ||
                IsEqualIID(riid, IID_ITfContextOwner))
            {
                *ppv = static_cast<ITfContextOwner*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() override
        {
            return (ULONG)InterlockedIncrement(&m_ref);
        }

        STDMETHODIMP_(ULONG) Release() override
        {
            LONG c = InterlockedDecrement(&m_ref);
            if (c == 0)
            {
                delete this;
            }
            return (ULONG)c;
        }


        STDMETHODIMP GetACPFromPoint(const POINT* pt, DWORD flags,
                                     LONG* pacp) override
        {
            (void)pt; (void)flags;
            if (pacp)
            {
                *pacp = 0;
            }
            return S_OK;
        }


        STDMETHODIMP GetTextExt(LONG acpStart, LONG acpEnd, RECT* prc,
                                BOOL* pfClipped) override
        {
            RECT r;
            (void)acpStart; (void)acpEnd;
            if (!prc)
            {
                return E_INVALIDARG;
            }
            if (pfClipped)
            {
                *pfClipped = FALSE;
            }
            if (InterlockedCompareExchange(&g_textExtValid, 1, 1) == 0 ||
                !CaretRectScreen(&r))
            {
                return E_FAIL;
            }
            *prc = r;
            if (g_textExtCalls < 12 || g_cfg.verbose)
            {
                ++g_textExtCalls;
                BridgeLog(L"TextBridge: GetTextExt -> %ld,%ld,%ld,%ld #%d\n",
                          r.left, r.top, r.right, r.bottom, g_textExtCalls);
            }
            return S_OK;
        }

        STDMETHODIMP GetScreenExt(RECT* prc) override
        {
            if (!prc)
            {
                return E_INVALIDARG;
            }
            const HWND app = CurrentAppWindow();

            if (app && GetWindowRect(app, prc))
            {
                return S_OK;
            }
            return E_FAIL;
        }

        STDMETHODIMP GetStatus(TF_STATUS* pdcs) override
        {
            if (!pdcs)
            {
                return E_INVALIDARG;
            }

            pdcs->dwDynamicFlags = 0;
            pdcs->dwStaticFlags = 0;
            return S_OK;
        }

        STDMETHODIMP GetWnd(HWND* phwnd) override
        {
            if (!phwnd)
            {
                return E_INVALIDARG;
            }
            *phwnd = CurrentAppWindow();
            return g_railHwnd ? S_OK : E_FAIL;
        }

        STDMETHODIMP GetAttribute(REFGUID rguid, VARIANT* pvar) override
        {
            (void)rguid;
            if (pvar)
            {
                VariantInit(pvar);
            }
            return E_NOTIMPL;
        }

    private:
        volatile LONG m_ref = 1;
    };

    BridgeContextOwner* g_owner = nullptr;

    void
    AdviseOwnerOnAppContext(const wchar_t* why)
    {
        ITfContext* appCtx = nullptr;
        ITfSource* src = nullptr;
        DWORD cookie = 0;
        HRESULT hrOwn;

        if (g_ownerAdvised || !g_threadMgr || !g_appDocMgr)
        {
            return;
        }
        if (g_appDocMgr == g_docMgr)
        {
            return;
        }
        if (!g_owner)
        {
            g_owner = new BridgeContextOwner();
        }
        if (FAILED(g_appDocMgr->GetTop(&appCtx)) || !appCtx)
        {
            BridgeLog(L"TextBridge: owner advise: no top ctx (%s)\\n", why);
            return;
        }
        if (SUCCEEDED(appCtx->QueryInterface(IID_ITfSource,
                                             reinterpret_cast<void**>(&src))) && src)
        {
            hrOwn = src->AdviseSink(IID_ITfContextOwner,
                                    static_cast<ITfContextOwner*>(g_owner),
                                    &cookie);
            if (SUCCEEDED(hrOwn))
            {
                g_ownerAdvised = true;
            }
            BridgeLog(L"TextBridge: advise owner on app ctx hr=%x (%s)\\n",
                      hrOwn, why);
            src->Release();
        }
        else
        {
            BridgeLog(L"TextBridge: owner advise: no ITfSource (%s)\\n", why);
        }
        appCtx->Release();
    }

    // ------------------------------------------------------------------

    class BridgeThreadMgrSink final : public ITfThreadMgrEventSink
    {
        LONG m_ref = 1;

    public:
        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv)
            {
                return E_POINTER;
            }
            if (riid == IID_IUnknown || riid == IID_ITfThreadMgrEventSink)
            {
                *ppv = static_cast<ITfThreadMgrEventSink*>(this);
            }
            else
            {
                *ppv = nullptr;
                return E_NOINTERFACE;
            }
            AddRef();
            return S_OK;
        }
        STDMETHODIMP_(ULONG) AddRef() override
        {
            return InterlockedIncrement(&m_ref);
        }
        STDMETHODIMP_(ULONG) Release() override
        {
            ULONG r = InterlockedDecrement(&m_ref);

            if (r == 0)
            {
                delete this;
            }
            return r;
        }

        STDMETHODIMP OnInitDocumentMgr(ITfDocumentMgr* pdim) override
        {
            (void)pdim;
            return S_OK;
        }
        STDMETHODIMP OnUninitDocumentMgr(ITfDocumentMgr* pdim) override
        {
            (void)pdim;
            return S_OK;
        }
        STDMETHODIMP OnSetFocus(ITfDocumentMgr* pdimFocus,
                                ITfDocumentMgr* pdimPrevFocus) override
        {
            (void)pdimPrevFocus;
            if (pdimFocus)
            {
                pdimFocus->AddRef();
            }
            if (g_appDocMgr)
            {
                g_appDocMgr->Release();
            }
            g_appDocMgr = pdimFocus;
            BridgeLog(L"TextBridge: [tmsink] OnSetFocus doc=%p (ours=%d)\\n",
                      (void*)pdimFocus, (pdimFocus == g_docMgr) ? 1 : 0);
            return S_OK;
        }
        STDMETHODIMP OnPushContext(ITfContext* pic) override
        {
            (void)pic;
            return S_OK;
        }
        STDMETHODIMP OnPopContext(ITfContext* pic) override
        {
            (void)pic;
            return S_OK;
        }
    };

    // ------------------------------------------------------------------
    void
    ActivateTsfOnCurrentThread(HWND hwnd)
    {
        BridgeLog(L"TextBridge: TSF activate on thread %u, hwnd=%p\n",
                  GetCurrentThreadId(), (void*)hwnd);
        HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        ITfThreadMgr* tm = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_ITfThreadMgr, reinterpret_cast<void**>(&tm));
        if (FAILED(hr) || !tm)
        {
            BridgeLog(L"TextBridge: TF_ThreadMgr create failed hr=%x\n", hr);
            return;
        }


        TfClientId clientId = 0;
        hr = tm->Activate(&clientId);
        if (FAILED(hr))
        {
            BridgeLog(L"TextBridge: TM activate failed hr=%x\n", hr);
            tm->Release();
            return;
        }
        BridgeLog(L"TextBridge: TSF activated, clientId=%u\n", clientId);
        g_clientId = clientId;

        ITfDocumentMgr* doc = nullptr;
        ITfContext* ctx = nullptr;
        hr = tm->CreateDocumentMgr(&doc);
        if (FAILED(hr) || !doc)
        {
            BridgeLog(L"TextBridge: CreateDocumentMgr failed hr=%x\n", hr);
            tm->Release();
            return;
        }
        TfEditCookie ec = 0;
        if (g_cfg.useTextStore && !g_textStore)
        {
            g_textStore = new BridgeTextStore();
        }

        if (g_cfg.useOwner && !g_owner)
        {
            g_owner = new BridgeContextOwner();
        }
        IUnknown* punk = nullptr;
        if (g_cfg.useTextStore && g_textStore)
        {
            punk = static_cast<ITextStoreACP*>(g_textStore);
        }
        else if (g_cfg.useOwner && g_owner)
        {
            punk = static_cast<ITfContextOwner*>(g_owner);
        }

        hr = doc->CreateContext(clientId, 0, punk, &ctx, &ec);
        BridgeLog(L"TextBridge: CreateContext hr=%x (textStore=%d owner=%d)\n",
                  hr, g_cfg.useTextStore ? 1 : 0,
                  (punk && !g_cfg.useTextStore) ? 1 : 0);
        if (FAILED(hr) || !ctx)
        {
            BridgeLog(L"TextBridge: CreateContext failed hr=%x\n", hr);
            doc->Release();
            tm->Release();
            return;
        }
        g_context = ctx;




        if (g_cfg.useSink)
        {
            ITfSource* ctxSource = nullptr;
            hr = ctx->QueryInterface(IID_ITfSource,
                                     reinterpret_cast<void**>(&ctxSource));
            if (SUCCEEDED(hr) && ctxSource)
            {
                ITfTextEditSink* teSink = new BridgeTsfSink();
                DWORD teCookie = 0;
                HRESULT hrAdv = ctxSource->AdviseSink(IID_ITfTextEditSink,
                                                      teSink, &teCookie);
                g_sinkAdvised = SUCCEEDED(hrAdv);
                BridgeLog(L"TextBridge: advise TextEditSink hr=%x\n", hrAdv);
                teSink->Release();
                ctxSource->Release();
            }
        }


        if (g_cfg.useOwner && hwnd)
        {
            ITfDocumentMgr* prev = nullptr;
            HRESULT hrAssoc = tm->AssociateFocus(hwnd, doc, &prev);
            if (prev)
            {
                prev->Release();
            }
            BridgeLog(L"TextBridge: AssociateFocus(hwnd=%p) hr=%x\n",
                      (void*)hwnd, hrAssoc);
        }

        hr = doc->Push(ctx);
        if (SUCCEEDED(hr))
        {
            hr = tm->SetFocus(doc);
        }
        BridgeLog(L"TextBridge: Push/SetFocus(doc) hr=%x\n", hr);

        {
            ITfSource* tmSrc = nullptr;

            if (SUCCEEDED(tm->QueryInterface(IID_ITfSource,
                                             reinterpret_cast<void**>(&tmSrc))) && tmSrc)
            {
                BridgeThreadMgrSink* tms = new BridgeThreadMgrSink();
                DWORD tmCookie = 0;
                HRESULT hrTm = tmSrc->AdviseSink(IID_ITfThreadMgrEventSink,
                                                 tms, &tmCookie);

                BridgeLog(L"TextBridge: advise ThreadMgrEventSink hr=%x\\n", hrTm);
                tms->Release();
                tmSrc->Release();
            }
        }



        hr = tm->QueryInterface(IID_ITfKeystrokeMgr,
                                reinterpret_cast<void**>(&g_keystrokeMgr));
        BridgeLog(L"TextBridge: keystroke mgr hr=%x\n", hr);



        g_threadMgr = tm;
        g_clientId = clientId;
        SetImeKeyboardState(L"activate");






        {
            ITfInputProcessorProfiles* ipp = nullptr;
            ITfInputProcessorProfileMgr* ppm = nullptr;
            HRESULT hrProf = CoCreateInstance(
                CLSID_TF_InputProcessorProfiles, nullptr,
                CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles,
                reinterpret_cast<void**>(&ipp));
            if (SUCCEEDED(hrProf) && ipp)
            {
                hrProf = ipp->QueryInterface(
                    IID_ITfInputProcessorProfileMgr,
                    reinterpret_cast<void**>(&ppm));
                ipp->Release();
            }
            if (SUCCEEDED(hrProf) && ppm)
            {
                TF_INPUTPROCESSORPROFILE prof{};
                HRESULT hrAct = ppm->GetActiveProfile(
                    GUID_TFCAT_TIP_KEYBOARD, &prof);
                if (SUCCEEDED(hrAct))
                {
                    BridgeLog(L"TextBridge: active keyboard profile type=%u "
                              L"langid=%04x clsid=%08x-%04x-%04x "
                              L"guid=%08x-%04x-%04x\n",
                              prof.dwProfileType, prof.langid,
                              (unsigned)prof.clsid.Data1,
                              (unsigned)prof.clsid.Data2,
                              (unsigned)prof.clsid.Data3,
                              (unsigned)prof.guidProfile.Data1,
                              (unsigned)prof.guidProfile.Data2,
                              (unsigned)prof.guidProfile.Data3);
                }
                else
                {
                    BridgeLog(L"TextBridge: GetActiveProfile hr=%x\n", hrAct);
                }
                ppm->Release();
            }
            else
            {
                BridgeLog(L"TextBridge: profile mgr hr=%x\n", hrProf);
            }
        }



        g_docMgr = doc;
        g_tsfThreadId = GetCurrentThreadId();
        g_tsfActivated = true;
        g_winFocused = RailWindowFocused();
        if (hrInit == S_OK)
        {
            CoUninitialize();
        }
    }



    void
    ReactivateTsf()
    {
        if (!g_railHwnd)
        {
            return;
        }

        ++g_reactivations;
        BridgeLog(L"TextBridge: IME not engaging, re-activating TSF (#%d)\n",
                  g_reactivations);

        g_compActive = false;
        g_docValid = false;
        g_lastPreedit.clear();
        g_lastCompositionText.clear();
        g_baseDoc.clear();
        g_compGoneTick = 0;
        g_sinkAdvised = false;

        if (g_context)
        {
            g_context->Release();
            g_context = nullptr;
        }
        if (g_docMgr)
        {
            g_docMgr->Release();
            g_docMgr = nullptr;
        }
        if (g_keystrokeMgr)
        {
            g_keystrokeMgr->Release();
            g_keystrokeMgr = nullptr;
        }
        if (g_threadMgr)
        {
            g_threadMgr->Release();
            g_threadMgr = nullptr;
        }
        g_tsfActivated = false;

        ActivateTsfOnCurrentThread(g_railHwnd);
        ReactivateInputProfile(L"tsf-reactivate");
    }




    class ReadCompSession final :
        public ITfEditSession
    {
        LONG m_ref = 1;

    public:
        std::wstring text;
        std::wstring docText;
        BOOL hasComposition = FALSE;

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_ITfEditSession)
                *ppv = static_cast<ITfEditSession*>(this);
            else { *ppv = nullptr; return E_NOINTERFACE; }
            AddRef();
            return S_OK;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
        STDMETHODIMP_(ULONG) Release() override
        {
            ULONG r = InterlockedDecrement(&m_ref);
            if (r == 0) delete this;
            return r;
        }

        STDMETHODIMP DoEditSession(TfEditCookie ec) override
        {
            if (!g_context) return S_OK;
            ITfContextComposition* cc = nullptr;
            HRESULT hr = g_context->QueryInterface(IID_ITfContextComposition,
                                                   reinterpret_cast<void**>(&cc));
            if (FAILED(hr) || !cc) return S_OK;
            IEnumITfCompositionView* en = nullptr;
            hr = cc->EnumCompositions(&en);
            cc->Release();
            if (FAILED(hr) || !en) return S_OK;
            ITfCompositionView* cv = nullptr;
            ULONG fetched = 0;
            while (en->Next(1, &cv, &fetched) == S_OK && fetched == 1)
            {
                hasComposition = TRUE;
                if (text.empty())
                {
                    ITfRange* range = nullptr;
                    if (SUCCEEDED(cv->GetRange(&range)) && range)
                    {
                        wchar_t buf[1024];
                        ULONG got = 0;
                        if (SUCCEEDED(range->GetText(ec, 0, buf, 1023, &got)))
                            text.assign(buf, got);
                        range->Release();
                    }
                }
                cv->Release();
            }
            en->Release();

            ReadContextText(ec, docText);
            return S_OK;
        }
    };




    void
    PollComposition()
    {
        static DWORD lastPoll = 0;
        DWORD now = GetTickCount();
        if (now - lastPoll < 60)
            return;
        lastPoll = now;

        if (!g_context || !g_clientId)
            return;

        ReadCompSession* sess = new ReadCompSession();
        HRESULT hrSession = S_OK;
        HRESULT hr = g_context->RequestEditSession(
            g_clientId, sess, TF_ES_READ | TF_ES_SYNC, &hrSession);
        if (SUCCEEDED(hr) && SUCCEEDED(hrSession))
        {
            PollCapture(sess->hasComposition == TRUE, sess->text,
                        sess->docText);
        }
        else if (g_cfg.verbose)
        {
            BridgeLog(L"TextBridge: RequestEditSession hr=%x session=%x\n",
                      hr, hrSession);
        }
        sess->Release();
    }

    LRESULT
    CALLBACK
    TsfHookProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code >= HC_ACTION && g_railHwnd)
        {
            MSG* msg = reinterpret_cast<MSG*>(lParam);
            if (msg && msg->hwnd == g_railHwnd)
            {
                if (!g_tsfActivated)
                {
                    g_tsfActivated = true;
                    ActivateTsfOnCurrentThread(g_railHwnd);
                }



                EnsureImeFocus(msg->message == WM_NULL ? L"wakeup" : L"msg");
                ApplyPendingCaret();

                if (g_tsfActivated)
                {
                    PollComposition();
                }

                if (g_cfg.verbose &&
                    (msg->message == WM_KEYDOWN || msg->message == WM_KEYUP ||
                     msg->message == WM_SYSKEYDOWN || msg->message == WM_SYSKEYUP ||
                     msg->message == WM_CHAR))
                {
                    BridgeLog(L"TextBridge: msg=%04x vk=%02x scan=%02x\n",
                              (unsigned)msg->message, (unsigned)msg->wParam,
                              (unsigned)((msg->lParam >> 16) & 0xFF));
                }





                if (msg->message == WM_KEYDOWN && g_tsfActivated &&
                    IsPrintableKey(msg->wParam))
                {
                    if (g_compActive)
                    {
                        g_keyStreak = 0;
                    }
                    else
                    {
                        int imeOpen = -1;
                        int imeConv = -1;
                        ReadImeMode(&imeOpen, &imeConv);
                        if (imeOpen == 1 && imeConv == 1 &&
                            RailWindowFocused())
                        {
                            if (++g_keyStreak >= 5)
                            {
                                g_keyStreak = 0;
                                ReactivateTsf();
                            }
                        }
                        else
                        {
                            g_keyStreak = 0;
                        }
                    }
                }






                if (g_cfg.keyFeed && g_keystrokeMgr && g_tsfActivated &&
                    (msg->message == WM_KEYDOWN ||
                     msg->message == WM_SYSKEYDOWN))
                {
                    BOOL eaten = FALSE;
                    HRESULT hrKey = g_keystrokeMgr->KeyDown(msg->wParam,
                                                            msg->lParam,
                                                            &eaten);
                    if (g_cfg.verbose)
                    {
                        BridgeLog(L"TextBridge: KeyDown vk=%02x hr=%x eaten=%d\n",
                                  (unsigned)msg->wParam, hrKey, eaten ? 1 : 0);
                    }
                    if (SUCCEEDED(hrKey) && eaten)
                    {
                        BridgeLog(L"TextBridge: key eaten vk=%x\n",
                                  (UINT32)msg->wParam);
                        msg->message = WM_NULL;
                        msg->wParam = 0;
                        msg->lParam = 0;
                    }
                }
                else if (g_cfg.keyFeed && g_keystrokeMgr && g_tsfActivated &&
                         (msg->message == WM_KEYUP ||
                          msg->message == WM_SYSKEYUP))
                {
                    BOOL eaten = FALSE;
                    HRESULT hrKey = g_keystrokeMgr->KeyUp(msg->wParam,
                                                          msg->lParam,
                                                          &eaten);
                    if (SUCCEEDED(hrKey) && eaten)
                    {
                        msg->message = WM_NULL;
                        msg->wParam = 0;
                        msg->lParam = 0;
                    }
                }
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // ------------------------------------------------------------------

    //




    // ------------------------------------------------------------------
    HWND g_foundRail = nullptr;

    BOOL
    CALLBACK
    FindRailProc(HWND hwnd, LPARAM lparam)
    {
        DWORD pid = 0;
        wchar_t cls[64] = L"";

        (void)lparam;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd))
        {
            return TRUE;
        }
        GetClassNameW(hwnd, cls, 64);
        if (wcscmp(cls, L"RAIL_WINDOW") != 0)
        {
            return TRUE;
        }
        g_foundRail = hwnd;
        return FALSE;
    }


    void
    ResetCompositionState(const wchar_t* why)
    {
        (void)why;
        g_compActive = false;
        g_docValid = false;
        g_lastPreedit.clear();
        g_lastCompositionText.clear();
        g_baseDoc.clear();
        g_lastLoggedDoc.clear();
        g_compGoneTick = 0;
        g_keyStreak = 0;
        g_caretCreated = false;
        InterlockedExchange(&g_pendingCaret, 0);
    }

    void
    InstallRailHook(DWORD tid)
    {
        HMODULE hMod = nullptr;

        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&TsfHookProc), &hMod);
        g_tsfHook = SetWindowsHookExW(WH_GETMESSAGE, TsfHookProc, hMod, tid);
        BridgeLog(L"TextBridge: hook installed on thread %lu (hook=%p)\n",
                  (unsigned long)tid, (void*)g_tsfHook);
    }

    void
    MaybeSwitchRailWindow()
    {
        HWND cur;
        DWORD oldTid, newTid;

        g_foundRail = nullptr;
        EnumWindows(FindRailProc, 0);
        cur = g_foundRail;

        if (cur == g_railHwnd)
        {
            return;
        }

        if (!cur)
        {
            if (g_railHwnd && !IsWindow(g_railHwnd))
            {
                BridgeLog(L"TextBridge: RAIL window %p gone, IME idle until "
                          L"the next app\n", (void*)g_railHwnd);
                if (g_tsfHook)
                {
                    UnhookWindowsHookEx(g_tsfHook);
                    g_tsfHook = nullptr;
                }
                g_railHwnd = nullptr;
                ResetCompositionState(L"window gone");
            }
            return;
        }

        oldTid = g_railHwnd ? GetWindowThreadProcessId(g_railHwnd, nullptr) : 0;
        newTid = GetWindowThreadProcessId(cur, nullptr);
        BridgeLog(L"TextBridge: RAIL window switch %p(tid %lu) -> %p(tid %lu), "
                  L"tsf thread %lu\n", (void*)g_railHwnd, (unsigned long)oldTid,
                  (void*)cur, (unsigned long)newTid,
                  (unsigned long)g_tsfThreadId);

        if (g_tsfHook)
        {
            UnhookWindowsHookEx(g_tsfHook);
            g_tsfHook = nullptr;
        }

        ResetCompositionState(L"window switch");

        if (newTid != g_tsfThreadId || !g_tsfActivated)
        {

            BridgeLog(L"TextBridge: TSF session invalidated (thread change), "
                      L"clientId was %u\n", g_clientId);
            g_context = nullptr;
            g_docMgr = nullptr;
            g_keystrokeMgr = nullptr;
            g_threadMgr = nullptr;
            g_clientId = 0;
            g_tsfActivated = false;
            g_sinkAdvised = false;
            g_tsfThreadId = 0;
        }

        g_railHwnd = cur;
        g_winFocused = false;

        InstallRailHook(newTid);

        PostMessageW(cur, WM_NULL, 0, 0);
    }

    HWND
    CurrentAppWindow()
    {
        g_foundRail = nullptr;
        EnumWindows(FindRailProc, 0);

        if (g_foundRail)
        {
            return g_foundRail;
        }

        return g_railHwnd;
    }

    // ------------------------------------------------------------------


    // ------------------------------------------------------------------
    struct EnumContext
    {
        DWORD pid;
        std::set<DWORD> tids;
        bool logWindows;
    };

    BOOL
    CALLBACK
    CollectRemoteUiThread(HWND hwnd, LPARAM lParam)
    {
        auto* ctx = reinterpret_cast<EnumContext*>(lParam);
        DWORD pid = 0;
        DWORD tid = GetWindowThreadProcessId(hwnd, &pid);
        if (pid != ctx->pid)
        {
            return TRUE;
        }
        if (ctx->logWindows)
        {
            wchar_t cls[64] = L"";
            GetClassNameW(hwnd, cls, 64);
            BridgeLog(L"  window hwnd=%p tid=%u visible=%d class=%s\n",
                      (void*)hwnd, tid, IsWindowVisible(hwnd) ? 1 : 0, cls);
        }
        if (IsWindowVisible(hwnd))
        {
            ctx->tids.insert(tid);

        }
        return TRUE;
    }

    void
    WatchdogLoop()
    {
        const DWORD pid = GetCurrentProcessId();
        int iteration = 0;

        while (!g_watchdogStop)
        {
            Sleep(100);
            ++iteration;

            if (iteration % 10 == 0)
            {
                LoadConfig();
            }
            if (!g_connection)
            {
                continue;
            }


            {
                static HWND s_lastRail = NULL;
                if (g_railHwnd != s_lastRail)
                {
                    s_lastRail = g_railHwnd;
                    InterlockedExchange(&g_gotGeom, 0);

                    InterlockedExchange(&g_geomCaretR, 0);
                    InterlockedExchange(&g_geomCaretB, 0);
                    InterlockedExchange(&g_textExtValid, 0);
                    InterlockedExchange(&g_textExtL, 0);
                    InterlockedExchange(&g_textExtT, 0);
                    InterlockedExchange(&g_textExtR, 0);
                    InterlockedExchange(&g_textExtB, 0);
                }
            }
            if (g_cfg.syncGeom && g_tsfActivated && !g_gotGeom &&
                iteration % 2 == 0)
            {
                SendPdu(0x0603, std::vector<uint8_t>());
                SendUpdateMode(RDPTXT_FEATURE_PREDICTION_MODE |
                               RDPTXT_FEATURE_LAYOUT_CHANGE_TRACKING |
                               RDPTXT_FEATURE_SELECTION_TRACKING, true);
                BridgeLog(L"TextBridge: [syncgeom] pre-fetch geometry\n");
            }


            if (iteration % 2 == 0)
            {
                MaybeSwitchRailWindow();

            if (iteration % 2 == 0 && g_docMgr && g_railHwnd)
            {
                EnsureImeFocus(L"watchdog");
                if (!g_ownerAdvised)
                {
                    AdviseOwnerOnAppContext(L"watchdog");
                }
            }
            }


            if (iteration % 10 == 0 && g_railHwnd)
            {
                PostMessageW(g_railHwnd, WM_NULL, 0, 0);
            }


            if (iteration % 30 == 0)
            {
                EnumContext ctx{ pid, {}, iteration <= 30 };
                EnumWindows(CollectRemoteUiThread, reinterpret_cast<LPARAM>(&ctx));
                for (DWORD tid : ctx.tids)
                {
                    if (g_registeredThreads.insert(tid).second)
                    {
                        try
                        {
                            g_connection.RegisterThread(tid);
                            BridgeLog(L"TextBridge: RegisterThread(%u)\n", tid);
                        }
                        catch (winrt::hresult_error const& e)
                        {
                            BridgeLog(L"TextBridge: RegisterThread(%u) failed hr=%x\n",
                                      tid, e.code());
                        }
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------------

    // ------------------------------------------------------------------
    struct TextBridgeChannelCallback :
        winrt::implements<TextBridgeChannelCallback, IWTSVirtualChannelCallback>
    {
        explicit TextBridgeChannelCallback(BridgeChannelRole role, IWTSVirtualChannel* pChannel) :
            m_role(role)
        {
            if (m_role == BridgeChannelRole::ClientToServer)
            {
                g_spC2SChannel.copy_from(pChannel);
            }
            else
            {
                g_spS2CChannel.copy_from(pChannel);
            }
            BridgeLog(L"TextBridge: channel connected (role=%d)\n", static_cast<int>(m_role));
        }




        STDMETHODIMP
        OnDataReceived(ULONG cbSize, _In_reads_(cbSize) BYTE* pBuffer)
        {
            if (m_role != BridgeChannelRole::ServerToClient || !g_connection)
                return S_OK;


            //   0x0308 EDIT_CONTROL_FOCUS: clientId4 + controlBounds16 + ...
            //   0x030F GEOMETRY_CHANGED  : clientId4 + controlId4 + bounds16 + ...
            if (cbSize >= 8)
            {
                const uint8_t* p = reinterpret_cast<const uint8_t*>(pBuffer);
                UINT16 pid = (UINT16)(p[4] | ((UINT16)p[5] << 8));

                if (g_cfg.verbose && g_geomPatchLogged < 60 &&
                    (pid == 0x0308 || pid == 0x030F))
                {
                    size_t co = (pid == 0x030F) ? (size_t)(6 + 8) : (size_t)(6 + 4);
                    if (cbSize >= co + 16)
                    {
                        int32_t c[4];
                        for (int k = 0; k < 4; ++k)
                        {
                            c[k] = (int32_t)((uint32_t)p[co + k * 4] |
                                     ((uint32_t)p[co + k * 4 + 1] << 8) |
                                     ((uint32_t)p[co + k * 4 + 2] << 16) |
                                     ((uint32_t)p[co + k * 4 + 3] << 24));
                        }
                        if (pid == 0x030F && cbSize >= 6 + 32 + 16)
                        {
                            int32_t r[4];
                            for (int k = 0; k < 4; ++k)
                            {
                                r[k] = (int32_t)((uint32_t)p[6 + 32 + k * 4] |
                                         ((uint32_t)p[6 + 32 + k * 4 + 1] << 8) |
                                         ((uint32_t)p[6 + 32 + k * 4 + 2] << 16) |
                                         ((uint32_t)p[6 + 32 + k * 4 + 3] << 24));
                            }
                            ++g_geomPatchLogged;
                            BridgeLog(L"TextBridge: [recv] 0x%04x ctrl=%d,%d,%d,%d "
                                      L"range=%d,%d,%d,%d\n", pid, c[0], c[1], c[2], c[3],
                                      r[0], r[1], r[2], r[3]);
                        }
                        else
                        {
                            ++g_geomPatchLogged;
                            BridgeLog(L"TextBridge: [recv] 0x%04x ctrl=%d,%d,%d,%d\n",
                                      pid, c[0], c[1], c[2], c[3]);
                        }
                    }
                }
                {
                    static bool s_modeSubscribed = false;

                    if (!s_modeSubscribed &&
                        (pid == 0x0308 || pid == 0x0309 || pid == 0x030F))
                    {
                        s_modeSubscribed = true;
                        SendUpdateMode(RDPTXT_FEATURE_LAYOUT_CHANGE_TRACKING |
                                       RDPTXT_FEATURE_SELECTION_TRACKING, true);
                        BridgeLog(L"TextBridge: [mode] subscribed LAYOUT+SELECTION tracking\\n");
                    }
                }

                if (pid == 0x0308 && cbSize >= 6 + 20)
                {
                    LONG c[4];

                    for (int i = 0; i < 4; ++i)
                    {
                        const uint8_t* b = p + 6 + 4 + i * 4;

                        c[i] = (LONG)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                                      ((uint32_t)b[2] << 16) |
                                      ((uint32_t)b[3] << 24));
                    }

                    if (c[2] > c[0] && c[3] > c[1])
                    {
                        const LONG cx = (c[0] + c[2]) / 2;
                        const LONG cy = c[3];

                        InterlockedExchange(&g_textExtL, cx - 2);
                        InterlockedExchange(&g_textExtT, cy - 40);
                        InterlockedExchange(&g_textExtR, cx + 2);
                        InterlockedExchange(&g_textExtB, cy);
                        InterlockedExchange(&g_textExtValid, 1);


                        InterlockedExchange(&g_pendingCaretX, cx);
                        InterlockedExchange(&g_pendingCaretY, cy);
                        InterlockedExchange(&g_pendingCaret, 1);
                        BridgeLog(L"TextBridge: [caret] derived %ld,%ld,%ld,%ld from ctrl %ld,%ld,%ld,%ld\\n",
                                  (long)(cx - 2), (long)(cy - 40), (long)(cx + 2), (long)cy,
                                  (long)c[0], (long)c[1], (long)c[2], (long)c[3]);

                        if (g_textStore)
                        {
                            g_textStore->NotifyLayoutChange();
                        }
                    }
                }
                else if (pid == 0x030F && cbSize >= 6 + 24)
                {
                    InterlockedExchange(&g_gotGeom, 1);
                    {
                        LONG q[4];
                        LONG w2[4];

                        for (int i = 0; i < 4; ++i)
                        {
                            const uint8_t* b = p + 6 + 32 + i * 4;
                            const uint8_t* c2 = p + 6 + 8 + i * 4;

                            q[i] = (LONG)((uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                                          ((uint32_t)b[2] << 16) |
                                          ((uint32_t)b[3] << 24));
                            w2[i] = (LONG)((uint32_t)c2[0] | ((uint32_t)c2[1] << 8) |
                                           ((uint32_t)c2[2] << 16) |
                                           ((uint32_t)c2[3] << 24));
                        }

                        BridgeLog(L"TextBridge: [geom-in] range=%ld,%ld,%ld,%ld ctrl=%ld,%ld,%ld,%ld ext=%ld,%ld,%ld,%ld valid=%ld\\n",
                                  (long)q[0], (long)q[1], (long)q[2], (long)q[3],
                                  (long)w2[0], (long)w2[1], (long)w2[2], (long)w2[3],
                                  (long)InterlockedCompareExchange(&g_textExtL, 0, 0),
                                  (long)InterlockedCompareExchange(&g_textExtT, 0, 0),
                                  (long)InterlockedCompareExchange(&g_textExtR, 0, 0),
                                  (long)InterlockedCompareExchange(&g_textExtB, 0, 0),
                                  (long)InterlockedCompareExchange(&g_textExtValid, 0, 0));
                    }

                    StashCaretFromBounds(p + 6 + 32, L"geometry-changed"); /* rangeBounds */

                    BridgeLog(L"TextBridge: [geom-out] ext=%ld,%ld,%ld,%ld valid=%ld\\n",
                              (long)InterlockedCompareExchange(&g_textExtL, 0, 0),
                              (long)InterlockedCompareExchange(&g_textExtT, 0, 0),
                              (long)InterlockedCompareExchange(&g_textExtR, 0, 0),
                              (long)InterlockedCompareExchange(&g_textExtB, 0, 0),
                              (long)InterlockedCompareExchange(&g_textExtValid, 0, 0));

                    if (g_textStore)
                    {
                        g_textStore->NotifyLayoutChange();
                        BridgeLog(L"TextBridge: OnLayoutChange(TS_LC_CHANGE) sent\n");
                    }

                    InterlockedExchange(&g_geomCaretR,
                        InterlockedCompareExchange(&g_textExtR, 0, 0));
                    InterlockedExchange(&g_geomCaretB,
                        InterlockedCompareExchange(&g_textExtB, 0, 0));
                }
                if (g_cfg.verbose)
                {

                    BridgeLog(L"TextBridge: S2C PDU 0x%04x (%u bytes)\n", pid,
                              (unsigned)cbSize);
                }
            }

            try
            {
                std::vector<uint8_t> patched;
                std::vector<uint8_t> guarded;

                if (GuardEmptyControlBounds(
                        reinterpret_cast<const uint8_t*>(pBuffer), cbSize,
                        guarded))
                {
                    if (guarded.empty())
                    {
                        return S_OK;
                    }

                    auto pdu = winrt::com_array<uint8_t>(
                        &guarded[0], &guarded[0] + guarded.size());

                    g_connection.ReportDataReceived(pdu);
                }
                else if (g_cfg.geomFix > 0 &&
                    PatchGeometryBounds(reinterpret_cast<const uint8_t*>(pBuffer),
                                        cbSize, patched))
                {
                    auto pdu = winrt::com_array<uint8_t>(&patched[0],
                                                         &patched[0] + patched.size());
                    g_connection.ReportDataReceived(pdu);
                }
                else
                {
                    auto pdu = winrt::com_array<uint8_t>(pBuffer, pBuffer + cbSize);
                    g_connection.ReportDataReceived(pdu);
                }
            }
            catch (winrt::hresult_error const& e)
            {
                BridgeLog(L"TextBridge: ReportDataReceived failed hr=%x %s\n",
                          e.code(), e.message().c_str());
            }
            return S_OK;
        }

        STDMETHODIMP
        OnClose()
        {
            if (m_role == BridgeChannelRole::ClientToServer)
            {
                g_spC2SChannel = nullptr;
            }
            else
            {
                g_spS2CChannel = nullptr;
            }
            BridgeLog(L"TextBridge: channel closed (role=%d)\n", static_cast<int>(m_role));
            return S_OK;
        }

    private:
        BridgeChannelRole m_role;
    };

    struct TextBridgeListenerCallback :
        winrt::implements<TextBridgeListenerCallback, IWTSListenerCallback>
    {
        explicit TextBridgeListenerCallback(BridgeChannelRole role) :
            m_role(role)
        {
        }

        STDMETHODIMP
        OnNewChannelConnection(
            __RPC__in_opt IWTSVirtualChannel* pChannel,
            __RPC__in_opt BSTR data,
            __RPC__out BOOL* pbAccept,
            __RPC__deref_out_opt IWTSVirtualChannelCallback** ppCallback)
        {
            UNREFERENCED_PARAMETER(data);

            auto callback = winrt::make<TextBridgeChannelCallback>(m_role, pChannel);
            if (!callback)
            {
                *pbAccept = FALSE;
                return E_OUTOFMEMORY;
            }
            *ppCallback = callback.detach();
            *pbAccept = TRUE;
            return S_OK;
        }

    private:
        BridgeChannelRole m_role;
    };
}

HRESULT
TextBridge::Start(_In_ IWTSVirtualChannelManager* pChannelMgr)
{

    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (winrt::hresult_error const&)
    {
    }

    LoadConfig();
    BridgeLog(L"TextBridge: config store=%d keyfeed=%d sink=%d verbose=%d\n",
              g_cfg.useTextStore ? 1 : 0, g_cfg.keyFeed ? 1 : 0,
              g_cfg.useSink ? 1 : 0, g_cfg.verbose ? 1 : 0);

    auto spC2SListener = winrt::make<TextBridgeListenerCallback>(BridgeChannelRole::ClientToServer);
    winrt::com_ptr<IWTSListener> spListener;
    HRESULT hr = pChannelMgr->CreateListener(c_c2sChannelName, 0,
                                             spC2SListener.get(), spListener.put());
    if (FAILED(hr))
    {
        BridgeLog(L"TextBridge: CreateListener(C2S) failed hr=%x\n", hr);
        return hr;
    }
    spListener = nullptr;

    auto spS2CListener = winrt::make<TextBridgeListenerCallback>(BridgeChannelRole::ServerToClient);
    hr = pChannelMgr->CreateListener(c_s2cChannelName, 0,
                                     spS2CListener.get(), spListener.put());
    if (FAILED(hr))
    {
        BridgeLog(L"TextBridge: CreateListener(S2C) failed hr=%x\n", hr);
        return hr;
    }
    spListener = nullptr;



    if (g_cfg.useTextStore && !g_textStore)
        g_textStore = new BridgeTextStore();


    try
    {
        auto handler = RemoteTextConnectionDataHandler(&ForwardClientToServer);
        g_connection = RemoteTextConnection(
            winrt::Windows::Foundation::GuidHelper::CreateNewGuid(),
            handler);
        g_connection.IsEnabled(true);
        BridgeLog(L"TextBridge: RemoteTextConnection created, IsEnabled=%d\n",
                  g_connection.IsEnabled() ? 1 : 0);
    }
    catch (winrt::hresult_error const& e)
    {
        BridgeLog(L"TextBridge: create connection failed hr=%x %s\n",
                  e.code(), e.message().c_str());
        return e.code();
    }


    g_watchdogStop = false;
    g_windowWatchdog = std::thread(WatchdogLoop);

    return S_OK;
}


void
BridgeLog(const wchar_t* format, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buf, _TRUNCATE, format, args);
    va_end(args);

    FILE* f = nullptr;
    if (_wfopen_s(&f, L"C:\\ProgramData\\wsltextbridge.log", L"a") == 0 && f)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fwprintf(f, L"[%02d:%02d:%02d.%03d] %s\n",
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
        fclose(f);
    }
}
