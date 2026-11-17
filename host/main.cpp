// host/main.cpp — WebView2 宿主骨架（切片 1：最小可编译框架）。
//
// 现阶段只建一个 Win32 窗口 + 消息循环，能空跑；WebView2 嵌入、
// bridge 接线、Vue dist 加载都在后续切片里做，锚点见 TODO(bridge)。
// 本文件只用 windows.h，不依赖 WebView2 SDK，所以 llvm-mingw 交叉
// 编译和用户机器的 MSVC 都能直接编过。
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

namespace {
constexpr wchar_t kClassName[] = L"CdcMainWindow";
constexpr wchar_t kWindowTitle[] = L"C盘清理工具";
constexpr int kWidth = 1040;
constexpr int kHeight = 680;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        // TODO(bridge): WM_SIZE 时同步 WebView2 控件尺寸。
        // TODO(bridge): 自定义标题栏按钮消息（frameless 时）在这里处理。
        default:
            return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE /*prev*/, PWSTR /*cmdline*/,
                    int show) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0) return 1;

    HWND hwnd = CreateWindowExW(0, kClassName, kWindowTitle,
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                kWidth, kHeight, nullptr, nullptr, instance, nullptr);
    if (hwnd == nullptr) return 1;
    // TODO(bridge): 在这里创建 WebView2 环境并导航到
    // file://.../web/dist/index.html，注册 chrome.webview.postMessage
    // 回调，接 bridge.{c,h} 的命令分发。
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
