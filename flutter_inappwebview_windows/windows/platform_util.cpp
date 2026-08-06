#include <ctime>
#include <nlohmann/json.hpp>
#include <Shlwapi.h>
#include <winrt/base.h>
#include <wrl/event.h>

#include "in_app_webview/in_app_webview_manager.h"
#include "platform_util.h"
#include "types/callbacks_complete.h"
#include "utils/flutter.h"
#include "utils/log.h"

namespace flutter_inappwebview_plugin
{
  using namespace Microsoft::WRL;

  PlatformUtil::PlatformUtil(const FlutterInappwebviewWindowsPlugin* plugin)
    : plugin(plugin), ChannelDelegate(plugin->registrar->messenger(), PlatformUtil::METHOD_CHANNEL_NAME_PREFIX)
  {}

  void PlatformUtil::HandleMethodCall(const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
  {
    result->NotImplemented();
  }

  void PlatformUtil::_EmitEvent(std::string eventName)
  {
    if (channel == nullptr)
      return;
    flutter::EncodableMap args = flutter::EncodableMap();
    args[flutter::EncodableValue("eventName")] = flutter::EncodableValue(eventName);
    channel->InvokeMethod(
      "onEvent", std::make_unique<flutter::EncodableValue>(args));
  }

  std::optional<LRESULT> PlatformUtil::HandleWindowProc(
    HWND hWnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) noexcept
  {
    std::optional<LRESULT> result = std::nullopt;

    if (message == WM_MOVING) {
      window_is_moving_ = true;
      if (!window_start_move_sent_) {
        window_start_move_sent_ = true;
        _EmitEvent("onWindowStartMove");
      }
      _EmitEvent("onWindowMove");
    }
    else if (message == WM_EXITSIZEMOVE) {
      if (window_is_moving_) {
        window_is_moving_ = false;
        window_start_move_sent_ = false;
        _EmitEvent("onWindowEndMove");
      }
    }
    else if (message == WM_SIZE) {
      if (wParam == SIZE_MINIMIZED) {
        if (minimized_windows_.insert(hWnd).second) {
          // hide the WebView2 instances natively as well, because their
          // input region ("ghost layer") would keep blocking mouse clicks
          // on the desktop area behind the minimized window, see
          // https://github.com/MicrosoftEdge/WebView2Feedback/issues/5459
          if (plugin && plugin->inAppWebViewManager) {
            plugin->inAppWebViewManager->setWindowVisibility(hWnd, false);
          }
          _EmitEvent("onWindowMinimize");
        }
      }
      else if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED) {
        if (minimized_windows_.erase(hWnd) > 0) {
          if (plugin && plugin->inAppWebViewManager) {
            plugin->inAppWebViewManager->setWindowVisibility(hWnd, true);
          }
          _EmitEvent("onWindowRestore");
        }
      }
    }

    return result;
  }

  PlatformUtil::~PlatformUtil()
  {
    debugLog("dealloc PlatformUtil");
    plugin = nullptr;
  }
}
