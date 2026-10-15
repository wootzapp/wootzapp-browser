// Copyright 2024 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wootz_offline_pages/wootz_offline_page_throttle.h"

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/stringprintf.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/scoped_blocking_call.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/wootz_offline_pages/wootz_offline_page_prefs.h"
#include "chrome/browser/wootz_offline_pages/wootz_offline_page_service.h"
#include "chrome/browser/wootz_offline_pages/wootz_offline_page_service_factory.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace {

// Posted to the UI task runner so that the new navigation starts AFTER the
// current navigation throttle has finished returning. Calling
// LoadURLWithParams() synchronously from inside WillStartRequest() /
// WillRedirectRequest() is re-entrant into NavigationController and trips a
// CHECK (manifests as SIGTRAP / TRAP_BRKPT on Android).
void StartContentNavigationLater(
    base::WeakPtr<content::WebContents> web_contents,
    GURL content_url) {
  if (!web_contents) {
    return;
  }
  content::NavigationController::LoadURLParams params(content_url);
  params.transition_type = ui::PAGE_TRANSITION_CLIENT_REDIRECT;
  web_contents->GetController().LoadURLWithParams(params);
}

}  // namespace

// static
std::unique_ptr<WootzOfflinePageThrottle>
WootzOfflinePageThrottle::MaybeCreateThrottleFor(
    content::NavigationHandle* handle) {
  if (!handle->IsInMainFrame())
    return nullptr;
  // Note: The throttle will be created via NavigationThrottleRegistry
  // This method is kept for compatibility but should not be used directly
  return nullptr;
}

WootzOfflinePageThrottle::WootzOfflinePageThrottle(
    content::NavigationThrottleRegistry& registry)
    : content::NavigationThrottle(registry) {}

WootzOfflinePageThrottle::~WootzOfflinePageThrottle() = default;

content::NavigationThrottle::ThrottleCheckResult
WootzOfflinePageThrottle::WillStartRequest() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  LOG(INFO) << " WootzOfflinePageThrottle::WillStartRequest for "
            << navigation_handle()->GetURL().spec();
  return CheckForOfflinePage();
}

content::NavigationThrottle::ThrottleCheckResult
WootzOfflinePageThrottle::WillRedirectRequest() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  LOG(INFO) << " WootzOfflinePageThrottle::WillRedirectRequest for "
            << navigation_handle()->GetURL().spec();
  return CheckForOfflinePage();
}

content::NavigationThrottle::ThrottleCheckResult
WootzOfflinePageThrottle::CheckForOfflinePage() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  
  GURL url = navigation_handle()->GetURL();
  
  // Only handle HTTP and HTTPS URLs - skip all special schemes
  if (!url.SchemeIsHTTPOrHTTPS()) {
    return content::NavigationThrottle::PROCEED;
  }

  Profile* profile = Profile::FromBrowserContext(
      navigation_handle()->GetWebContents()->GetBrowserContext());
  if (!profile) {
    LOG(WARNING) << " No profile for navigation, skipping offline page check";
    return content::NavigationThrottle::PROCEED;
  }
  const bool auto_open_enabled =
      wootz_offline_pages::prefs::IsAutoOpenOfflinePagesEnabled(
          profile->GetPrefs());
  LOG(INFO) << " [WootzOfflinePageThrottle] auto_open_pref="
            << (auto_open_enabled ? "true" : "false")
            << " url=" << url.spec();
  
  if (!auto_open_enabled) {
    LOG(INFO) << " Auto-open offline pages disabled, proceeding with online navigation";
    return content::NavigationThrottle::PROCEED;
  }
  
  WootzOfflinePageService* service =
      WootzOfflinePageServiceFactory::GetForProfile(profile);

  // Allow blocking operations for file I/O during navigation check
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                 base::BlockingType::MAY_BLOCK);

  base::FilePath mhtml_path;
  if (service->GetOfflinePagePath(url, &mhtml_path)) {
    LOG(INFO) << " Found offline page at " << mhtml_path.value();
    
    if (!base::PathExists(mhtml_path)) {
      LOG(ERROR) << " Offline page file does not exist";
      return content::NavigationThrottle::PROCEED;
    }
    
    std::string content_uri = "content://org.chromium.chrome.wootz_offline_pages" + 
                              mhtml_path.value();
    GURL content_url(content_uri);
    
    LOG(INFO) << " Loading offline page via: " << content_url.spec();

    auto web_contents_weak =
        navigation_handle()->GetWebContents()->GetWeakPtr();
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&StartContentNavigationLater, web_contents_weak,
                       content_url));

    return content::NavigationThrottle::ThrottleCheckResult(
        content::NavigationThrottle::CANCEL_AND_IGNORE);
  }
  
  base::FilePath fallback_page_path;
  std::string page_type;
  
  fallback_page_path = service->Get404PagePath();
  page_type = "404";
  LOG(INFO) << " No offline page for " << url.spec() 
            << " - loading 404 page";
  
  if (!fallback_page_path.empty() && base::PathExists(fallback_page_path)) {
    std::string content_uri = "content://org.chromium.chrome.wootz_offline_pages" + 
                              fallback_page_path.value();
    GURL content_url(content_uri);
    
    LOG(INFO) << " Loading " << page_type << " page: " << content_url.spec();

    auto web_contents_weak =
        navigation_handle()->GetWebContents()->GetWeakPtr();
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&StartContentNavigationLater, web_contents_weak,
                       content_url));

    return content::NavigationThrottle::ThrottleCheckResult(
        content::NavigationThrottle::CANCEL_AND_IGNORE);
  }
  
  // No fallback page available - proceed with online navigation
  LOG(INFO) << " No " << page_type << " page available, proceeding with online navigation";
  return content::NavigationThrottle::PROCEED;
}

const char* WootzOfflinePageThrottle::GetNameForLogging() {
  return "WootzOfflinePageThrottle";
}
