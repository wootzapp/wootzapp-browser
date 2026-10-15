// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wootz_authoring/wootz_authoring_tab_helper.h"

#include "base/logging.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/wootz_authoring/wootz_authoring_service.h"
#include "chrome/browser/wootz_authoring/wootz_authoring_service_factory.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"

namespace wootz_authoring {

WootzAuthoringTabHelper::WootzAuthoringTabHelper(
    content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<WootzAuthoringTabHelper>(*web_contents) {}

WootzAuthoringTabHelper::~WootzAuthoringTabHelper() = default;

WootzAuthoringService* WootzAuthoringTabHelper::GetService() {
  Profile* profile =
      Profile::FromBrowserContext(web_contents()->GetBrowserContext());
  if (!profile) {
    return nullptr;
  }
  return WootzAuthoringServiceFactory::GetForProfile(profile);
}

void WootzAuthoringTabHelper::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!navigation_handle->HasCommitted() ||
      !navigation_handle->IsInPrimaryMainFrame()) {
    return;
  }

  const GURL& url = navigation_handle->GetURL();

  if (!url.SchemeIsHTTPOrHTTPS()) {
    return;
  }

  if (!navigation_handle->IsSameDocument()) {
    current_shell_url_ = url;
    return;
  }

  if (current_shell_url_.is_empty() || url == current_shell_url_) {
    return;
  }

  WootzAuthoringService* service = GetService();
  if (!service || !service->IsCaptureEnabled()) {
    return;
  }

  LOG(ERROR) << " [WootzAuthoringTabHelper] same-document route url=" << url
             << " shell=" << current_shell_url_;
  service->RecordSameDocumentRoute(current_shell_url_, url);
}

void WootzAuthoringTabHelper::DocumentOnLoadCompletedInPrimaryMainFrame() {
  WootzAuthoringService* service = GetService();
  if (!service || !service->IsCaptureEnabled()) {
    return;
  }

  const GURL& url = web_contents()->GetLastCommittedURL();
  if (!url.SchemeIsHTTPOrHTTPS()) {
    return;
  }

  LOG(ERROR) << " [WootzAuthoringTabHelper] snapshot client state url=" << url;
  service->SnapshotClientStateForUrl(url);
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(WootzAuthoringTabHelper);

}  // namespace wootz_authoring
