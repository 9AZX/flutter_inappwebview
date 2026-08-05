#include <DispatcherQueue.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>
#include <shlobj.h>
#include <windows.graphics.capture.h>

#include "../in_app_webview/in_app_webview_settings.h"
#include "../plugin_scripts_js/javascript_bridge_js.h"
#include "../types/url_request.h"
#include "../types/user_script.h"
#include "../utils/flutter.h"
#include "../utils/log.h"
#include "../utils/string.h"
#include "../utils/vector.h"
#include "../webview_environment/webview_environment_manager.h"
#include "in_app_webview_manager.h"

namespace flutter_inappwebview_plugin
{
  InAppWebViewManager::InAppWebViewManager(const FlutterInappwebviewWindowsPlugin* plugin)
    : plugin(plugin),
    ChannelDelegate(plugin->registrar->messenger(), InAppWebViewManager::METHOD_CHANNEL_NAME)
  {
    if (!rohelper_) {
      rohelper_ = std::make_unique<rx::RoHelper>(RO_INIT_SINGLETHREADED);

      if (rohelper_->WinRtAvailable()) {
        DispatcherQueueOptions options{ sizeof(DispatcherQueueOptions),
                                       DQTYPE_THREAD_CURRENT, DQTAT_COM_STA };

        if (FAILED(rohelper_->CreateDispatcherQueueController(
          options, dispatcher_queue_controller_.put()))) {
          std::cerr << "Creating DispatcherQueueController failed." << std::endl;
          return;
        }

        if (!isGraphicsCaptureSessionSupported()) {
          std::cerr << "Windows::Graphics::Capture::GraphicsCaptureSession is not "
            "supported."
            << std::endl;
          return;
        }

        graphics_context_ = std::make_unique<GraphicsContext>(rohelper_.get());
        compositor_ = graphics_context_->CreateCompositor();
        if (compositor_) {
          // fix for KernelBase.dll RaiseFailFastException
          // when app is closing 
          compositor_->AddRef();
        }
        valid_ = graphics_context_->IsValid();
      }
    }

    windowClass_.lpszClassName = CustomPlatformView::CLASS_NAME;
    windowClass_.lpfnWndProc = &DefWindowProc;
    windowClass_.style |= CS_NOCLOSE;

    RegisterClass(&windowClass_);
  }

  void InAppWebViewManager::HandleMethodCall(const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
  {
    auto* arguments = std::get_if<flutter::EncodableMap>(method_call.arguments());
    auto& methodName = method_call.method_name();

    if (string_equals(methodName, "createInAppWebView")) {
      if (isSupported()) {
        createInAppWebView(arguments, std::move(result));
      }
      else {
        result->Error("0", "Creating an InAppWebView instance is not supported! Graphics Context is not valid!");
      }
    }
    else if (string_equals(methodName, "dispose")) {
      auto id = get_fl_map_value<int64_t>(*arguments, "id");
      if (map_contains(webViews, (uint64_t)id)) {
        auto platformView = webViews.at(id).get();
        if (platformView) {
          platformView->UnregisterMethodCallHandler();
        }
        webViews.erase(id);
      }
      result->Success();
    }
    else if (string_equals(methodName, "disposeKeepAlive")) {
      auto keepAliveId = get_fl_map_value<std::string>(*arguments, "keepAliveId");
      disposeKeepAlive(keepAliveId);
      result->Success();
    }
    else if (string_equals(methodName, "setJavaScriptBridgeName")) {
      auto bridgeName = get_fl_map_value<std::string>(*arguments, "bridgeName");
      JavaScriptBridgeJS::set_JAVASCRIPT_BRIDGE_NAME(bridgeName);
      result->Success();
    }
    else if (string_equals(methodName, "getJavaScriptBridgeName")) {
      result->Success(JavaScriptBridgeJS::get_JAVASCRIPT_BRIDGE_NAME());
    }
    else {
      result->NotImplemented();
    }
  }

  void InAppWebViewManager::createInAppWebView(const flutter::EncodableMap* arguments, std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
  {
    auto result_ = std::shared_ptr<flutter::MethodResult<flutter::EncodableValue>>(std::move(result));

    if (!plugin) {
      result_->Error("0", "Cannot create the InAppWebView instance!");
      return;
    }

    auto settingsMap = get_fl_map_value<flutter::EncodableMap>(*arguments, "initialSettings");
    auto urlRequestMap = get_optional_fl_map_value<flutter::EncodableMap>(*arguments, "initialUrlRequest");
    auto initialFile = get_optional_fl_map_value<std::string>(*arguments, "initialFile");
    auto initialDataMap = get_optional_fl_map_value<flutter::EncodableMap>(*arguments, "initialData");
    auto initialUserScriptList = get_optional_fl_map_value<flutter::EncodableList>(*arguments, "initialUserScripts");
    auto webViewEnvironmentId = get_optional_fl_map_value<std::string>(*arguments, "webViewEnvironmentId");
    auto keepAliveId = get_optional_fl_map_value<std::string>(*arguments, "keepAliveId");
    auto windowId = get_optional_fl_map_value<int64_t>(*arguments, "windowId");

    HWND flutterWindowHWnd = plugin->registrar->GetView()->GetNativeWindow();

    RECT bounds;
    GetClientRect(flutterWindowHWnd, &bounds);

    // create the window at the current Flutter window position instead of
    // the (0, 0) screen origin, so the WebView2 input region never sits over
    // an area of the desktop not covered by the app before the first
    // setPosition call coming from the Dart side.
    RECT flutterWindowRect;
    GetWindowRect(flutterWindowHWnd, &flutterWindowRect);

    auto hwnd = CreateWindowEx(0, windowClass_.lpszClassName, L"", 0,
      flutterWindowRect.left, flutterWindowRect.top,
      bounds.right - bounds.left, bounds.bottom - bounds.top,
      flutterWindowHWnd,
      nullptr,
      windowClass_.hInstance, nullptr);

    if (keepAliveId.has_value() && map_contains(keepAliveWebViews, keepAliveId.value())) {
      auto webView = std::move(keepAliveWebViews.at(keepAliveId.value())->view);
      keepAliveWebViews.erase(keepAliveId.value());
      auto customPlatformView = std::make_unique<CustomPlatformView>(plugin->registrar->messenger(),
        plugin->registrar->texture_registrar(),
        graphics_context(),
        hwnd,
        std::move(webView));
      auto textureId = customPlatformView->texture_id();
      keepAliveWebViews.insert({ keepAliveId.value(), std::move(customPlatformView) });
      result_->Success(textureId);
      return;
    }

    auto webViewEnvironment = webViewEnvironmentId.has_value() && map_contains(plugin->webViewEnvironmentManager->webViewEnvironments, webViewEnvironmentId.value())
      ? plugin->webViewEnvironmentManager->webViewEnvironments.at(webViewEnvironmentId.value()).get() : nullptr;

    auto initialSettings = std::make_shared<InAppWebViewSettings>(settingsMap);

    InAppWebView::createInAppWebViewEnv(hwnd, true, webViewEnvironment, initialSettings,
      [=](wil::com_ptr<ICoreWebView2Environment> webViewEnv,
        wil::com_ptr<ICoreWebView2Controller> webViewController,
        wil::com_ptr<ICoreWebView2CompositionController> webViewCompositionController)
      {
        if (plugin && webViewEnv && webViewController && webViewCompositionController) {
          std::optional<std::vector<std::shared_ptr<UserScript>>> initialUserScripts = initialUserScriptList.has_value() ?
            functional_map(initialUserScriptList.value(), [](const flutter::EncodableValue& map) { return std::make_shared<UserScript>(std::get<flutter::EncodableMap>(map)); }) :
            std::optional<std::vector<std::shared_ptr<UserScript>>>{};

          InAppWebViewCreationParams params = {
            "",
            std::move(initialSettings),
            initialUserScripts
          };

          auto inAppWebView = std::make_unique<InAppWebView>(plugin, params, hwnd, std::move(webViewEnv), std::move(webViewController), std::move(webViewCompositionController));

          std::optional<std::shared_ptr<URLRequest>> urlRequest = urlRequestMap.has_value() ? std::make_shared<URLRequest>(urlRequestMap.value()) : std::optional<std::shared_ptr<URLRequest>>{};
          if (urlRequest.has_value()) {
            inAppWebView->loadUrl(urlRequest.value());
          }
          else if (initialFile.has_value()) {
            inAppWebView->loadFile(initialFile.value());
          }
          else if (initialDataMap.has_value()) {
            inAppWebView->loadData(get_fl_map_value<std::string>(initialDataMap.value(), "data"));
          }

          if (windowId.has_value() && map_contains(windowWebViews, windowId.value())) {
            auto windowWebViewArgs = windowWebViews.at(windowId.value()).get();
            windowWebViewArgs->args->put_NewWindow(inAppWebView->webView.get());
            windowWebViewArgs->args->put_Handled(TRUE);
            windowWebViewArgs->deferral->Complete();
            windowWebViews.erase(windowId.value());
          }

          auto customPlatformView = std::make_unique<CustomPlatformView>(plugin->registrar->messenger(),
            plugin->registrar->texture_registrar(),
            graphics_context(),
            hwnd,
            std::move(inAppWebView));

          auto textureId = customPlatformView->texture_id();

          if (keepAliveId.has_value()) {
            customPlatformView->view->initChannel(keepAliveId.value(), std::nullopt);
            keepAliveWebViews.insert({ keepAliveId.value(), std::move(customPlatformView) });
          }
          else {
            customPlatformView->view->initChannel(textureId, std::nullopt);
            webViews.insert({ textureId, std::move(customPlatformView) });
          }
          result_->Success(textureId);
        }
        else {
          result_->Error("0", "Cannot create the InAppWebView instance!");
        }
      }
    );
  }

  void InAppWebViewManager::setWindowVisibility(HWND topLevelWindow, bool visible)
  {
    auto applyVisibility = [topLevelWindow, visible](CustomPlatformView* platformView)
      {
        if (!platformView || !platformView->view) {
          return;
        }
        // the WebView2 host window is an owned window whose owner is the
        // top-level Flutter window, so apply only to webviews belonging
        // to the top-level window that is being minimized/restored.
        auto owner = GetWindow(platformView->hwnd(), GW_OWNER);
        if (owner != nullptr && GetAncestor(owner, GA_ROOT) != topLevelWindow) {
          return;
        }
        if (visible) {
          platformView->view->show();
        }
        else {
          platformView->view->hide();
        }
      };

    for (auto& [id, platformView] : webViews) {
      applyVisibility(platformView.get());
    }
    for (auto& [keepAliveId, platformView] : keepAliveWebViews) {
      applyVisibility(platformView.get());
    }
  }

  void InAppWebViewManager::disposeKeepAlive(const std::string& keepAliveId)
  {
    if (map_contains(keepAliveWebViews, keepAliveId)) {
      auto platformView = keepAliveWebViews.at(keepAliveId).get();
      if (platformView) {
        platformView->UnregisterMethodCallHandler();
      }
      keepAliveWebViews.erase(keepAliveId);
    }
  }

  bool InAppWebViewManager::isGraphicsCaptureSessionSupported()
  {
    HSTRING className;
    HSTRING_HEADER classNameHeader;

    if (FAILED(rohelper_->GetStringReference(
      RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession,
      &className, &classNameHeader))) {
      return false;
    }

    ABI::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics*
      capture_session_statics;
    if (FAILED(rohelper_->GetActivationFactory(
      className,
      __uuidof(
        ABI::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics),
      (void**)&capture_session_statics))) {
      return false;
    }

    boolean is_supported = false;
    if (FAILED(capture_session_statics->IsSupported(&is_supported))) {
      return false;
    }

    return !!is_supported;
  }

  InAppWebViewManager::~InAppWebViewManager()
  {
    debugLog("dealloc InAppWebViewManager");
    webViews.clear();
    keepAliveWebViews.clear();
    windowWebViews.clear();
    UnregisterClass(windowClass_.lpszClassName, nullptr);
    plugin = nullptr;
  }
}
