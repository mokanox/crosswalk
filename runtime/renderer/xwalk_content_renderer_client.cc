// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Copyright (c) 2013 Intel Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xwalk/runtime/renderer/xwalk_content_renderer_client.h"

#include "base/command_line.h"
#include "base/strings/utf_string_conversions.h"
#include "components/autofill/content/renderer/autofill_agent.h"
#include "components/autofill/content/renderer/password_autofill_agent.h"
#include "components/visitedlink/renderer/visitedlink_slave.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "content/public/renderer/render_frame_observer_tracker.h"
#include "content/public/renderer/render_thread.h"
#include "content/public/renderer/render_view.h"
#include "components/error_page/common/localized_error.h"
#include "grit/xwalk_application_resources.h"
#include "grit/xwalk_sysapps_resources.h"
#include "net/base/net_errors.h"
#include "third_party/WebKit/public/platform/WebCachePolicy.h"
#include "third_party/WebKit/public/platform/WebString.h"
#include "third_party/WebKit/public/platform/WebURLError.h"
#include "third_party/WebKit/public/platform/WebURLRequest.h"
#include "third_party/WebKit/public/web/WebDocument.h"
#include "third_party/WebKit/public/web/WebSecurityPolicy.h"
#include "xwalk/application/common/constants.h"
#include "xwalk/application/renderer/application_native_module.h"
#include "xwalk/extensions/common/xwalk_extension_switches.h"
#include "xwalk/extensions/renderer/xwalk_js_module.h"
#include "xwalk/runtime/common/xwalk_common_messages.h"
#include "xwalk/runtime/common/xwalk_localized_error.h"
#include "xwalk/runtime/renderer/isolated_file_system.h"
#include "xwalk/runtime/renderer/pepper/pepper_helper.h"
#include "services/service_manager/public/cpp/interface_registry.h"

#if defined(OS_ANDROID)
#include "components/cdm/renderer/android_key_systems.h"
#include "xwalk/runtime/browser/android/net/url_constants.h"
#include "xwalk/runtime/common/android/xwalk_render_view_messages.h"
#include "xwalk/runtime/renderer/android/xwalk_permission_client.h"
#include "xwalk/runtime/renderer/android/xwalk_render_thread_observer.h"
#include "xwalk/runtime/renderer/android/xwalk_render_frame_ext.h"
#include "third_party/WebKit/public/web/WebLocalFrame.h"
#endif

#if !defined(DISABLE_NACL)
#include "components/nacl/renderer/nacl_helper.h"
#endif

#ifdef TENTA_CHROMIUM_BUILD
// gen
#include "xwalk/third_party/tenta/crosswalk_extensions/resources/grit/tenta_error_pages_browser_resources.h"
//tenta
#include "renderer/neterror/tenta_net_error_helper.h"
//chromium
#include "ui/base/resource/resource_bundle.h"
#include "third_party/zlib/google/compression_utils.h"
#endif

using content::RenderThread;

namespace xwalk {

namespace {

xwalk::XWalkContentRendererClient* g_renderer_client;

class XWalkFrameHelper
    : public content::RenderFrameObserver,
      public content::RenderFrameObserverTracker<XWalkFrameHelper> {
 public:
  XWalkFrameHelper(
      content::RenderFrame* render_frame,
      extensions::XWalkExtensionRendererController* extension_controller)
      : content::RenderFrameObserver(render_frame),
        content::RenderFrameObserverTracker<XWalkFrameHelper>(render_frame),
        extension_controller_(extension_controller) {}
  ~XWalkFrameHelper() override {}

  // RenderFrameObserver implementation.
  void DidCreateScriptContext(v8::Handle<v8::Context> context,
                              int world_id) override {
    if (extension_controller_)
      extension_controller_->DidCreateScriptContext(
          render_frame()->GetWebFrame(), context);
  }
  void WillReleaseScriptContext(v8::Handle<v8::Context> context,
                                int world_id) override {
    if (extension_controller_)
      extension_controller_->WillReleaseScriptContext(
          render_frame()->GetWebFrame(), context);
  }

  void OnDestruct() override {
    delete this;
  }

 private:
  extensions::XWalkExtensionRendererController* extension_controller_;

  DISALLOW_COPY_AND_ASSIGN(XWalkFrameHelper);
};

}  // namespace

XWalkContentRendererClient* XWalkContentRendererClient::Get() {
  return g_renderer_client;
}

XWalkContentRendererClient::XWalkContentRendererClient() {
  DCHECK(!g_renderer_client);
  g_renderer_client = this;
}

XWalkContentRendererClient::~XWalkContentRendererClient() {
  g_renderer_client = NULL;
}

void XWalkContentRendererClient::RenderThreadStarted() {
  content::RenderThread* thread = content::RenderThread::Get();
  xwalk_render_thread_observer_.reset(new XWalkRenderThreadObserver);
  thread->AddObserver(xwalk_render_thread_observer_.get());
  visited_link_slave_.reset(new visitedlink::VisitedLinkSlave);
  thread->GetInterfaceRegistry()->AddInterface(
    visited_link_slave_->GetBindCallback());

  base::CommandLine* cmd_line = base::CommandLine::ForCurrentProcess();
  if (!cmd_line->HasSwitch(switches::kXWalkDisableExtensions))
    extension_controller_.reset(
        new extensions::XWalkExtensionRendererController(this));
/*
  // TODO moved to runtime/common/xwalk_content_client.cc
   * @note see chromium commit 0901535a93c8bdf5d9a67edc8085beb2786888b3
  blink::WebString application_scheme(
      blink::WebString::fromASCII(application::kApplicationScheme));
  blink::WebSecurityPolicy::registerURLSchemeAsSecure(application_scheme);
  blink::WebSecurityPolicy::registerURLSchemeAsCORSEnabled(application_scheme);
#if defined(OS_ANDROID)
  blink::WebString content_scheme(
      blink::WebString::fromASCII(xwalk::kContentScheme));
  blink::WebSecurityPolicy::registerURLSchemeAsLocal(content_scheme);
#endif
*/
}

#if defined(OS_ANDROID)
bool XWalkContentRendererClient::HandleNavigation(
    content::RenderFrame* render_frame,
    bool is_content_initiated,
    bool render_view_was_created_by_renderer,
    blink::WebFrame* frame,
    const blink::WebURLRequest& request,
    blink::WebNavigationType type,
    blink::WebNavigationPolicy default_policy,
    bool is_redirect) {
#if TENTA_LOG_ENABLE == 1
  LOG(INFO) << "!!! " << __func__ << " is_content_initiated=" << is_content_initiated
      << " render_view_was_created_by_renderer=" << render_view_was_created_by_renderer
      << " is_redirect=" << is_redirect
      << " type=" << type
      << " is_main_frame=" << !frame->Parent()
      << " url=" << request.Url();
#endif
  // Only GETs can be overridden.
  if (!request.HttpMethod().Equals("GET"))
    return false;

  // Any navigation from loadUrl, and goBack/Forward are considered application-
  // initiated and hence will not yield a shouldOverrideUrlLoading() callback.
  // Webview classic does not consider reload application-initiated so we
  // continue the same behavior.
  // TODO(sgurun) is_content_initiated is normally false for cross-origin
  // navigations but since android_webview does not swap out renderers, this
  // works fine. This will stop working if android_webview starts swapping out
  // renderers on navigation.
  bool application_initiated =
      !is_content_initiated || type == blink::kWebNavigationTypeBackForward;

  // Don't offer application-initiated navigations unless it's a redirect.
  if (application_initiated && !is_redirect)
    return false;
  bool is_main_frame = !frame->Parent();
  const GURL& gurl = request.Url();
  // For HTTP schemes, only top-level navigations can be overridden. Similarly,
  // WebView Classic lets app override only top level about:blank navigations.
  // So we filter out non-top about:blank navigations here.
  if (!is_main_frame &&
      (gurl.SchemeIs(url::kHttpScheme) || gurl.SchemeIs(url::kHttpsScheme) ||
       gurl.SchemeIs(url::kAboutScheme)))
    return false;
  // TODO(iotto) analyse how to replace opener_id with render_view_was_created_by_renderer

  // use NavigationInterception throttle to handle the call as that can
  // be deferred until after the java side has been constructed.
//  if (opener_id != MSG_ROUTING_NONE)
//    return false;

  bool ignore_navigation = false;
  base::string16 url = request.Url().GetString().Utf16();
  bool has_user_gesture = request.HasUserGesture();

  int render_frame_id = render_frame->GetRoutingID();
  RenderThread::Get()->Send(new XWalkViewHostMsg_ShouldOverrideUrlLoading(
      render_frame_id, url, has_user_gesture, is_redirect, is_main_frame,
      &ignore_navigation));
  return ignore_navigation;
}
#endif

void XWalkContentRendererClient::RenderFrameCreated(
    content::RenderFrame* render_frame) {
  new XWalkFrameHelper(render_frame, extension_controller_.get());
  new XWalkRenderFrameExt(render_frame);
#if defined(OS_ANDROID)
  new XWalkPermissionClient(render_frame);
#endif

#if defined(ENABLE_PLUGINS)
  new PepperHelper(render_frame);
#endif

#if !defined(DISABLE_NACL)
  new nacl::NaClHelper(render_frame);
#endif

  // The following code was copied from
  // android_webview/renderer/aw_content_renderer_client.cc
#if defined(OS_ANDROID)
  // TODO(jam): when the frame tree moves into content and parent() works at
  // RenderFrame construction, simplify this by just checking parent().
  content::RenderFrame* parent_frame =
      render_frame->GetRenderView()->GetMainRenderFrame();
  if (parent_frame && parent_frame != render_frame) {
    // Avoid any race conditions from having the browser's UI thread tell the IO
    // thread that a subframe was created.
    RenderThread::Get()->Send(new XWalkViewHostMsg_SubFrameCreated(
        parent_frame->GetRoutingID(), render_frame->GetRoutingID()));
  }
#endif
  // TODO(sgurun) do not create a password autofill agent (change
  // autofill agent to store a weakptr).
  autofill::PasswordAutofillAgent* password_autofill_agent =
      new autofill::PasswordAutofillAgent(render_frame);
  new autofill::AutofillAgent(render_frame, password_autofill_agent, nullptr);

#ifdef TENTA_CHROMIUM_BUILD
  new ::tenta::ext::TentaNetErrorHelper(render_frame);
#endif

}

void XWalkContentRendererClient::RenderViewCreated(
    content::RenderView* render_view) {
#if defined(OS_ANDROID)
  // TODO (iotto) replaced with XWalkRenderFrameExt
//  XWalkRenderViewExt::RenderViewCreated(render_view);
#endif
}

void XWalkContentRendererClient::DidCreateModuleSystem(
    extensions::XWalkModuleSystem* module_system) {
  std::unique_ptr<extensions::XWalkNativeModule> app_module(
      new application::ApplicationNativeModule());
  module_system->RegisterNativeModule("application", std::move(app_module));
  std::unique_ptr<extensions::XWalkNativeModule> isolated_file_system_module(
      new extensions::IsolatedFileSystem());
  module_system->RegisterNativeModule("isolated_file_system",
      std::move(isolated_file_system_module));
  module_system->RegisterNativeModule("sysapps_common",
      extensions::CreateJSModuleFromResource(IDR_XWALK_SYSAPPS_COMMON_API));
  module_system->RegisterNativeModule("widget_common",
      extensions::CreateJSModuleFromResource(
          IDR_XWALK_APPLICATION_WIDGET_COMMON_API));
}

bool XWalkContentRendererClient::IsExternalPepperPlugin(
    const std::string& module_name) {
  // TODO(bbudge) remove this when the trusted NaCl plugin has been removed.
  // We must defer certain plugin events for NaCl instances since we switch
  // from the in-process to the out-of-process proxy after instantiating them.
  return module_name == "Native Client";
}

unsigned long long XWalkContentRendererClient::VisitedLinkHash(
    const char* canonical_url, size_t length) {
  return visited_link_slave_->ComputeURLFingerprint(canonical_url, length);
}

bool XWalkContentRendererClient::IsLinkVisited(unsigned long long link_hash) {
  return visited_link_slave_->IsVisited(link_hash);
}

bool XWalkContentRendererClient::WillSendRequest(
                       blink::WebLocalFrame* frame,
                       ui::PageTransition transition_type,
                       const blink::WebURL& url,
                       GURL* new_url) {
#if TENTA_LOG_NET_ENABLE == 1
  LOG(INFO) << "XWalkContentRendererClient::WillSendRequest doc_url="
               << frame->GetDocument().Url().GetString().Utf8() << " url="
               << url.GetString().Utf8();
#endif
#if defined(OS_ANDROID)
  content::RenderView* render_view =
      content::RenderView::FromWebView(frame->View());
  if ( render_view == nullptr ) {
    return false; // no overwrite
  }

  content::RenderFrame* render_frame = render_view->GetMainRenderFrame();
  if ( render_frame == nullptr ) {
    return false; // no overwrite
  }

  int render_frame_id = render_frame->GetRoutingID();

  bool did_overwrite = false;
  std::string url_str = url.GetString().Utf8();
  std::string new_url_str;

  RenderThread::Get()->Send(new XWalkViewHostMsg_WillSendRequest(render_frame_id,
                                                                 url_str,
                                                                 transition_type,
                                                                 &new_url_str,
                                                                 &did_overwrite));

  if ( did_overwrite ) {
    *new_url = GURL(new_url_str);
#if TENTA_LOG_NET_ENABLE == 1
    LOG(INFO) << "XWalkContentRendererClient::WillSendRequest did_overwrite";
#endif
  }
  return did_overwrite;
#else
  // TODO(iotto) for other than android implement
/*  if (!xwalk_render_thread_observer_->IsWarpMode() &&
      !xwalk_render_thread_observer_->IsCSPMode())
    return false;

  GURL origin_url(frame->document().url());
  GURL app_url(xwalk_render_thread_observer_->app_url());
  // if under CSP mode.
  if (xwalk_render_thread_observer_->IsCSPMode()) {
    if (!origin_url.is_empty() && origin_url != first_party_for_cookies &&
        !xwalk_render_thread_observer_->CanRequest(app_url, url)) {
      LOG(INFO) << "[BLOCK] allow-navigation: " << url.spec();
      content::RenderThread::Get()->Send(new ViewMsg_OpenLinkExternal(url));
      *new_url = GURL();
      return true;
    }
    return false;
  }

  // if under WARP mode.
  if (url.GetOrigin() == app_url.GetOrigin() ||
      xwalk_render_thread_observer_->CanRequest(app_url, url)) {
    DLOG(INFO) << "[PASS] " << origin_url.spec() << " request " << url.spec();
    return false;
  }

  LOG(INFO) << "[BLOCK] " << origin_url.spec() << " request " << url.spec();
  *new_url = GURL();
  return true;
*/
#endif
}

void XWalkContentRendererClient::GetNavigationErrorStrings(
    content::RenderFrame* render_frame,
    const blink::WebURLRequest& failed_request,
    const blink::WebURLError& error,
    std::string* error_html,
    base::string16* error_description) {
  // TODO(guangzhen): Check whether error_html is needed in xwalk runtime.
  bool is_post = failed_request.HttpMethod().Ascii() == "POST";

  LOG(INFO) << "iotto " << __func__ << " http_method=" << failed_request.HttpMethod().Ascii() << " error_html="
            << error_html << " error_description=" << error_description;

//  if (error_description) {
//    if (error.localized_description.IsEmpty())
//      *error_description = base::ASCIIToUTF16(net::ErrorToString(error.reason));
//    else
//      *error_description = error.localized_description.Utf16();
//  }
  if (error_description) {
    *error_description = error_page::LocalizedError::GetErrorDetails(
        error.domain.Utf8(), error.reason, is_post);
  }
#ifdef TENTA_CHROMIUM_BUILD
  bool is_ignoring_cache = failed_request.GetCachePolicy() == blink::WebCachePolicy::kBypassingCache;

  if (error_html) {
    ::tenta::ext::TentaNetErrorHelper::Get(render_frame)->GetErrorHTML(error, is_post, is_ignoring_cache, error_html);
  }
#endif
//  if (error_html) {
//    // TODO(iotto) : Move this to LocalizedError
//    int resource_id = IDR_TENTA_ERR_UNKNOWN;
//    if (error.domain.Utf8() == net::kErrorDomain) {
//      switch (error.reason) {
//        case net::ERR_NAME_NOT_RESOLVED:
//          resource_id = IDR_TENTA_ERR_NAME_NOT_RESOLVED_HTML;
//          break;
//        case net::ERR_INTERNET_DISCONNECTED:
//          resource_id = IDR_TENTA_ERR_INTERNET_DISCONNECTED_HTML;
//          break;
//      }
//      base::StringPiece raw_response = ui::ResourceBundle::GetSharedInstance().GetRawDataResource(resource_id);
//      compression::GzipUncompress(raw_response.as_string(), error_html);
//    }
//  }

//  if (error_description) {
//    *error_description = LocalizedError::GetErrorDetails(error, is_post);
//  }

//  if ( error_description && !error_description->empty()) {
//    LOG(INFO) << "iotto " << __func__ << " error_description=" << *error_description;
//  }
}

void XWalkContentRendererClient::AddSupportedKeySystems(
    std::vector<std::unique_ptr<::media::KeySystemProperties>>* key_systems) {
#if defined(OS_ANDROID)
  cdm::AddAndroidWidevine(key_systems);
#endif  // defined(OS_ANDROID)
}

bool XWalkContentRendererClient::ShouldReportDetailedMessageForSource(const base::string16& source) const {
  LOG(INFO) << "iotto " << __func__ << " src=" << source;
  return false;
}

bool XWalkContentRendererClient::HasErrorPage(int http_status_code, std::string* error_domain) {
  // TODO(iotto) : Replace with tenta implementation
  // or maybe return true and load a default error page for unhandled errors
  LOG(INFO) << "iotto " << __func__ << " httpStatusCode=" << http_status_code;
  // Use an internal error page, if we have one for the status code.
  if (!error_page::LocalizedError::HasStrings(
          error_page::LocalizedError::kHttpErrorDomain, http_status_code)) {
    return false;
  }

  *error_domain = error_page::LocalizedError::kHttpErrorDomain;
  return true;
}

}  // namespace xwalk
