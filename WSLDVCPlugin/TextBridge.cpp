// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
// WSLg 文本输入桥：实现 MS-RDPETXT 规格所述"扩展 DLL"的角色——在
// msrdc 进程内把 Windows 系统文本输入服务（TSF3 IME 宿主）与 WSLg
// RDP 服务端（weston rdptext.c）对接，使宿主机输入法（微软拼音等）
// 可为远程编辑控件工作。
//
// 组成：
//   1. RemoteTextConnection（公开 WinRT API），IsEnabled=true，
//      经 pduForwarder/ReportDataReceived 与 InputService 双向转发 PDU；
//   2. 自定义 DVC 通道对 WSL::TextBridge::{ClientToServer,ServerToClient}
//      （TextInput_*DVC 已被 msrdc 内置 remotetextplugin 占用）；
//   3. RAIL 窗口线程的 RegisterThread 与 TSF 激活（msrdc 在虚拟化
//      未启用时不为 RAIL 窗口激活 TSF，需公开 msctf API 补齐）；
//   4. TSF 文本/组合监听：IME 写入上下文的组合串/确认文本由桥读取，
//      转为 UPDATE_COMPOSITION（preedit）/ UPDATE_TEXT（提交）PDU。
#include "pch.h"
#include "TextBridge.h"
#include "utils.h"

#include <cstdio>
#include <cwchar>
#include <set>
#include <thread>
#include <unknwn.h>
#include <msctf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.RemoteDesktop.Input.h>

using namespace winrt::Windows::System::RemoteDesktop::Input;

// 桥的独立文件日志（msrdc 的 DebugPrint 不落地，现场诊断需要）。
void
BridgeLog(const wchar_t* format, ...);

namespace
{
    constexpr char c_c2sChannelName[] = "WSL::TextBridge::ClientToServer";
    constexpr char c_s2cChannelName[] = "WSL::TextBridge::ServerToClient";

    constexpr UINT32 PDU_CLIENT_ID = 1;    // 与 weston 侧注册的 id 一致
    constexpr UINT32 PDU_CONTROL_ID = 1;
    constexpr UINT32 PDU_HOST_ID = 1;

    enum class BridgeChannelRole
    {
        ClientToServer,  // 插件→weston：InputService 发出的 PDU
        ServerToClient,  // weston→插件：喂给 ReportDataReceived 的 PDU
    };

    // ------------------------------------------------------------------
    // 全局状态（生命周期 = msrdc 进程，无需显式清理）
    // ------------------------------------------------------------------
    RemoteTextConnection g_connection{ nullptr };
    winrt::com_ptr<IWTSVirtualChannel> g_spC2SChannel;
    winrt::com_ptr<IWTSVirtualChannel> g_spS2CChannel;
    ITfContext* g_context = nullptr;
    winrt::com_ptr<ITfComposition> g_activeComposition;
    ITfThreadMgr* g_threadMgr = nullptr;
    ITfDocumentMgr* g_docMgr = nullptr;
    HWND g_railHwnd = nullptr;
    HHOOK g_tsfHook = nullptr;
    bool g_tsfActivated = false;
    bool g_watchdogStop = false;
    std::thread g_windowWatchdog;
    std::set<DWORD> g_registeredThreads;
    uint32_t g_pduOpId = 0;

    // ------------------------------------------------------------------
    // C2S PDU 发送（外层长度前缀 + 6 字节头 + payload，与 C2S 通道
    // 现有帧格式一致；weston 的 pump 会剥离外层前缀）
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

    // UPDATE_COMPOSITION（组合串 → weston 的 preedit），MS-RDPETXT 2.2.2.10。
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

    // UPDATE_TEXT（确认文本 → weston 的 commit），MS-RDPETXT 2.2.2.9。
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

    // 读取组合范围文本。
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
    // TSF 事件桥：IME 组合/确认文本写入激活的上下文，读取后转为 PDU。
    // ------------------------------------------------------------------
    struct BridgeTsfSink :
        winrt::implements<BridgeTsfSink, ITfTextEditSink, ITfContextOwnerCompositionSink>
    {
        STDMETHODIMP
        OnTextChange(TfEditCookie ec, ITfRange* pChange)
        {
            UNREFERENCED_PARAMETER(pChange);
            if (g_activeComposition)
            {
                auto text = ReadCompositionText(ec, g_activeComposition.get());
                SendComposition(text, 2);   // UPDATE
            }
            return S_OK;
        }

        STDMETHODIMP
        OnStartComposition(TfEditCookie ecWrite, ITfComposition* pComposition)
        {
            g_activeComposition.copy_from(pComposition);
            BridgeLog(L"TextBridge: composition started\n");
            return S_OK;
        }

        STDMETHODIMP
        OnUpdateComposition(TfEditCookie ecWrite, ITfComposition* pComposition)
        {
            UNREFERENCED_PARAMETER(ecWrite);
            UNREFERENCED_PARAMETER(pComposition);
            return S_OK;
        }

        STDMETHODIMP
        OnEndComposition(TfEditCookie ecWrite, ITfComposition* pComposition)
        {
            auto text = ReadCompositionText(ecWrite, pComposition);
            BridgeLog(L"TextBridge: composition ended len=%u\n", (UINT32)text.size());
            if (!text.empty())
            {
                SendUpdateText(text);
            }
            SendComposition(L"", 3);        // LEAVE：清 weston 的 preedit
            g_activeComposition = nullptr;
            return S_OK;
        }
    };

    // RemoteTextConnectionDataHandler：InputService 产生的每条 C2S PDU
    // 经此回调，原样写往 weston 的 C2S 通道。返回 true 表示已消费。
    bool
    ForwardClientToServer(winrt::array_view<uint8_t const> const& pdu)
    {
        BridgeLog(L"TextBridge: pduForwarder %u bytes\n", (UINT32)pdu.size());
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
    // RAIL 窗口线程的 TSF 激活（公开 msctf API）
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

        // 普通 GUI 模式激活（TF_TMF_CONSOLE 会走控制台路径）。
        TfClientId clientId = 0;
        hr = tm->Activate(&clientId);
        if (FAILED(hr))
        {
            BridgeLog(L"TextBridge: TM activate failed hr=%x\n", hr);
            tm->Release();
            return;
        }
        BridgeLog(L"TextBridge: TSF activated, clientId=%u\n", clientId);

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
        hr = doc->CreateContext(clientId, 0, nullptr, &ctx, &ec);
        if (FAILED(hr) || !ctx)
        {
            BridgeLog(L"TextBridge: CreateContext failed hr=%x\n", hr);
            doc->Release();
            tm->Release();
            return;
        }
        g_context = ctx;

        // 监听文本变化与组合生命周期。
        ITfSource* ctxSource = nullptr;
        hr = ctx->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&ctxSource));
        if (SUCCEEDED(hr) && ctxSource)
        {
            auto sink = winrt::make<BridgeTsfSink>();
            auto teSink = sink.as<ITfTextEditSink>();
            DWORD teCookie = 0;
            HRESULT hrAdv = ctxSource->AdviseSink(IID_ITfTextEditSink,
                                                  teSink.get(), &teCookie);
            BridgeLog(L"TextBridge: advise TextEditSink hr=%x\n", hrAdv);
            auto ocSink = sink.as<ITfContextOwnerCompositionSink>();
            DWORD ocCookie = 0;
            hrAdv = ctxSource->AdviseSink(IID_ITfContextOwnerCompositionSink,
                                          ocSink.get(), &ocCookie);
            BridgeLog(L"TextBridge: advise ContextOwnerCompositionSink hr=%x\n", hrAdv);
            ctxSource->Release();
        }

        hr = doc->Push(ctx);
        if (SUCCEEDED(hr))
        {
            hr = tm->SetFocus(doc);
        }
        BridgeLog(L"TextBridge: Push/SetFocus(doc) hr=%x\n", hr);

        // 保持 TSF 对象存活；WM_SETFOCUS 时重绑文档焦点。
        g_threadMgr = tm;
        g_docMgr = doc;
        if (hrInit == S_OK)
        {
            CoUninitialize();
        }
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
                // RAIL 窗口获得键盘焦点时重绑 TSF 文档焦点（msrdc 未
                // 启用虚拟化，不会自己调 SetFocus）。
                if (g_threadMgr && g_docMgr && msg->message == WM_SETFOCUS)
                {
                    HRESULT hr = g_threadMgr->SetFocus(g_docMgr);
                    BridgeLog(L"TextBridge: refocus on WM_SETFOCUS hr=%x\n", hr);
                }
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // ------------------------------------------------------------------
    // 窗口线程注册看门狗：InputService 只对已注册线程上的前台窗口做
    // IME 路由。RAIL 窗口动态创建，周期枚举并注册新线程。
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
            // RAIL 窗口：挂一次性线程钩子执行 TSF 激活。
            if (!g_tsfActivated && !g_tsfHook)
            {
                wchar_t cls[64] = L"";
                GetClassNameW(hwnd, cls, 64);
                if (wcscmp(cls, L"RAIL_WINDOW") == 0)
                {
                    g_railHwnd = hwnd;
                    HMODULE hMod = nullptr;
                    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       reinterpret_cast<LPCWSTR>(&TsfHookProc), &hMod);
                    g_tsfHook = SetWindowsHookExW(WH_GETMESSAGE, TsfHookProc, hMod, tid);
                    BridgeLog(L"TextBridge: RAIL_WINDOW hwnd=%p tid=%u, tsf hook=%p\n",
                              (void*)hwnd, tid, (void*)g_tsfHook);
                }
            }
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
            if (g_connection)
            {
                EnumContext ctx{ pid, {}, iteration < 2 };
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
                ++iteration;
            }
            for (int i = 0; i < 30 && !g_watchdogStop; ++i)
            {
                Sleep(100);
            }
        }
    }

    // ------------------------------------------------------------------
    // DVC 通道回调/监听
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

        // 只有 S2C 通道会收到 weston 的数据。
        STDMETHODIMP
        OnDataReceived(ULONG cbSize, _In_reads_(cbSize) BYTE* pBuffer)
        {
            if (m_role == BridgeChannelRole::ServerToClient && g_connection)
            {
                try
                {
                    auto pdu = winrt::com_array<uint8_t>(pBuffer, pBuffer + cbSize);
                    g_connection.ReportDataReceived(pdu);
                }
                catch (winrt::hresult_error const& e)
                {
                    BridgeLog(L"TextBridge: ReportDataReceived failed hr=%x %s\n",
                              e.code(), e.message().c_str());
                }
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
    // WinRT apartment：若线程已被初始化为其他模型则沿用（对象 agile）。
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (winrt::hresult_error const&)
    {
    }

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

    // 创建 RemoteTextConnection 并启用文本输入虚拟化。
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

    // 窗口线程注册 + TSF 激活看门狗。
    g_watchdogStop = false;
    g_windowWatchdog = std::thread(WatchdogLoop);

    return S_OK;
}
