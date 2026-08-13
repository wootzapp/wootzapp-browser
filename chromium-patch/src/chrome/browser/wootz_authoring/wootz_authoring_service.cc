// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wootz_authoring/wootz_authoring_service.h"

#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/wootz_authoring/wootz_authoring_client_state.h"
#include "chrome/common/chrome_switches.h"
#include "content/public/browser/storage_partition.h"
#include "url/origin.h"

namespace wootz_authoring {
namespace {

WootzAuthoringMode ModeFromCommandLine(const base::CommandLine& command_line) {
  if (command_line.HasSwitch(switches::kWootzAuthoringReplay)) {
    return WootzAuthoringMode::kReplay;
  }
  if (command_line.HasSwitch(switches::kWootzAuthoringCapture)) {
    return WootzAuthoringMode::kCapture;
  }
  return WootzAuthoringMode::kOff;
}

std::string SessionFromCommandLine(const base::CommandLine& command_line) {
  std::string session_id =
      command_line.GetSwitchValueASCII(switches::kWootzAuthoringSession);
  return session_id.empty() ? "default" : session_id;
}

const char* ModeForLogging(WootzAuthoringMode mode) {
  switch (mode) {
    case WootzAuthoringMode::kOff:
      return "off";
    case WootzAuthoringMode::kCapture:
      return "capture";
    case WootzAuthoringMode::kReplay:
      return "replay";
  }
}

// Forces a cold app state for capture: clears every origin's IndexedDB,
// CacheStorage, and service workers in the profile's default storage partition
void ClearProfileStorageForColdCapture(Profile* profile) {
  content::StoragePartition* partition = profile->GetDefaultStoragePartition();
  if (!partition) {
    return;
  }
  const uint32_t remove_mask =
      content::StoragePartition::REMOVE_DATA_MASK_INDEXEDDB |
      content::StoragePartition::REMOVE_DATA_MASK_CACHE_STORAGE |
      content::StoragePartition::REMOVE_DATA_MASK_SERVICE_WORKERS;
  LOG(ERROR) << " [WootzAuthoring] cold capture: clearing profile app storage "
                "(all origins; keeping cookies and localStorage)";
  partition->ClearData(
      remove_mask, nullptr,
      content::StoragePartition::StorageKeyPolicyMatcherFunction(), nullptr,
      false, base::Time(), base::Time::Max(), base::BindOnce([]() {
        LOG(ERROR)
            << " [WootzAuthoring] cold capture: profile storage cleared";
      }));
}

}  // namespace

WootzAuthoringService::WootzAuthoringService(Profile* profile)
    : profile_(profile) {
  const base::CommandLine& command_line =
      *base::CommandLine::ForCurrentProcess();
  mode_ = ModeFromCommandLine(command_line);
  store_ = std::make_unique<WootzAuthoringStore>(
      profile->GetPath(), SessionFromCommandLine(command_line));

  LOG(ERROR) << " [WootzAuthoring] service mode=" << ModeForLogging(mode_)
             << " session=" << store_->session_id();

  if (IsCaptureEnabled()) {
    ClearProfileStorageForColdCapture(profile);
  } else if (IsReplayEnabled()) {
    StartReplayBootstrap();
  }
}

WootzAuthoringService::~WootzAuthoringService() = default;

void WootzAuthoringService::Shutdown() {
  client_state_snapshot_timer_.Stop();
  profile_ = nullptr;
  replay_state_waiters_.clear();
}

void WootzAuthoringService::SaveCapturedResponse(
    const CapturedRequestInfo& request,
    const CapturedResponseInfo& response,
    std::string_view payload) {
  if (!IsCaptureEnabled()) {
    return;
  }
  RecordCapturedUrl(request.url);
  store_->SaveResponse(request, response, payload);
  ScheduleClientStateSnapshotForUrl(request.url);
}

void WootzAuthoringService::SaveCapturedRedirect(
    const CapturedRequestInfo& request,
    const CapturedResponseInfo& response,
    const GURL& redirect_url) {
  if (!IsCaptureEnabled()) {
    return;
  }
  RecordCapturedUrl(request.url);
  RecordCapturedUrl(redirect_url);
  store_->SaveRedirect(request, response, redirect_url);
  ScheduleClientStateSnapshotForUrl(redirect_url.is_valid() ? redirect_url
                                                            : request.url);
}

bool WootzAuthoringService::LoadReplayResponse(
    const network::ResourceRequest& request,
    ReplayResponse* response) {
  if (!IsReplayEnabled()) {
    return false;
  }
  return store_->LoadResponse(request, response);
}

void WootzAuthoringService::RecordSameDocumentRoute(const GURL& shell_url,
                                                    const GURL& route_url) {
  if (!IsCaptureEnabled()) {
    return;
  }
  RecordCapturedUrl(shell_url);
  RecordCapturedUrl(route_url);
  store_->RecordSameDocumentRoute(shell_url, route_url);
}

void WootzAuthoringService::SnapshotClientStateForUrl(const GURL& url) {
  if (!IsCaptureEnabled() || !profile_) {
    return;
  }
  RecordCapturedUrl(url);
  if (captured_origin_specs_.empty() && captured_hosts_.empty()) {
    return;
  }
  SnapshotClientStateForArchive(
      profile_, captured_origin_specs_, captured_hosts_,
      base::BindOnce(&WootzAuthoringService::OnClientStateSnapshot,
                     weak_factory_.GetWeakPtr()));
}

void WootzAuthoringService::ScheduleClientStateSnapshotForUrl(const GURL& url) {
  if (!IsCaptureEnabled() || !profile_) {
    return;
  }
  RecordCapturedUrl(url);
  pending_client_state_snapshot_url_ = url;
  client_state_snapshot_timer_.Start(
      FROM_HERE, base::Seconds(2),
      base::BindOnce(&WootzAuthoringService::RunScheduledClientStateSnapshot,
                     weak_factory_.GetWeakPtr()));
}

void WootzAuthoringService::RunWhenReplayStateReady(
    base::OnceClosure callback) {
  if (!IsReplayEnabled() || replay_state_ready_) {
    std::move(callback).Run();
    return;
  }
  replay_state_waiters_.push_back(std::move(callback));
  StartReplayBootstrap();
}

void WootzAuthoringService::RecordCapturedUrl(const GURL& url) {
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS()) {
    return;
  }
  captured_origin_specs_.insert(url::Origin::Create(url).Serialize());
  captured_hosts_.insert(base::ToLowerASCII(url.host()));
}

void WootzAuthoringService::StartReplayBootstrap() {
  if (!IsReplayEnabled() || replay_state_started_) {
    return;
  }
  replay_state_started_ = true;

  std::vector<WootzAuthoringStore::StorageState> storage_states =
      store_->LoadStorageStates();
  std::optional<base::ListValue> cookie_state = store_->LoadCookieState();
  LOG(ERROR) << " [WootzAuthoring] replay bootstrap: loaded archive state "
             << "local_origins=" << storage_states.size() << " cookies="
             << (cookie_state.has_value() ? cookie_state->size() : 0);

  ClearReplayClientState(
      profile_,
      base::BindOnce(&WootzAuthoringService::OnReplayClientStateCleared,
                     weak_factory_.GetWeakPtr(), std::move(storage_states),
                     std::move(cookie_state)));
}

void WootzAuthoringService::OnReplayClientStateCleared(
    std::vector<WootzAuthoringStore::StorageState> storage_states,
    std::optional<base::ListValue> cookie_state) {
  RestoreClientStateForReplay(
      profile_, std::move(storage_states), std::move(cookie_state),
      base::BindOnce(&WootzAuthoringService::OnReplayBootstrapComplete,
                     weak_factory_.GetWeakPtr()));
}

void WootzAuthoringService::OnReplayBootstrapComplete() {
  replay_state_ready_ = true;
  std::vector<base::OnceClosure> waiters;
  waiters.swap(replay_state_waiters_);
  LOG(ERROR) << " [WootzAuthoring] replay bootstrap: releasing waiters="
             << waiters.size();
  for (auto& waiter : waiters) {
    std::move(waiter).Run();
  }
}

void WootzAuthoringService::RunScheduledClientStateSnapshot() {
  SnapshotClientStateForUrl(pending_client_state_snapshot_url_);
}

void WootzAuthoringService::OnClientStateSnapshot(
    std::vector<WootzAuthoringStore::StorageState> storage_states,
    base::ListValue cookies) {
  if (!IsCaptureEnabled()) {
    return;
  }

  for (const WootzAuthoringStore::StorageState& state : storage_states) {
    store_->SaveStorageState(state.origin, state.local_storage);
  }
  if (!cookies.empty()) {
    store_->SaveCookieState(std::move(cookies));
  }
}

base::WeakPtr<WootzAuthoringService> WootzAuthoringService::GetWeakPtr() {
  return weak_factory_.GetWeakPtr();
}

}  // namespace wootz_authoring
