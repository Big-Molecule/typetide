//
//  Hooks.h — 共享的低级鼠标/键盘钩子（多个订阅者复用一个钩子）。
//
//  钩子装在独立的钩子线程上，回调在该线程触发，必须极快返回：
//  它只把事件投递给 dispatch 窗口，真正的工作在主线程上做。
//  这样主线程忙于渲染时，全系统的鼠标输入不会被卡住。
//
//  用法：进程启动时调用 SetDispatchWindow(App 窗口)；
//  键盘需要"吞键"时用 SetSwallowKey 注册一个原子开关，钩子线程读它决定吞不吞，
//  动作本身通过 postMessage 回到主线程执行。
//
#pragma once
#include <windows.h>
#include <functional>
#include <atomic>

namespace hooks {

/// 鼠标事件（wParam = WM_LBUTTONDOWN/WM_LBUTTONUP/WM_RBUTTONDOWN…，pt = 屏幕坐标）
using MouseFn = std::function<void(WPARAM event, POINT pt)>;
int AddMouse(MouseFn fn);
void RemoveMouse(int id);

/// 键盘按下事件；返回 true 表示吞掉该键。
/// 在主线程上调用，可以安全地做 UI 操作（关闭弹窗等）。
using KeyFn = std::function<bool(DWORD vk)>;
int AddKey(KeyFn fn);
void RemoveKey(int id);

/// 钩子事件投递的目标窗口（主线程创建）；必须先于 Add* 调用。
void SetDispatchWindow(HWND hwnd);
/// 钩子线程 → dispatch 窗口的鼠标事件消息（lParam = MouseEvent*，接收方释放）
UINT MouseMessage();
/// 钩子线程 → dispatch 窗口的键盘动作消息（wParam = id，lParam = vk）
UINT KeyMessage();

/// 让钩子线程同步决定是否吞掉某个键：enabled 非空且为 true 时，按下 vk 会被吞掉，
/// 并把 KeyMessage 连同订阅 id 投给 dispatch 窗口，由该订阅者的回调执行动作。
/// 传 enabled=nullptr 取消。这是唯一在钩子线程上读取的状态，必须保持无锁。
void SetSwallowKey(DWORD vk, int subscription, const std::atomic<bool>* enabled);

/// 在 dispatch 窗口的消息处理里调用，把排队的事件派发给订阅者（主线程）。
void DispatchMouse(WPARAM wp, LPARAM lp);
void DispatchKey(WPARAM wp, LPARAM lp);

/// 关闭钩子线程（退出时）。
void Shutdown();

} // namespace hooks
