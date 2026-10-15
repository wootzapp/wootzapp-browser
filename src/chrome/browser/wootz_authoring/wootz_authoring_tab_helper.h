// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_TAB_HELPER_H_
#define CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_TAB_HELPER_H_

#include "base/memory/weak_ptr.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "url/gurl.h"

namespace content {
class NavigationHandle;
class WebContents;
}  // namespace content

namespace wootz_authoring {

class WootzAuthoringService;

class WootzAuthoringTabHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<WootzAuthoringTabHelper> {
 public:
  WootzAuthoringTabHelper(const WootzAuthoringTabHelper&) = delete;
  WootzAuthoringTabHelper& operator=(const WootzAuthoringTabHelper&) = delete;
  ~WootzAuthoringTabHelper() override;

  // content::WebContentsObserver:
  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override;
  void DocumentOnLoadCompletedInPrimaryMainFrame() override;

 private:
  friend class content::WebContentsUserData<WootzAuthoringTabHelper>;
  explicit WootzAuthoringTabHelper(content::WebContents* web_contents);

  WootzAuthoringService* GetService();

  GURL current_shell_url_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace wootz_authoring

#endif  // CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_TAB_HELPER_H_
