// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/android/devtools_manager_delegate_android.h"

#include <map>
#include <memory>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/android/tab_android.h"
#include "chrome/browser/devtools/chrome_devtools_session_android.h"
#include "chrome/browser/profiles/profile_destroyer.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/android/tab_model/tab_model.h"
#include "chrome/browser/ui/android/tab_model/tab_model_list.h"
#include "chrome/grit/browser_resources.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/devtools_external_agent_proxy.h"
#include "content/public/browser/devtools_external_agent_proxy_delegate.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/resource/resource_bundle.h"
#include "url/gurl.h"
#include "url/url_constants.h"

using content::DevToolsAgentHost;
using content::WebContents;

namespace {

constexpr base::TimeDelta kDestroyProfileTimeout = base::Seconds(60);

class ClientProxy : public content::DevToolsAgentHostClient {
 public:
  explicit ClientProxy(content::DevToolsExternalAgentProxy* proxy)
      : proxy_(proxy) {}

  ClientProxy(const ClientProxy&) = delete;
  ClientProxy& operator=(const ClientProxy&) = delete;

  ~ClientProxy() override = default;

  void DispatchProtocolMessage(DevToolsAgentHost* agent_host,
                               base::span<const uint8_t> message) override {
    proxy_->DispatchOnClientHost(message);
  }

  void AgentHostClosed(DevToolsAgentHost* agent_host) override {
    proxy_->ConnectionClosed();
  }

 private:
  raw_ptr<content::DevToolsExternalAgentProxy> proxy_;
};

class TabProxyDelegate : public content::DevToolsExternalAgentProxyDelegate {
 public:
  TabProxyDelegate(TabAndroid* tab, bool use_tab_target)
      : tab_id_(tab->GetAndroidId()),
        title_(base::UTF16ToUTF8(tab->GetTitle())),
        url_(tab->GetURL()),
        use_tab_target_(use_tab_target) {
    if (tab->web_contents()) {
      agent_host_ =
          use_tab_target
              ? DevToolsAgentHost::GetOrCreateForTab(tab->web_contents())
              : DevToolsAgentHost::GetOrCreateFor(tab->web_contents());
    }
  }
  TabProxyDelegate(const TabProxyDelegate&) = delete;
  TabProxyDelegate& operator=(const TabProxyDelegate&) = delete;

  ~TabProxyDelegate() override = default;

  void Attach(content::DevToolsExternalAgentProxy* proxy) override {
    proxies_[proxy] = std::make_unique<ClientProxy>(proxy);
    if (agent_host_)
      agent_host_->AttachClient(proxies_[proxy].get());
  }

  void Detach(content::DevToolsExternalAgentProxy* proxy) override {
    auto it = proxies_.find(proxy);
    if (it == proxies_.end())
      return;
    if (agent_host_)
      agent_host_->DetachClient(it->second.get());
    proxies_.erase(it);
    if (proxies_.empty()) {
      agent_host_ = nullptr;
      MaterializeAgentHost();
    }
  }

  std::string GetType() override {
    return agent_host_ ? agent_host_->GetType() : DevToolsAgentHost::kTypePage;
  }

  std::string GetTitle() override {
    return agent_host_ ? agent_host_->GetTitle() : title_;
  }

  std::string GetDescription() override {
    return agent_host_ ? agent_host_->GetDescription() : "";
  }

  GURL GetURL() override {
    return agent_host_ ? agent_host_->GetURL() : url_;
  }

  GURL GetFaviconURL() override {
    return agent_host_ ? agent_host_->GetFaviconURL() : GURL();
  }

  std::string GetFrontendURL() override {
    return std::string();
  }

  bool Activate() override {
    TabModel* model;
    int index;
    if (!FindTab(&model, &index))
      return false;
    model->SetActiveIndex(index);
    return true;
  }

  void Reload() override {
    MaterializeAgentHost();
    if (agent_host_)
      agent_host_->Reload();
  }

  bool Close() override {
    TabModel* model;
    int index;
    if (!FindTab(&model, &index))
      return false;
    model->CloseTabAt(index);
    return true;
  }

  base::TimeTicks GetLastActivityTime() override {
    return agent_host_ ? agent_host_->GetLastActivityTime() : base::TimeTicks();
  }

  void SendMessageToBackend(content::DevToolsExternalAgentProxy* proxy,
                            base::span<const uint8_t> message) override {
    auto it = proxies_.find(proxy);
    if (it == proxies_.end())
      return;
    if (agent_host_)
      agent_host_->DispatchProtocolMessage(it->second.get(), message);
  }

 private:
  void MaterializeAgentHost() {
    if (agent_host_)
      return;
    TabModel* model;
    int index;
    if (!FindTab(&model, &index))
      return;
    WebContents* web_contents = model->GetWebContentsAt(index);
    if (!web_contents)
      return;
    agent_host_ = use_tab_target_
                      ? DevToolsAgentHost::GetOrCreateForTab(web_contents)
                      : DevToolsAgentHost::GetOrCreateFor(web_contents);
  }

  bool FindTab(TabModel** model_result, int* index_result) const {
    for (TabModel* model : TabModelList::models()) {
      for (int i = 0; i < model->GetTabCount(); ++i) {
        TabAndroid* tab = model->GetTabAt(i);
        if (tab && tab->GetAndroidId() == tab_id_) {
          *model_result = model;
          *index_result = i;
          return true;
        }
      }
    }
    return false;
  }

  const int tab_id_;
  const std::string title_;
  const GURL url_;
  const bool use_tab_target_;
  scoped_refptr<DevToolsAgentHost> agent_host_;
  std::map<content::DevToolsExternalAgentProxy*, std::unique_ptr<ClientProxy>>
      proxies_;
};

scoped_refptr<DevToolsAgentHost> DevToolsAgentHostForTab(TabAndroid* tab,
                                                         bool use_tab_target) {
  scoped_refptr<DevToolsAgentHost> result = tab->GetDevToolsAgentHost();
  if (result)
    return result;

  result = DevToolsAgentHost::Forward(
      base::NumberToString(tab->GetAndroidId()),
      std::make_unique<TabProxyDelegate>(tab, use_tab_target));
  tab->SetDevToolsAgentHost(result);
  return result;
}

static const void* const kCreatedByDevTools = &kCreatedByDevTools;

}  //  namespace

static DevToolsManagerDelegateAndroid* g_instance = nullptr;

DevToolsManagerDelegateAndroid::DevToolsManagerDelegateAndroid() {
  DCHECK(!g_instance);
  g_instance = this;
}

// static
DevToolsManagerDelegateAndroid* DevToolsManagerDelegateAndroid::GetInstance() {
  return g_instance;
}

DevToolsManagerDelegateAndroid::~DevToolsManagerDelegateAndroid() {
  // Clean up all OTR contexts on shutdown.
  for (auto& [context_id, profile] : otr_profiles_) {
    context_web_contents_.erase(context_id);
    if (profile) {
      ProfileDestroyer::DestroyOTRProfileWhenAppropriate(profile);
    }
  }
  otr_profiles_.clear();
  context_web_contents_.clear();
  profile_observation_.RemoveAllObservations();
  g_instance = nullptr;
}

content::BrowserContext*
DevToolsManagerDelegateAndroid::GetDefaultBrowserContext() {
  return ProfileManager::GetActiveUserProfile()->GetOriginalProfile();
}

content::BrowserContext*
DevToolsManagerDelegateAndroid::CreateBrowserContext() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (static_cast<int>(otr_profiles_.size()) >= kMaxOtrContexts) {
    return nullptr;
  }

  Profile* original_profile =
      ProfileManager::GetActiveUserProfile()->GetOriginalProfile();

  Profile* otr_profile = original_profile->GetOffTheRecordProfile(
      Profile::OTRProfileID::CreateUniqueForDevTools(),
      /*create_if_needed=*/true);

  const std::string& context_id = otr_profile->UniqueId();
  profile_observation_.AddObservation(otr_profile);
  otr_profiles_[context_id] = otr_profile;
  return otr_profile;
}

void DevToolsManagerDelegateAndroid::DisposeBrowserContext(
    content::BrowserContext* context,
    DisposeCallback callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  std::string context_id = context->UniqueId();
  auto it = otr_profiles_.find(context_id);
  if (it == otr_profiles_.end()) {
    std::move(callback).Run(
        false, "Failed to find browser context with id " + context_id);
    return;
  }

  Profile* profile = it->second;

  // Destroy all WebContents for this context first.
  DestroyWebContentsForContext(context_id);

  // Stop observing and remove from tracking.
  if (profile_observation_.IsObservingSource(profile)) {
    profile_observation_.RemoveObservation(profile);
  }
  otr_profiles_.erase(it);

  // Schedule profile destruction.
  ProfileDestroyer::DestroyOTRProfileWhenAppropriateWithTimeout(
      profile, kDestroyProfileTimeout);

  std::move(callback).Run(true, "");
}

std::vector<content::BrowserContext*>
DevToolsManagerDelegateAndroid::GetBrowserContexts() {
  std::vector<content::BrowserContext*> result;
  for (const auto& [context_id, profile] : otr_profiles_) {
    result.push_back(profile);
  }
  return result;
}

scoped_refptr<content::DevToolsAgentHost>
DevToolsManagerDelegateAndroid::CreateNewTarget(
    const GURL& url,
    TargetType target_type,
    bool new_window) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  const TabModelList::TabModelVector& models = TabModelList::models();
  if (models.empty()) {
    return nullptr;
  }

  TabModel* tab_model = models[0];
  if (!tab_model) {
    return nullptr;
  }

  WebContents* web_contents =
      tab_model->CreateNewTabForDevTools(url, new_window);
  if (!web_contents) {
    return nullptr;
  }

  MarkCreatedByDevTools(*web_contents);

  return target_type == TargetType::kTab
             ? DevToolsAgentHost::GetOrCreateForTab(web_contents)
             : DevToolsAgentHost::GetOrCreateFor(web_contents);
}

content::WebContents* DevToolsManagerDelegateAndroid::CreateWebContentsForContext(
    const std::string& context_id,
    const GURL& url) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  auto it = otr_profiles_.find(context_id);
  if (it == otr_profiles_.end()) {
    return nullptr;
  }

  Profile* profile = it->second;

  content::WebContents::CreateParams params(profile);
  params.initially_hidden = true;
  std::unique_ptr<content::WebContents> web_contents =
      content::WebContents::Create(params);

  // Always navigate to ensure a renderer process is created.
  // Without this, DevTools commands like Runtime.enable will hang.
  GURL nav_url = url.is_empty() ? GURL(url::kAboutBlankURL) : url;
  content::NavigationController::LoadURLParams load_params(nav_url);
  load_params.transition_type = ui::PAGE_TRANSITION_AUTO_TOPLEVEL;
  web_contents->GetController().LoadURLWithParams(load_params);

  // Wake the compositor so BeginCommitCompositorFrame fires.
  // initially_hidden=true suppresses compositor commits — WasShown() re-enables them.
  web_contents->WasShown();

  MarkCreatedByDevTools(*web_contents);

  content::WebContents* raw_ptr = web_contents.get();
  context_web_contents_[context_id].push_back(std::move(web_contents));
  return raw_ptr;
}

Profile* DevToolsManagerDelegateAndroid::GetProfileForContext(
    const std::string& context_id) {
  auto it = otr_profiles_.find(context_id);
  if (it == otr_profiles_.end()) {
    return nullptr;
  }
  return it->second;
}

bool DevToolsManagerDelegateAndroid::IsCreatedByDevTools(
    const WebContents& web_contents) {
  return !!web_contents.GetUserData(kCreatedByDevTools);
}

void DevToolsManagerDelegateAndroid::MarkCreatedByDevTools(
    WebContents& web_contents) {
  DCHECK(!IsCreatedByDevTools(web_contents));
  web_contents.SetUserData(kCreatedByDevTools,
                           std::make_unique<base::SupportsUserData::Data>());
}

std::string DevToolsManagerDelegateAndroid::GetTargetType(
    content::WebContents* web_contents) {
  TabAndroid* tab = web_contents ? TabAndroid::FromWebContents(web_contents)
      : nullptr;
  return tab ? DevToolsAgentHost::kTypePage :
      DevToolsAgentHost::kTypeOther;
}

DevToolsAgentHost::List DevToolsManagerDelegateAndroid::RemoteDebuggingTargets(
    DevToolsManagerDelegate::TargetType target_type) {
  // Enumerate existing tabs, including the ones with no WebContents.
  DevToolsAgentHost::List result;
  std::set<WebContents*> tab_web_contents;
  for (const TabModel* model : TabModelList::models()) {
    for (int i = 0; i < model->GetTabCount(); ++i) {
      TabAndroid* tab = model->GetTabAt(i);
      if (!tab)
        continue;

      WebContents* wc = tab->web_contents();
      // For web contents created programmatically by CDP clients, do not create
      // tab proxies to avoid clients being confused by the fact they get more
      // targets than they create and match the behavior of desktop chrome.
      if (!wc || !IsCreatedByDevTools(*wc)) {
        result.push_back(DevToolsAgentHostForTab(
            tab, target_type == DevToolsManagerDelegate::kTab));
        if (wc) {
          tab_web_contents.insert(wc);
        }
      }
    }
  }

  // Add descriptors for targets not associated with any tabs.
  DevToolsAgentHost::List agents = DevToolsAgentHost::GetOrCreateAll();
  for (DevToolsAgentHost::List::iterator it = agents.begin();
       it != agents.end(); ++it) {
    if (WebContents* web_contents = (*it)->GetWebContents()) {
      if (tab_web_contents.find(web_contents) != tab_web_contents.end())
        continue;
    }
    result.push_back(*it);
  }

  return result;
}

bool DevToolsManagerDelegateAndroid::IsBrowserTargetDiscoverable() {
  return true;
}

void DevToolsManagerDelegateAndroid::HandleCommand(
    content::DevToolsAgentHostClientChannel* channel,
    base::span<const uint8_t> message,
    NotHandledCallback callback) {
  auto it = sessions_.find(channel);
  if (it == sessions_.end()) {
    // This should not happen, but happens. NOTREACHED tries to get
    // a repro in some test.
    NOTREACHED();
  }
  it->second->HandleCommand(message, std::move(callback));
}

void DevToolsManagerDelegateAndroid::ClientAttached(
    content::DevToolsAgentHostClientChannel* channel) {
  DCHECK(sessions_.find(channel) == sessions_.end());
  sessions_.emplace(channel,
                    std::make_unique<ChromeDevToolsSessionAndroid>(channel));
}

void DevToolsManagerDelegateAndroid::ClientDetached(
    content::DevToolsAgentHostClientChannel* channel) {
  sessions_.erase(channel);
}

void DevToolsManagerDelegateAndroid::OnProfileWillBeDestroyed(Profile* profile) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  std::string context_id = profile->UniqueId();

  // Clean up WebContents for this context.
  DestroyWebContentsForContext(context_id);

  // Remove from tracking.
  if (profile_observation_.IsObservingSource(profile)) {
    profile_observation_.RemoveObservation(profile);
  }
  otr_profiles_.erase(context_id);
}

void DevToolsManagerDelegateAndroid::DestroyWebContentsForContext(
    const std::string& context_id) {
  // Erasing the vector destroys all unique_ptrs, which destroys all
  // WebContents. This is safe because we initiate the destruction —
  // the WebContents are not mid-destruction.
  context_web_contents_.erase(context_id);
}
