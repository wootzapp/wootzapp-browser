// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ANDROID_DEVTOOLS_MANAGER_DELEGATE_ANDROID_H_
#define CHROME_BROWSER_ANDROID_DEVTOOLS_MANAGER_DELEGATE_ANDROID_H_

#include <memory>
#include <string>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/memory/raw_ptr.h"
#include "base/scoped_multi_source_observation.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_observer.h"
#include "content/public/browser/devtools_agent_host_observer.h"
#include "content/public/browser/devtools_manager_delegate.h"
#include "content/public/browser/web_contents.h"

class ChromeDevToolsSessionAndroid;

class DevToolsManagerDelegateAndroid : public content::DevToolsManagerDelegate,
                                       public ProfileObserver {
 public:
  static constexpr int kMaxOtrContexts = 10;

  DevToolsManagerDelegateAndroid();

  DevToolsManagerDelegateAndroid(const DevToolsManagerDelegateAndroid&) =
      delete;
  DevToolsManagerDelegateAndroid& operator=(
      const DevToolsManagerDelegateAndroid&) = delete;

  ~DevToolsManagerDelegateAndroid() override;

  static DevToolsManagerDelegateAndroid* GetInstance();

  static void MarkCreatedByDevTools(content::WebContents& web_contents);
  static bool IsCreatedByDevTools(const content::WebContents& web_contents);

  // Creates a headless WebContents in the given OTR context.
  // Returns nullptr if context_id is not found.
  content::WebContents* CreateWebContentsForContext(
      const std::string& context_id,
      const GURL& url);

  // Returns the OTR profile for the given context_id, or nullptr.
  Profile* GetProfileForContext(const std::string& context_id);

 private:
  // content::DevToolsManagerDelegate implementation.
  content::BrowserContext* GetDefaultBrowserContext() override;
  content::BrowserContext* CreateBrowserContext() override;
  void DisposeBrowserContext(content::BrowserContext* context,
                             DisposeCallback callback) override;
  std::vector<content::BrowserContext*> GetBrowserContexts() override;
  scoped_refptr<content::DevToolsAgentHost> CreateNewTarget(
      const GURL& url,
      TargetType target_type,
      bool new_window) override;
  std::string GetTargetType(content::WebContents* web_contents) override;
  content::DevToolsAgentHost::List RemoteDebuggingTargets(
      TargetType target_type) override;
  bool IsBrowserTargetDiscoverable() override;

  void HandleCommand(content::DevToolsAgentHostClientChannel* channel,
                     base::span<const uint8_t> message,
                     NotHandledCallback callback) override;
  void ClientAttached(
      content::DevToolsAgentHostClientChannel* channel) override;
  void ClientDetached(
      content::DevToolsAgentHostClientChannel* channel) override;

  // ProfileObserver:
  void OnProfileWillBeDestroyed(Profile* profile) override;

  // Destroys all WebContents for a given context and removes tracking.
  void DestroyWebContentsForContext(const std::string& context_id);

  base::flat_map<content::DevToolsAgentHostClientChannel*,
                 std::unique_ptr<ChromeDevToolsSessionAndroid>>
      sessions_;

  // OTR profiles created via CreateBrowserContext, keyed by UniqueId.
  base::flat_map<std::string, raw_ptr<Profile>> otr_profiles_;

  // Headless WebContents owned per context, keyed by context UniqueId.
  base::flat_map<std::string,
                 std::vector<std::unique_ptr<content::WebContents>>>
      context_web_contents_;

  base::ScopedMultiSourceObservation<Profile, ProfileObserver>
      profile_observation_{this};
};

#endif  // CHROME_BROWSER_ANDROID_DEVTOOLS_MANAGER_DELEGATE_ANDROID_H_
