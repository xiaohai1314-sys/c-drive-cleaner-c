// cdc_host.cpp — WebView2 宿主外壳（切片 5）。
//
// 职责：建窗口 → 起 WebView2 → 把 dist/ 以 https://cdc.local/ 映射出去 →
// 接桥（bridge.cpp）→ 跑消息循环。业务命令的注册不在此处：
// src/main.c（GUI 入口）在调 cdc_host_run() 之前先完成后端初始化并
// cdc_bridge_register() 全部后端命令；window_* 三个宿主命令由本文件注册。
//
// 为什么不用 file:// 直接加载 dist/index.html：
// Vite 产物是 ES module 脚本，Chromium 会以 CORS 为由拦截 file:// 源的
// 模块加载（实测报错 "blocked by CORS policy"），页面会白屏。
// 标准做法就是虚拟域名 + WebResourceRequested 回本地目录取文件
// （与 Tauri 的 asset 协议同一路数），所以这里用 https://cdc.local/。

// 全文件按 Unicode 编译（IDC_ARROW 等宏才展开为宽字符版）。
// 必须在任何 windows.h 的 include 之前（bridge.h 里就包含了它）。
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "bridge.h"

#include <WebView2.h>

#include <shlwapi.h>
#include <string.h>
#include <wctype.h>
#include <windows.h>
#include <windowsx.h>

#include <new>
#include <string>

#ifdef _MSC_VER
#pragma comment(lib, "shlwapi.lib")
#endif

namespace {

// ---------------------------------------------------------------------------
// 全局状态（宿主单窗口，够用）
// ---------------------------------------------------------------------------
HWND g_hwnd = nullptr;
ICoreWebView2Environment *g_env = nullptr;
ICoreWebView2Controller *g_controller = nullptr;
ICoreWebView2 *g_webview = nullptr;
std::wstring g_dist_dir;  // dist/ 的绝对路径（末尾带 '\'）。

void resize_webview();  // 定义在后（ControllerHandler 里调用）。

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
std::wstring exe_dir() {
  wchar_t buf[MAX_PATH];
  DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) return L".\\";
  std::wstring path(buf, len);
  size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? L".\\" : path.substr(0, slash + 1);
}

std::wstring local_app_data() {
  wchar_t buf[MAX_PATH];
  DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) return exe_dir();
  return std::wstring(buf, len) + L"\\";
}

const wchar_t *content_type_for(const std::wstring &path) {
  size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring::npos) return L"application/octet-stream";
  std::wstring ext = path.substr(dot);
  for (auto &ch : ext) ch = (wchar_t)towlower(ch);
  if (ext == L".html" || ext == L".htm") return L"text/html";
  if (ext == L".js" || ext == L".mjs") return L"text/javascript";
  if (ext == L".css") return L"text/css";
  if (ext == L".json") return L"application/json";
  if (ext == L".png") return L"image/png";
  if (ext == L".jpg" || ext == L".jpeg") return L"image/jpeg";
  if (ext == L".gif") return L"image/gif";
  if (ext == L".svg") return L"image/svg+xml";
  if (ext == L".ico") return L"image/x-icon";
  if (ext == L".webp") return L"image/webp";
  if (ext == L".woff") return L"font/woff";
  if (ext == L".woff2") return L"font/woff2";
  if (ext == L".ttf") return L"font/ttf";
  return L"application/octet-stream";
}

// "%20" 这类转义只做最小处理（Vite 资源名是 ASCII，空格是唯一常见情况）。
std::wstring url_decode_path(const std::wstring &in) {
  std::wstring out;
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == L'%' && i + 2 < in.size()) {
      auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
      };
      int hi = hex(in[i + 1]);
      int lo = hex(in[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back((wchar_t)((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(in[i]);
  }
  return out;
}

// ---------------------------------------------------------------------------
// COM 回调：把 https://cdc.local/* 映射到 dist/ 目录。
// ---------------------------------------------------------------------------
class ResourceHandler final
    : public ICoreWebView2WebResourceRequestedEventHandler {
 public:
  ResourceHandler() : ref_(1) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == __uuidof(ICoreWebView2WebResourceRequestedEventHandler)) {
      *ppv = static_cast<ICoreWebView2WebResourceRequestedEventHandler *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = --ref_;
    if (count == 0) delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE Invoke(
      ICoreWebView2 *sender,
      ICoreWebView2WebResourceRequestedEventArgs *args) override {
    (void)sender;
    if (!args || !g_env) return S_OK;
    ICoreWebView2WebResourceRequest *request = nullptr;
    if (FAILED(args->get_Request(&request)) || !request) return S_OK;
    LPWSTR uri = nullptr;
    HRESULT hr = request->get_Uri(&uri);
    if (SUCCEEDED(hr) && uri) {
      serve(uri, args);
      CoTaskMemFree(uri);
    }
    request->Release();
    return S_OK;
  }

 private:
  void serve(const std::wstring &uri,
             ICoreWebView2WebResourceRequestedEventArgs *args) {
    static const std::wstring kPrefix = L"https://cdc.local/";
    if (uri.rfind(kPrefix, 0) != 0) return;  // 不是本站请求，不管。
    std::wstring rel = uri.substr(kPrefix.size());
    size_t q = rel.find_first_of(L"?#");
    if (q != std::wstring::npos) rel = rel.substr(0, q);
    if (rel.empty() || rel.back() == L'/') rel += L"index.html";
    rel = url_decode_path(rel);
    for (auto &ch : rel) {
      if (ch == L'/') ch = L'\\';
    }
    // 拒绝越出 dist/ 的路径（".."、绝对路径）。
    if (rel.find(L"..") != std::wstring::npos ||
        (rel.size() >= 2 && rel[1] == L':')) {
      respond(args, 403, L"Forbidden", L"text/plain", nullptr);
      return;
    }
    std::wstring full = g_dist_dir + rel;
    IStream *stream = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(full.c_str(), STGM_READ,
                                        FILE_SHARE_READ, FALSE, nullptr, &stream);
    if (FAILED(hr) || !stream) {
      respond(args, 404, L"Not Found", L"text/plain", nullptr);
      return;
    }
    std::wstring headers = L"Content-Type: ";
    headers += content_type_for(full);
    headers += L"\r\n";
    ICoreWebView2WebResourceResponse *response = nullptr;
    hr = g_env->CreateWebResourceResponse(stream, 200, L"OK", headers.c_str(),
                                          &response);
    stream->Release();
    if (SUCCEEDED(hr) && response) {
      args->put_Response(response);
      response->Release();
    }
  }

  void respond(ICoreWebView2WebResourceRequestedEventArgs *args, int status,
               const wchar_t *reason, const wchar_t *content_type,
               IStream *stream) {
    std::wstring headers = L"Content-Type: ";
    headers += content_type;
    headers += L"\r\n";
    ICoreWebView2WebResourceResponse *response = nullptr;
    if (SUCCEEDED(g_env->CreateWebResourceResponse(stream, status, reason,
                                                   headers.c_str(), &response)) &&
        response) {
      args->put_Response(response);
      response->Release();
    }
  }

  ULONG ref_;
};

// ---------------------------------------------------------------------------
// COM 回调：环境 / 控制器创建完成。
// ---------------------------------------------------------------------------
class ControllerHandler final
    : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
 public:
  ControllerHandler() : ref_(1) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == __uuidof(
            ICoreWebView2CreateCoreWebView2ControllerCompletedHandler)) {
      *ppv = static_cast<
          ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = --ref_;
    if (count == 0) delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT result,
                                   ICoreWebView2Controller *controller) override {
    if (FAILED(result) || !controller) return S_OK;
    g_controller = controller;
    g_controller->AddRef();
    if (FAILED(g_controller->get_CoreWebView2(&g_webview)) || !g_webview) {
      return S_OK;
    }
    // 资源映射：https://cdc.local/* -> dist/。
    g_webview->AddWebResourceRequestedFilter(
        L"https://cdc.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    auto *resource = new (std::nothrow) ResourceHandler();
    if (resource) {
      g_webview->add_WebResourceRequested(resource, nullptr);
      resource->Release();
    }
    // 前端消息桥。
    auto *handler = cdc_bridge_create_message_handler(g_webview, g_hwnd);
    if (handler) {
      g_webview->add_WebMessageReceived(handler, nullptr);
      handler->Release();
    }
    resize_webview();
    g_webview->Navigate(L"https://cdc.local/index.html");
    return S_OK;
  }

 private:
  ULONG ref_;
};

class EnvironmentHandler final
    : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
 public:
  EnvironmentHandler() : ref_(1) {}
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown ||
        riid == __uuidof(
            ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler)) {
      *ppv = static_cast<
          ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG count = --ref_;
    if (count == 0) delete this;
    return count;
  }
  HRESULT STDMETHODCALLTYPE Invoke(
      HRESULT result, ICoreWebView2Environment *env) override {
    if (FAILED(result) || !env) return S_OK;
    g_env = env;
    g_env->AddRef();
    auto *controller = new (std::nothrow) ControllerHandler();
    if (controller) {
      g_env->CreateCoreWebView2Controller(g_hwnd, controller);
      controller->Release();
    }
    return S_OK;
  }

 private:
  ULONG ref_;
};

void resize_webview() {
  if (!g_controller) return;
  RECT bounds;
  GetClientRect(g_hwnd, &bounds);
  g_controller->put_Bounds(bounds);
}

// ---------------------------------------------------------------------------
// 宿主命令 window_minimize / window_maximize / window_close。
// ---------------------------------------------------------------------------
int window_minimize(const char *args_json, char **out_json, char **out_error) {
  (void)args_json;
  (void)out_error;
  ShowWindow(g_hwnd, SW_MINIMIZE);
  *out_json = _strdup("null");
  return *out_json ? 0 : -1;
}

int window_maximize(const char *args_json, char **out_json, char **out_error) {
  (void)args_json;
  (void)out_error;
  ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
  *out_json = _strdup("null");
  return *out_json ? 0 : -1;
}

int window_close(const char *args_json, char **out_json, char **out_error) {
  (void)args_json;
  (void)out_error;
  PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
  *out_json = _strdup("null");
  return *out_json ? 0 : -1;
}

// ---------------------------------------------------------------------------
// 窗口过程：无边框（对齐 Tauri decorations:false）+ 顶栏拖动。
// ---------------------------------------------------------------------------
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_SIZE:
      resize_webview();
      return 0;
    case WM_CDC_BRIDGE_REPLY:
      cdc_bridge_flush_replies(g_webview);
      return 0;
    case WM_NCHITTEST: {
      // 顶部 40px 是 Vue 的 WindowBar 区域，当标题栏拖动。
      // （WebView2 内的具体按钮位置宿主不可见，近似处理。）
      LRESULT hit = DefWindowProcW(hwnd, msg, wparam, lparam);
      if (hit == HTCLIENT) {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd, &pt);
        if (pt.y >= 0 && pt.y < 40) return HTCAPTION;
      }
      return hit;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

// GUI 入口：main.c 在调本函数前完成后端初始化 + 命令注册。
// 返回进程退出码。
extern "C" int cdc_host_run(HINSTANCE instance, int cmd_show) {
  g_dist_dir = exe_dir() + L"dist\\";

  cdc_bridge_register("window_minimize", window_minimize);
  cdc_bridge_register("window_maximize", window_maximize);
  cdc_bridge_register("window_close", window_close);

  const wchar_t kClass[] = L"CdcCleanerHost";
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  wc.lpszClassName = kClass;
  if (!RegisterClassExW(&wc)) return 1;

  // 无边框 + 可调尺寸（对齐 Tauri decorations:false + 1040x680）。
  g_hwnd = CreateWindowExW(
      0, kClass, L"C盘清理工具",
      WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
          WS_SYSMENU,
      CW_USEDEFAULT, CW_USEDEFAULT, 1040, 680, nullptr, nullptr, instance,
      nullptr);
  if (!g_hwnd) return 1;
  ShowWindow(g_hwnd, cmd_show);
  UpdateWindow(g_hwnd);

  std::wstring user_data = local_app_data() + L"C盘清理工具\\webview2";
  auto *env_handler = new (std::nothrow) EnvironmentHandler();
  if (!env_handler) return 1;
  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, user_data.c_str(), nullptr, env_handler);
  env_handler->Release();
  if (FAILED(hr)) return 1;

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  if (g_webview) g_webview->Release();
  if (g_controller) g_controller->Release();
  if (g_env) g_env->Release();
  return (int)msg.wParam;
}
