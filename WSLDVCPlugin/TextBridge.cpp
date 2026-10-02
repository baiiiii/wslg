// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
#include "pch.h"
#include "TextBridge.h"
#include "utils.h"

#include <cstdio>
#include <cwchar>
#include <unknwn.h>
#include <set>
#include <thread>
#include <msctf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.RemoteDesktop.Input.h>

using namespace winrt::Windows::System::RemoteDesktop::Input;

// 桥的独立文件日志（msrdc 的 DebugPrint 不落地，现场诊断需要）。
void
BridgeLog(const wchar_t* format, ...);

// RAIL 窗口线程的 TSF 激活：msrdc 在虚拟化未启用时不为 RAIL 窗口
// 激活 TSF（无上下文 → InputService 无从服务）。此处用公开 msctf API
// 补上标准激活序列（ActivateEx → CreateContext → Push → SetFocus），
// 让窗口线程拥有 TSF 输入上下文；InputService（TSF3 集中宿主）随之
// 获得可关联到 RemoteTextConnection 的上下文。
namespace
{
    HWND g_railHwnd = nullptr;
    HHOOK g_tsfHook = nullptr;
    bool g_tsfActivated = false;

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
        ITfThreadMgrEx* tmEx = nullptr;
        hr = tm->QueryInterface(IID_ITfThreadMgrEx, reinterpret_cast<void**>(&tmEx));
        if (SUCCEEDED(hr) && tmEx)
        {
            hr = tmEx->ActivateEx(&clientId, TF_TMF_CONSOLE);
            tmEx->Release();
        }
        else
        {
            hr = tm->Activate(&clientId);
        }
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
        // 压入基础上下文并把焦点设到该文档（线程的 TSF 输入焦点）。
        hr = doc->Push(ctx);
        if (SUCCEEDED(hr))
        {
            hr = tm->SetFocus(doc);
        }
        BridgeLog(L"TextBridge: Push hr=%x SetFocus(doc) hr=%x\n",
                  hr == S_OK ? 0 : hr, hr);

        // 全局引用保持 TSF 对象存活（线程生命周期内不释放）。
        static ITfThreadMgr* s_persistMgr = tm;
        static ITfDocumentMgr* s_persistDoc = doc;
        static ITfContext* s_persistCtx = ctx;
        if (hrInit == S_OK)
        {
            CoUninitialize();
        }
    }

    LRESULT
    CALLBACK
    TsfHookProc(int code, WPARAM wParam, LPARAM lParam)
    {
        if (code >= 0 && !g_tsfActivated && g_railHwnd)
        {
            g_tsfActivated = true;
            ActivateTsfOnCurrentThread(g_railHwnd);
            if (g_tsfHook)
            {
                UnhookWindowsHookEx(g_tsfHook);
                g_tsfHook = nullptr;
            }
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
}

// 桥的独立文件日志（msrdc 的 DebugPrint 不落地，现场诊断需要）。
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

namespace
{
    enum class BridgeChannelRole
    {
        ClientToServer,  // 插件→weston：系统文本输入服务发出的 PDU
        ServerToClient,  // weston→插件：喂给 ReportDataReceived 的 PDU
    };

    // 桥的全局状态：一条 RemoteTextConnection 连接 + 两条通道。
    // 生命周期与插件（msrdc 进程）相同，无需显式清理。
    RemoteTextConnection g_connection{ nullptr };
    winrt::com_ptr<IWTSVirtualChannel> g_spC2SChannel;
    winrt::com_ptr<IWTSVirtualChannel> g_spS2CChannel;

    // RegisterThread 看门狗：InputService 只对"已注册线程上的前台窗口"
    // 做 IME 路由（API 语义："registers a thread on which the client
    // will present remote UI"）。RAIL 窗口由 msrdc 的窗口线程动态创建，
    // 因此周期性枚举本进程的可见顶层窗口，把新出现的窗口线程注册进
    // 连接，保证前台 RAIL 窗口始终属于已注册线程。
    std::thread g_windowWatchdog;
    bool g_watchdogStop = false;
    std::set<DWORD> g_registeredThreads;

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
            wchar_t title[96] = L"";
            GetClassNameW(hwnd, cls, 64);
            GetWindowTextW(hwnd, title, 96);
            BridgeLog(L"  window hwnd=%p tid=%u visible=%d class=%s title=%.60s",
                      (void*)hwnd, tid, IsWindowVisible(hwnd) ? 1 : 0, cls, title);
        }
        if (IsWindowVisible(hwnd))
        {
            ctx->tids.insert(tid);
            // 发现 RAIL 远程应用窗口：在该线程挂一次性消息钩子，
            // 借钩子执行 TSF 激活序列（TSF 绑定窗口线程）。
            wchar_t cls[64] = L"";
            GetClassNameW(hwnd, cls, 64);
            if (wcscmp(cls, L"RAIL_WINDOW") == 0 && !g_tsfActivated && !g_tsfHook)
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
                // 前几轮打印窗口明细，确认 RegisterThread 覆盖了前台
                // RAIL 窗口所在线程。
                EnumContext ctx{ pid, {}, iteration < 3 };
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
                Sleep(100);  // 3 秒轮询，休眠粒度保证快速退出
            }
        }
    }

    // RemoteTextConnectionDataHandler：系统文本输入服务产生的每条
    // client→server PDU 在此回调，原样写往 weston 的 C2S 通道。
    // 返回 true 表示数据已消费。
    bool
    ForwardClientToServer(winrt::array_view<uint8_t const> const& pdu)
    {
        BridgeLog(L"TextBridge: pduForwarder %u bytes\n", static_cast<ULONG>(pdu.size()));
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

    // 通道数据回调：S2C 通道收到 weston 的 PDU 后喂给系统文本输入服务。
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

        // 只有 S2C 通道会收到 weston 的数据；C2S 通道的数据方向相反。
        STDMETHODIMP
        OnDataReceived(
            ULONG cbSize,
            _In_reads_(cbSize) BYTE* pBuffer)
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

    // Listener：接受 weston（服务端）发起的通道创建请求。
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
    // WinRT 调用要求线程已初始化 apartment；若 msrdc 已按 STA 初始化
    // （RPC_E_CHANGED_MODE），保持原样即可，WinRT 对象默认 agile。
    try
    {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }
    catch (winrt::hresult_error const&)
    {
        // 线程已被初始化为其他 apartment 模型，直接沿用。
    }

    auto spC2SListener = winrt::make<TextBridgeListenerCallback>(BridgeChannelRole::ClientToServer);
    winrt::com_ptr<IWTSListener> spListener;
    HRESULT hr = pChannelMgr->CreateListener("WSL::TextBridge::ClientToServer", 0,
                                             spC2SListener.get(), spListener.put());
    if (FAILED(hr))
    {
        BridgeLog(L"TextBridge: CreateListener(ClientToServer) failed hr=%x\n", hr);
        return hr;
    }
    spListener = nullptr;

    auto spS2CListener = winrt::make<TextBridgeListenerCallback>(BridgeChannelRole::ServerToClient);
    hr = pChannelMgr->CreateListener("WSL::TextBridge::ServerToClient", 0,
                                     spS2CListener.get(), spListener.put());
    if (FAILED(hr))
    {
        BridgeLog(L"TextBridge: CreateListener(ServerToClient) failed hr=%x\n", hr);
        return hr;
    }
    spListener = nullptr;

    // 创建 RemoteTextConnection 并启用文本输入虚拟化。
    // connectionId 使用随机 GUID（规格未规定来源，官方客户端同样随机）。
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

    // 启动窗口线程注册看门狗（见上方说明）。
    g_watchdogStop = false;
    g_windowWatchdog = std::thread(WatchdogLoop);

    return S_OK;
}
