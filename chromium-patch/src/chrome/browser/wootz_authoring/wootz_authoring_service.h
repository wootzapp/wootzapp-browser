// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_SERVICE_H_
#define CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_SERVICE_H_

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "chrome/browser/wootz_authoring/wootz_authoring_store.h"
#include "chrome/browser/wootz_authoring/wootz_authoring_types.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace network {
struct ResourceRequest;
}

namespace wootz_authoring {

class WootzAuthoringService : public KeyedService {
 public:
  explicit WootzAuthoringService(Profile* profile);
  WootzAuthoringService(const WootzAuthoringService&) = delete;
  WootzAuthoringService& operator=(const WootzAuthoringService&) = delete;
  ~WootzAuthoringService() override;

  // KeyedService:
  void Shutdown() override;

  WootzAuthoringMode mode() const { return mode_; }
  bool IsEnabled() const { return mode_ != WootzAuthoringMode::kOff; }
  bool IsCaptureEnabled() const {
    return mode_ == WootzAuthoringMode::kCapture;
  }
  bool IsReplayEnabled() const { return mode_ == WootzAuthoringMode::kReplay; }
  const std::string& session_id() const { return store_->session_id(); }

  void SaveCapturedResponse(const CapturedRequestInfo& request,
                            const CapturedResponseInfo& response,
                            std::string_view payload);
  void SaveCapturedRedirect(const CapturedRequestInfo& request,
                            const CapturedResponseInfo& response,
                            const GURL& redirect_url);
  bool LoadReplayResponse(const network::ResourceRequest& request,
                          ReplayResponse* response);

  void RecordSameDocumentRoute(const GURL& shell_url, const GURL& route_url);

  void SnapshotClientStateForUrl(const GURL& url);
  void ScheduleClientStateSnapshotForUrl(const GURL& url);
  void RunWhenReplayStateReady(base::OnceClosure callback);

  base::WeakPtr<WootzAuthoringService> GetWeakPtr();

 private:
  void RecordCapturedUrl(const GURL& url);
  void StartReplayBootstrap();
  void OnReplayClientStateCleared(
      std::vector<WootzAuthoringStore::StorageState> storage_states,
      std::optional<base::ListValue> cookie_state);
  void OnReplayBootstrapComplete();
  void RunScheduledClientStateSnapshot();
  void OnClientStateSnapshot(
      std::vector<WootzAuthoringStore::StorageState> storage_states,
      base::ListValue cookies);

  WootzAuthoringMode mode_ = WootzAuthoringMode::kOff;
  raw_ptr<Profile> profile_ = nullptr;
  std::unique_ptr<WootzAuthoringStore> store_;
  std::set<std::string> captured_origin_specs_;
  std::set<std::string> captured_hosts_;
  bool replay_state_started_ = false;
  bool replay_state_ready_ = false;
  std::vector<base::OnceClosure> replay_state_waiters_;
  GURL pending_client_state_snapshot_url_;
  base::OneShotTimer client_state_snapshot_timer_;
  base::WeakPtrFactory<WootzAuthoringService> weak_factory_{this};
};

}  // namespace wootz_authoring

#endif  // CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_SERVICE_H_
