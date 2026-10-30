#include "Hooks.h"
#include <map>
#include <mutex>
#include <vector>
#include <thread>
#include <atomic>

namespace hooks {

namespace {

constexpr UINT kMouseMessage = WM_APP + 20;
constexpr UINT kKeyMessage = WM_APP + 21;
constexpr size_t kMaxQueuedMouseEvents = 64;
constexpr int kMaxSwallowSlots = 4;

// ---------- 订阅者（只在主线程增删/调用）----------

std::map<int, MouseFn> g_mouse;
std::map<int, KeyFn> g_key;
int g_nextId = 1;

// ---------- 钩子线程 ----------

HHOOK g_mouseHook = nullptr;
HHOOK g_keyHook = nullptr;
DWORD g_hookThreadId = 0;
HANDLE g_hookThread = nullptr;
HANDLE g_hookReady = nullptr;
std::atomic<bool> g_hookThreadFailed{false};
std::atomic<bool> g_shouldExit{false};

std::atomic<HWND> g_dispatch{nullptr};

// ---------- 事件队列（钩子线程写，主线程读）----------

struct MouseEvent {
    WPARAM event;
    POINT pt;
};

std::mutex g_queueMutex;
std::vector<MouseEvent> g_queue;
std::atomic<bool> g_dispatchPosted{false};

// ---------- 吞键注册（钩子线程读，必须无锁）----------

struct SwallowSlot {
    std::atomic<DWORD> vk{0};
    std::atomic<int> subscription{0};
    std::atomic<const std::atomic<bool>*> enabled{nullptr};
};

SwallowSlot g_swallow[kMaxSwallowSlots];

bool shouldPostMouse(WPARAM event) {
    switch (event) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return true;
    default:
        return false;  // 忽略移动等高频事件：每个像素都投递会把主线程淹掉
    }
}

void postMouse(WPARAM event, POINT pt) {
    HWND target = g_dispatch.load();
    if (!target) return;
    {
        std::lock_guard lock(g_queueMutex);
        if (g_queue.size() >= kMaxQueuedMouseEvents) return;
        g_queue.push_back({event, pt});
    }
    // 一次只挂一条消息：主线程处理时会把整个队列一次性取走
    if (!g_dispatchPosted.exchange(true)) {
        if (!PostMessageW(target, kMouseMessage, 0, 0)) {
            g_dispatchPosted.store(false);
            std::lock_guard lock(g_queueMutex);
            g_queue.clear();
        }
    }
}

LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam) {
    // 这里运行在钩子线程，任何阻塞都会拖住全系统的鼠标输入
    if (code == HC_ACTION && shouldPostMouse(wParam)) {
        auto* info = (MSLLHOOKSTRUCT*)lParam;
        postMouse(wParam, info->pt);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK keyProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)) {
        auto* info = (KBDLLHOOKSTRUCT*)lParam;
        for (auto& slot : g_swallow) {
            const std::atomic<bool>* gate = slot.enabled.load();
            if (!gate || slot.vk.load() != info->vkCode) continue;
            if (!gate->load()) continue;
            HWND target = g_dispatch.load();
            if (!target) continue;
            const int subscription = slot.subscription.load();
            if (PostMessageW(target, kKeyMessage, (WPARAM)subscription, (LPARAM)info->vkCode))
                return 1;  // 吞掉
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

/// 钩子线程：安装钩子后跑一个普通消息循环，只为活着并接收 WM_QUIT。
void hookThread() {
    g_hookThreadId = GetCurrentThreadId();
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, GetModuleHandleW(nullptr), 0);
    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyProc, GetModuleHandleW(nullptr), 0);
    g_hookThreadFailed.store(!g_mouseHook || !g_keyHook);
    SetEvent(g_hookReady);
    if (g_hookThreadFailed.load()) return;

    MSG msg;
    while (!g_shouldExit.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
    if (g_keyHook) { UnhookWindowsHookEx(g_keyHook); g_keyHook = nullptr; }
    g_hookThreadId = 0;
}

/// 确保钩子线程在跑；返回是否可用。
bool ensureThread() {
    if (g_hookThread) {
        if (WaitForSingleObject(g_hookThread, 0) == WAIT_TIMEOUT) return !g_hookThreadFailed.load();
        // 线程意外退出：收掉句柄，允许下面重新拉起
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
        g_hookThreadId = 0;
    }
    if (g_hookThreadFailed.load()) return false;

    g_hookReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_hookReady) return false;
    g_shouldExit.store(false);
    g_hookThread = CreateThread(nullptr, 0, [](void*) -> DWORD {
        hookThread();
        return 0;
    }, nullptr, 0, nullptr);
    if (!g_hookThread) {
        CloseHandle(g_hookReady);
        g_hookReady = nullptr;
        return false;
    }
    WaitForSingleObject(g_hookReady, 5000);
    CloseHandle(g_hookReady);
    g_hookReady = nullptr;
    return !g_hookThreadFailed.load();
}

} // namespace

void SetDispatchWindow(HWND hwnd) {
    g_dispatch.store(hwnd);
}

UINT MouseMessage() { return kMouseMessage; }
UINT KeyMessage() { return kKeyMessage; }

int AddMouse(MouseFn fn) {
    int id = g_nextId++;
    g_mouse[id] = std::move(fn);
    ensureThread();
    return id;
}

void RemoveMouse(int id) {
    g_mouse.erase(id);
}

int AddKey(KeyFn fn) {
    int id = g_nextId++;
    g_key[id] = std::move(fn);
    ensureThread();
    return id;
}

void RemoveKey(int id) {
    g_key.erase(id);
    for (auto& slot : g_swallow) {
        if (slot.subscription.load() == id) {
            slot.enabled.store(nullptr);
            slot.subscription.store(0);
            slot.vk.store(0);
        }
    }
}

void SetSwallowKey(DWORD vk, int subscription, const std::atomic<bool>* enabled) {
    if (!enabled) {
        for (auto& slot : g_swallow) {
            slot.enabled.store(nullptr);
            slot.subscription.store(0);
            slot.vk.store(0);
        }
        return;
    }
    for (auto& slot : g_swallow) {
        const std::atomic<bool>* current = slot.enabled.load();
        if (current == nullptr || current == enabled) {
            slot.subscription.store(subscription);
            slot.vk.store(vk);
            slot.enabled.store(enabled);
            return;
        }
    }
}

void DispatchMouse(WPARAM, LPARAM) {
    std::vector<MouseEvent> batch;
    {
        std::lock_guard lock(g_queueMutex);
        batch.swap(g_queue);
        g_dispatchPosted.store(false);
    }
    if (batch.empty() || g_mouse.empty()) return;
    // 快照订阅者，允许回调中增删
    auto subs = g_mouse;
    for (const auto& event : batch)
        for (auto& [id, fn] : subs) fn(event.event, event.pt);
}

void DispatchKey(WPARAM wp, LPARAM lp) {
    auto it = g_key.find((int)wp);
    if (it == g_key.end()) return;
    KeyFn fn = it->second;  // 拷贝：回调里可能 RemoveKey 自己
    fn((DWORD)lp);
}

void Shutdown() {
    g_mouse.clear();
    g_key.clear();
    if (g_hookThread) {
        g_shouldExit.store(true);
        if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, 2000);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }
}

} // namespace hooks
