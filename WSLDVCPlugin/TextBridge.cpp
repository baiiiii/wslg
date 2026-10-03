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
    winrt::com_ptr<ITfCompositionView> g_activeCompositionView;
    std::wstring g_lastCompositionText;   // OnEndEdit 缓存的组合串（提交用）
    BOOL g_sinkAdvised = FALSE;           // TextEditSink 挂接成功标志
    std::wstring g_pendingCommitText;     // IME 插入文本存储的最终文本（提交用）
    TfClientId g_clientId = 0;
    ITfThreadMgr* g_threadMgr = nullptr;
    ITfDocumentMgr* g_docMgr = nullptr;
    ITfKeystrokeMgr* g_keystrokeMgr = nullptr;   // 按键路由（IME 收键的通道）
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
    // TSF 事件桥：IME 组合/确认文本写入激活的上下文，桥读取后转为 PDU。
    // （签名按 msctf.h 实际定义）
    // ------------------------------------------------------------------
    // ------------------------------------------------------------------
    // 文本存储（ITextStoreACP，官方 TSF 应用开发标准模式）：给 IME 一个
    // 可写的"文档"。没有它 IME 只能组合（候选窗可用）但无法把选中的
    // 最终文本写入——选字后文本丢失。存储内容就是一个 wstring；IME 经
    // 编辑会话（RequestLock 同步授权）读写。每次提交后清空，保证下次
    // 组合从干净状态开始。
    // ------------------------------------------------------------------
    class BridgeTextStore final :
        public ITextStoreACP
    {
        LONG m_ref = 1;
        ITextStoreACPSink* m_pSink = nullptr;

    public:
        std::wstring text;              // 文档内容
        LONG selStart = 0, selEnd = 0;  // 选区（ACP 偏移）
        std::wstring inserted;          // 最近一次编辑会话写入的文本

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

        // TSF 编辑锁 sink：RequestLock 同步授权时必须调用
        // ITextStoreACPSink::OnLockGranted 触发待决编辑会话——
        // 此前只回 S_OK 不调回调，IME 的编辑会话永远不执行，
        // 导致挂存储后 IME 无法组合（候选窗消失）。
        STDMETHODIMP AdviseSink(REFIID riid, IUnknown* punk, DWORD dwMask) override
        {
            (void)riid; (void)dwMask;
            if (!punk) return E_INVALIDARG;
            m_pSink = nullptr;
            HRESULT hr = punk->QueryInterface(IID_ITextStoreACPSink,
                                              reinterpret_cast<void**>(&m_pSink));
            return hr;
        }
        STDMETHODIMP UnadviseSink(IUnknown* punk) override
        {
            (void)punk;
            if (m_pSink) { m_pSink->Release(); m_pSink = nullptr; }
            return S_OK;
        }

        STDMETHODIMP RequestLock(DWORD dwLockFlags, HRESULT* phrSession) override
        {
            if (!phrSession) return E_INVALIDARG;
            if (!m_pSink) { *phrSession = TF_E_SYNCHRONOUS; return S_OK; }
            // 同步授权：OnLockGranted 返回值即会话执行结果
            *phrSession = m_pSink->OnLockGranted(dwLockFlags);
            return S_OK;
        }

        STDMETHODIMP GetStatus(TS_STATUS* pdyn) override
        {
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
            if (ulIndex != 0 || ulCount < 1) return S_OK;   // 仅支持默认选区
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

        // 屏幕几何：IME 用 GetTextExt/GetScreenExt 定位候选窗。返回
        // RAIL 窗口的屏幕矩形（组合字符串位近似）。
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
            BridgeLog(L"TextBridge: IME querying GetTextExt\n");
            (void)vcView; (void)acpStart; (void)acpEnd;
            if (!prc || !pfClipped) return E_INVALIDARG;
            *prc = RailWindowRect();
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

    BridgeTextStore* g_textStore = nullptr;   // 全局存储（传给 CreateContext）

    // TSF 事件 sink：手写 IUnknown（官方 TSF 示例的标准做法；
    // winrt::implements 对经典 COM 接口的 QI 不被 AdviseSink 接受，
    // 返回 CONNECT_E_CANNOTCONNECT）。
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

        // ITfTextEditSink：每次编辑会话结束后调用（IME 的组合/提交
        // 都通过编辑会话写入本上下文）。组合生命周期 sink 无法挂接
        // （CONNECT_E_CANNOTCONNECT），因此这里自足式处理：枚举上下文
        // 活动组合——存在则发 preedit（UPDATE_COMPOSITION）；消失则把
        // 最后组合文本作为提交发出（UPDATE_TEXT）并清 preedit。
        STDMETHODIMP OnEndEdit(ITfContext* pic, TfEditCookie ecReadOnly,
                               ITfEditRecord* pEditRecord) override
        {
            BridgeLog(L"TextBridge: OnEndEdit fired");
            // vacant 上下文（无存储）：用 pEditRecord 取本会话变更文本
            // ——组合期间即新组合串；选字确认时若 IME 成功写入最终中文，
            // 变更文本即中文（否则为空，退回组合串缓存）。
            std::wstring changedText;
            {
                IEnumTfRanges* chEnum = nullptr;
                HRESULT hrCh = pEditRecord->GetTextAndPropertyUpdates(
                    0, nullptr, 0, &chEnum);
                if (SUCCEEDED(hrCh) && chEnum)
                {
                    ITfRange* chRange = nullptr;
                    ULONG fetchedCh = 0;
                    while (chEnum->Next(1, &chRange, &fetchedCh) == S_OK &&
                           fetchedCh == 1)
                    {
                        if (changedText.empty())
                        {
                            wchar_t buf[1024];
                            ULONG got = 0;
                            if (SUCCEEDED(chRange->GetText(ecReadOnly, 0,
                                                           buf, 1023, &got)))
                                changedText.assign(buf, got);
                        }
                        chRange->Release();
                    }
                    chEnum->Release();
                }
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

            if (hasComposition)
            {
                // 组合活跃：优先变更文本，退回组合串
                const std::wstring& preedit =
                    !changedText.empty() ? changedText : text;
                g_lastCompositionText = preedit;
                SendComposition(preedit, 2);   // UPDATE preedit
                BridgeLog(L"TextBridge: preedit len=%u",
                          (UINT32)preedit.size());
                g_pendingCommitText.clear();   // 组合仍活跃，提交文本未定
            }
            else if (!changedText.empty() || !g_lastCompositionText.empty())
            {
                // 组合结束（选字确认）：优先变更文本（选中的最终中文），
                // 退回最后组合串。提交后清空。
                const std::wstring& commit = !changedText.empty()
                    ? changedText : g_lastCompositionText;
                BridgeLog(L"TextBridge: commit len=%u\n",
                          (UINT32)commit.size());
                SendUpdateText(commit);
                SendComposition(L"", 3);   // LEAVE 清 preedit
                g_lastCompositionText.clear();
                g_pendingCommitText.clear();
                if (g_textStore)
                    g_textStore->Reset();   // 清空存储，下次组合干净开始
            }
            return S_OK;
        }

        // ITfContextOwnerCompositionSink：组合生命周期。
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

        // 组合结束（选字确认）：提交 OnEndEdit 缓存的最终组合串。
        STDMETHODIMP OnEndComposition(ITfCompositionView* pComposition) override
        {
            BridgeLog(L"TextBridge: composition ended");
            if (pComposition && !g_lastCompositionText.empty())
            {
                BridgeLog(L"TextBridge: commit len=%u", (UINT32)g_lastCompositionText.size());
                SendUpdateText(g_lastCompositionText);
                SendComposition(L"", 3);   // LEAVE 清 preedit
            }
            g_lastCompositionText.clear();
            g_activeCompositionView = nullptr;
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
        hr = doc->CreateContext(clientId, 0, g_textStore, &ctx, &ec);
        if (FAILED(hr) || !ctx)
        {
            BridgeLog(L"TextBridge: CreateContext failed hr=%x\n", hr);
            doc->Release();
            tm->Release();
            return;
        }
        g_context = ctx;

        // 组合文本读取改由 TsfHookProc 内的同步只读编辑会话轮询完成
        // （见下方），不再挂接 sink——挂接的 TextEditSink 与文本存储
        // 是"出候选配置"中没有的两个变量，属候选窗消失的嫌疑项，
        // 先全部摘除以回归已验证基线。

        hr = doc->Push(ctx);
        if (SUCCEEDED(hr))
        {
            hr = tm->SetFocus(doc);
        }
        BridgeLog(L"TextBridge: Push/SetFocus(doc) hr=%x\n", hr);

        // 挂接 TextEditSink（手写 QI）：OnEndEdit 驱动组合文本捕获。
        // 实测单独挂接与候选窗是否出现的关系待验证——文本存储已确认
        // 破坏 IME 参与，sink 单独从未测过。
        ITfSource* ctxSource = nullptr;
        hr = ctx->QueryInterface(IID_ITfSource, reinterpret_cast<void**>(&ctxSource));
        if (SUCCEEDED(hr) && ctxSource)
        {
            ITfTextEditSink* teSink = new BridgeTsfSink();
            DWORD teCookie = 0;
            HRESULT hrAdv = ctxSource->AdviseSink(IID_ITfTextEditSink,
                                                  teSink, &teCookie);
            BridgeLog(L"TextBridge: advise TextEditSink hr=%x\n", hrAdv);
            if (SUCCEEDED(hrAdv)) { g_sinkAdvised = TRUE; }
            teSink->Release();
            ctxSource->Release();
        }

        // 激活微软拼音配置档：TSF Activate 默认档很可能是英文键盘，
        // 不显式激活中文 IME 则 IME 的按键 sink 不消费任何键。
        // CLSID/Profile 取自客户端 UPDATE_INPUT_PROFILE PDU（实测捕获）。
        {
            ITfInputProcessorProfiles* profiles = nullptr;
            HRESULT hrProf = CoCreateInstance(CLSID_TF_InputProcessorProfiles,
                                              nullptr, CLSCTX_INPROC_SERVER,
                                              IID_ITfInputProcessorProfiles,
                                              reinterpret_cast<void**>(&profiles));
            if (SUCCEEDED(hrProf) && profiles)
            {
                // 新版接口：支持 TF_IPP_ 标志位（旧接口 E_FAIL 时备用）
                ITfInputProcessorProfileMgr* mgr = nullptr;
                hrProf = profiles->QueryInterface(IID_ITfInputProcessorProfileMgr,
                                                  reinterpret_cast<void**>(&mgr));
                if (SUCCEEDED(hrProf) && mgr)
                {
                    CLSID clsid = {};
                    GUID profile = {};
                    CLSIDFromString(L"{81D4E9C9-1D3B-41BC-9E6C-4B40BF79E35E}", &clsid);
                    CLSIDFromString(L"{FA550B04-5AD7-411F-A5AC-CA038EC515D7}", &profile);
                    hrProf = mgr->ActivateProfile(
                        TF_PROFILETYPE_INPUTPROCESSOR, 0x0804, clsid, profile,
                        nullptr, 0);
                    BridgeLog(L"TextBridge: mgr ActivateProfile(MS Pinyin) hr=%x\n",
                              hrProf);
                    mgr->Release();
                }
                else
                {
                    CLSID clsid = {};
                    CLSIDFromString(L"{81D4E9C9-1D3B-41BC-9E6C-4B40BF79E35E}", &clsid);
                    hrProf = profiles->ActivateLanguageProfile(
                        GUID_TFCAT_TIP_KEYBOARD, 0x0804, clsid);
                    BridgeLog(L"TextBridge: ActivateLanguageProfile(MS Pinyin) hr=%x\n",
                              hrProf);
                }
                profiles->Release();
            }
        }

        // 按键路由管理器：把 RAIL 窗口的按键消息喂给 TSF（IME 组合的
        // 前提——没有它按键直达应用，IME 收不到键、永远不组合）。
        hr = tm->QueryInterface(IID_ITfKeystrokeMgr,
                                reinterpret_cast<void**>(&g_keystrokeMgr));
        BridgeLog(L"TextBridge: keystroke mgr hr=%x\n", hr);

        // 打开键盘 compartment（IME 按键处理的前提——新激活的线程
        // 默认关闭；关闭时 IME 不消费任何按键）。
        ITfCompartmentMgr* cm = nullptr;
        hr = tm->QueryInterface(IID_ITfCompartmentMgr,
                                reinterpret_cast<void**>(&cm));
        if (SUCCEEDED(hr) && cm)
        {
            ITfCompartment* pOpen = nullptr;
            hr = cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_OPENCLOSE,
                                    &pOpen);
            if (SUCCEEDED(hr) && pOpen)
            {
                VARIANT v;
                VariantInit(&v);
                v.vt = VT_I4;
                v.lVal = 1;   // keyboard open
                hr = pOpen->SetValue(clientId, &v);
                BridgeLog(L"TextBridge: keyboard open hr=%x\n", hr);
                pOpen->Release();
            }
            // IME 转换模式：native（中文）
            ITfCompartment* pConv = nullptr;
            hr = cm->GetCompartment(GUID_COMPARTMENT_KEYBOARD_INPUTMODE_CONVERSION,
                                    &pConv);
            if (SUCCEEDED(hr) && pConv)
            {
                VARIANT v2;
                VariantInit(&v2);
                v2.vt = VT_I4;
                v2.lVal = 1;   // native（中文）
                pConv->SetValue(clientId, &v2);
                pConv->Release();
            }
            cm->Release();
        }

        // 保持 TSF 对象存活；WM_SETFOCUS 时重绑文档焦点。
        g_threadMgr = tm;
        g_docMgr = doc;
        if (hrInit == S_OK)
        {
            CoUninitialize();
        }
    }

    // 同步只读编辑会话：枚举上下文活动组合并读取文本（绕开无法
    // 挂接的组合生命周期 sink；组合文本存在组合对象里，vacant
    // 上下文也能读）。
    class ReadCompSession final :
        public ITfEditSession
    {
        LONG m_ref = 1;

    public:
        std::wstring text;          // 活动组合文本（无组合时为空）
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
            return S_OK;
        }
    };

    // 组合状态推进：同步只读会话读取活动组合——存在则发 preedit
    // （UPDATE_COMPOSITION），消失则提交缓存文本（UPDATE_TEXT）并清
    // preedit。在 RAIL 窗口线程上调用（同步会话要求所属线程）。
    void
    PollComposition()
    {
        static DWORD lastPoll = 0;
        DWORD now = GetTickCount();
        if (now - lastPoll < 120)   // 节流 ~8 次/秒
            return;
        lastPoll = now;

        // sink 已挂接时由 OnEndEdit 驱动，轮询停用（避免双发）
        if (g_sinkAdvised)
            return;

        if (!g_context || !g_clientId)
            return;

        ReadCompSession* sess = new ReadCompSession();
        HRESULT hrSession = S_OK;
        HRESULT hr = g_context->RequestEditSession(
            g_clientId, sess, TF_ES_READ | TF_ES_SYNC, &hrSession);
        if (SUCCEEDED(hr) && SUCCEEDED(hrSession))
        {
            // 捕获 IME 写入存储的文本：选字确认时 IME 用最终中文覆写
            // 组合区间（写入文本存储），提交时应使用它而非拼音缓存。
            if (g_textStore && !g_textStore->inserted.empty())
                g_pendingCommitText = g_textStore->inserted;

            if (sess->hasComposition)
            {
                g_lastCompositionText = sess->text;
                SendComposition(sess->text, 2);   // UPDATE preedit
                BridgeLog(L"TextBridge: poll preedit len=%u",
                          (UINT32)sess->text.size());
                g_pendingCommitText.clear();   // 组合仍活跃，提交文本未定
            }
            else if (!g_lastCompositionText.empty() ||
                     !g_pendingCommitText.empty())
            {
                // 组合结束（选字确认）：提交 IME 写入存储的最终中文；
                // 存储无写入时退回拼音缓存。提交后清空存储保证下次
                // 组合从干净状态开始。
                const std::wstring& commit = !g_pendingCommitText.empty()
                    ? g_pendingCommitText : g_lastCompositionText;
                BridgeLog(L"TextBridge: poll commit len=%u",
                          (UINT32)commit.size());
                SendUpdateText(commit);
                SendComposition(L"", 3);   // LEAVE 清 preedit
                g_lastCompositionText.clear();
                g_pendingCommitText.clear();
                if (g_textStore)
                    g_textStore->Reset();
            }
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
                // RAIL 窗口获得键盘焦点时重绑 TSF 文档焦点（msrdc 未
                // 启用虚拟化，不会自己调 SetFocus）。
                if (g_threadMgr && g_docMgr && msg->message == WM_SETFOCUS)
                {
                    HRESULT hr = g_threadMgr->SetFocus(g_docMgr);
                    BridgeLog(L"TextBridge: refocus on WM_SETFOCUS hr=%x\n", hr);
                }
                // 组合轮询：读活动组合 → preedit；组合消失 → 提交
                if (g_tsfActivated)
                {
                    PollComposition();
                }
                // 按键路由：把按键喂给 TSF。IME 消费（组合中/功能键）
                // 则吞掉消息，应用收不到；未消费则放行（普通字符键）。
                if (g_keystrokeMgr && g_tsfActivated &&
                    (msg->message == WM_KEYDOWN ||
                     msg->message == WM_SYSKEYDOWN))
                {
                    BOOL eaten = FALSE;
                    HRESULT hrKey = g_keystrokeMgr->KeyDown(msg->wParam,
                                                            msg->lParam,
                                                            &eaten);
                    BridgeLog(L"TextBridge: KeyDown vk=%x hr=%x eaten=%d\n",
                              (UINT32)msg->wParam, (UINT32)hrKey,
                              eaten ? 1 : 0);
                    if (SUCCEEDED(hrKey) && eaten)
                    {
                        BridgeLog(L"TextBridge: key eaten vk=%x\n",
                                  (UINT32)msg->wParam);
                        msg->message = WM_NULL;
                        msg->wParam = 0;
                        msg->lParam = 0;
                    }
                }
                else if (g_keystrokeMgr && g_tsfActivated &&
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
                // RegisterThread 已禁用：实测线程注册后 InputService
                // 会把该线程标记为远程 UI 线程并抑制本地 IME（等待永不
                // 到来的远程组合驱动），候选窗消失。出候选依赖的是
                // 本地 TSF 激活（RAIL_WINDOW 钩子），注册反而有害。
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

        // 只有 S2C 通道会收到 weston 的数据：完整转发给 InputService
        // （最终组合拳：配合 rdclientax 按键拦截补丁，InputService 的
        // 远程模式 IME 消费按键后产生的 UPDATE_COMPOSITION/UPDATE_TEXT
        // 经本桥到达 weston；服务器侧 UPDATE_MODE 应答已实现）。
        STDMETHODIMP
        OnDataReceived(ULONG cbSize, _In_reads_(cbSize) BYTE* pBuffer)
        {
            if (m_role != BridgeChannelRole::ServerToClient || !g_connection)
                return S_OK;

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

    // 文本存储：交给 CreateContext，使 IME 能把选中的最终文本写入。
    if (!g_textStore)
        g_textStore = new BridgeTextStore();

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

// 桥的独立文件日志实现（追加到 msrdc 可读的固定路径，现场诊断用）。
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
