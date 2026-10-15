// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wootz_authoring/wootz_authoring_client_state.h"

#include <stdint.h>

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "components/services/storage/public/mojom/local_storage_control.mojom.h"
#include "components/services/storage/public/mojom/storage_usage_info.mojom.h"
#include "content/public/browser/storage_partition.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_access_result.h"
#include "net/cookies/cookie_options.h"
#include "net/cookies/cookie_partition_key.h"
#include "net/cookies/cookie_util.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/dom_storage/storage_area.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace wootz_authoring {
namespace {

int64_t TimeToWindowsEpochMicros(base::Time time) {
  return time.ToDeltaSinceWindowsEpoch().InMicroseconds();
}

base::Time WindowsEpochMicrosToTime(int64_t micros) {
  return base::Time::FromDeltaSinceWindowsEpoch(base::Microseconds(micros));
}

std::string TimeToString(base::Time time) {
  return base::NumberToString(TimeToWindowsEpochMicros(time));
}

std::optional<int64_t> ReadInt64String(const base::DictValue& dict,
                                       std::string_view key) {
  const std::string* value = dict.FindString(key);
  if (!value) {
    return std::nullopt;
  }
  int64_t parsed = 0;
  if (!base::StringToInt64(*value, &parsed)) {
    return std::nullopt;
  }
  return parsed;
}

int ReadInt(const base::DictValue& dict, std::string_view key, int fallback) {
  return dict.FindInt(key).value_or(fallback);
}

bool ReadBool(const base::DictValue& dict,
              std::string_view key,
              bool fallback) {
  return dict.FindBool(key).value_or(fallback);
}

bool CookieMatchesCapturedHosts(const net::CanonicalCookie& cookie,
                                const std::set<std::string>& captured_hosts) {
  if (captured_hosts.empty()) {
    return false;
  }

  const std::string cookie_host = base::ToLowerASCII(cookie.DomainWithoutDot());
  if (cookie_host.empty()) {
    return false;
  }

  for (const std::string& captured_host : captured_hosts) {
    if (captured_host == cookie_host ||
        base::EndsWith(captured_host, "." + cookie_host,
                       base::CompareCase::SENSITIVE)) {
      return true;
    }
  }
  return false;
}

std::optional<base::DictValue> CookieToDict(
    const net::CanonicalCookie& cookie) {
  base::expected<net::CookiePartitionKey::SerializedCookiePartitionKey,
                 std::string>
      partition_key = net::CookiePartitionKey::Serialize(cookie.PartitionKey());
  if (!partition_key.has_value()) {
    return std::nullopt;
  }

  base::DictValue dict;
  dict.Set("name", cookie.Name());
  dict.Set("value", cookie.Value());
  dict.Set("domain", cookie.Domain());
  dict.Set("path", cookie.Path());
  dict.Set("creation_us", TimeToString(cookie.CreationDate()));
  dict.Set("expiration_us", TimeToString(cookie.ExpiryDate()));
  dict.Set("last_access_us", TimeToString(cookie.LastAccessDate()));
  dict.Set("last_update_us", TimeToString(cookie.LastUpdateDate()));
  dict.Set("secure", cookie.SecureAttribute());
  dict.Set("httponly", cookie.IsHttpOnly());
  dict.Set("same_site", static_cast<int>(cookie.SameSite()));
  dict.Set("priority", static_cast<int>(cookie.Priority()));
  dict.Set("partition_key", partition_key->TopLevelSite());
  dict.Set("partition_has_cross_site_ancestor",
           partition_key->has_cross_site_ancestor());
  dict.Set("source_scheme", static_cast<int>(cookie.SourceScheme()));
  dict.Set("source_port", cookie.SourcePort());
  dict.Set("source_type", static_cast<int>(cookie.SourceType()));
  return dict;
}

std::unique_ptr<net::CanonicalCookie> CookieFromDict(
    const base::DictValue& dict) {
  const std::string* name = dict.FindString("name");
  const std::string* value = dict.FindString("value");
  const std::string* domain = dict.FindString("domain");
  const std::string* path = dict.FindString("path");
  if (!name || !value || !domain || !path) {
    return nullptr;
  }

  std::optional<int64_t> creation = ReadInt64String(dict, "creation_us");
  std::optional<int64_t> expiration = ReadInt64String(dict, "expiration_us");
  std::optional<int64_t> last_access = ReadInt64String(dict, "last_access_us");
  std::optional<int64_t> last_update = ReadInt64String(dict, "last_update_us");
  if (!creation || !expiration || !last_access || !last_update) {
    return nullptr;
  }

  const std::string* partition_key = dict.FindString("partition_key");
  const bool has_cross_site_ancestor =
      ReadBool(dict, "partition_has_cross_site_ancestor", /*fallback=*/false);
  base::expected<std::optional<net::CookiePartitionKey>, std::string>
      deserialized_partition_key = net::CookiePartitionKey::FromStorage(
          partition_key ? *partition_key : std::string(),
          has_cross_site_ancestor);
  if (!deserialized_partition_key.has_value()) {
    return nullptr;
  }

  return net::CanonicalCookie::FromStorage(
      *name, *value, *domain, *path, WindowsEpochMicrosToTime(*creation),
      WindowsEpochMicrosToTime(*expiration),
      WindowsEpochMicrosToTime(*last_access),
      WindowsEpochMicrosToTime(*last_update),
      ReadBool(dict, "secure", /*fallback=*/false),
      ReadBool(dict, "httponly", /*fallback=*/false),
      static_cast<net::CookieSameSite>(
          ReadInt(dict, "same_site",
                  static_cast<int>(net::CookieSameSite::UNSPECIFIED))),
      static_cast<net::CookiePriority>(
          ReadInt(dict, "priority", net::COOKIE_PRIORITY_DEFAULT)),
      deserialized_partition_key.value(),
      static_cast<net::CookieSourceScheme>(
          ReadInt(dict, "source_scheme",
                  static_cast<int>(net::CookieSourceScheme::kUnset))),
      ReadInt(dict, "source_port", url::PORT_UNSPECIFIED),
      static_cast<net::CookieSourceType>(
          ReadInt(dict, "source_type",
                  static_cast<int>(net::CookieSourceType::kOther))),
      net::CanonicalCookieFromStorageCallSite::kCookieManager);
}

class SnapshotOperation : public base::RefCounted<SnapshotOperation> {
 public:
  SnapshotOperation(Profile* profile,
                    std::set<std::string> captured_origins,
                    std::set<std::string> captured_hosts,
                    ClientStateSnapshotCallback callback)
      : profile_(profile),
        captured_origins_(std::move(captured_origins)),
        captured_hosts_(std::move(captured_hosts)),
        callback_(std::move(callback)) {}

  SnapshotOperation(const SnapshotOperation&) = delete;
  SnapshotOperation& operator=(const SnapshotOperation&) = delete;

  void Start() {
    if (!profile_) {
      FinishIfReady();
      return;
    }

    content::StoragePartition* partition =
        profile_->GetDefaultStoragePartition();
    if (!partition) {
      FinishIfReady();
      return;
    }

    storage::mojom::LocalStorageControl* local_storage_control =
        partition->GetLocalStorageControl();
    if (local_storage_control && !captured_origins_.empty()) {
      AddPending();
      local_storage_control->GetUsage(
          base::BindOnce(&SnapshotOperation::OnLocalStorageUsage,
                         base::WrapRefCounted(this), local_storage_control));
    }

    network::mojom::CookieManager* cookie_manager =
        partition->GetCookieManagerForBrowserProcess();
    if (cookie_manager && !captured_hosts_.empty()) {
      AddPending();
      cookie_manager->GetAllCookies(base::BindOnce(
          &SnapshotOperation::OnCookies, base::WrapRefCounted(this)));
    }

    started_finishing_ = true;
    FinishIfReady();
  }

 private:
  friend class base::RefCounted<SnapshotOperation>;
  ~SnapshotOperation() = default;

  void AddPending() { ++pending_; }

  void CompleteOne() {
    if (pending_ == 0) {
      return;
    }
    --pending_;
    FinishIfReady();
  }

  void FinishIfReady() {
    if (!started_finishing_ || pending_ != 0 || !callback_) {
      return;
    }
    std::move(callback_).Run(std::move(storage_states_), std::move(cookies_));
  }

  void OnLocalStorageUsage(
      storage::mojom::LocalStorageControl* local_storage_control,
      std::vector<storage::mojom::StorageUsageInfoPtr> usage_info) {
    if (local_storage_control) {
      for (const auto& usage : usage_info) {
        if (!usage) {
          continue;
        }
        const std::string origin = usage->storage_key.origin().Serialize();
        if (!captured_origins_.contains(origin)) {
          continue;
        }

        mojo::Remote<blink::mojom::StorageArea>& area =
            storage_areas_.emplace_back();
        local_storage_control->BindStorageArea(
            usage->storage_key, area.BindNewPipeAndPassReceiver());
        AddPending();
        area->GetAll(mojo::NullRemote(),
                     base::BindOnce(&SnapshotOperation::OnStorageArea,
                                    base::WrapRefCounted(this), origin));
      }
    }
    CompleteOne();
  }

  void OnStorageArea(std::string origin,
                     std::vector<blink::mojom::KeyValuePtr> key_values) {
    WootzAuthoringStore::StorageState state;
    state.origin = std::move(origin);
    for (const auto& key_value : key_values) {
      if (!key_value || key_value->key.empty()) {
        continue;
      }
      WootzAuthoringStore::StorageEntry entry;
      entry.key_base64 = base::Base64Encode(base::span(key_value->key));
      entry.value_base64 = base::Base64Encode(base::span(key_value->value));
      state.local_storage.push_back(std::move(entry));
    }
    storage_states_.push_back(std::move(state));
    CompleteOne();
  }

  void OnCookies(const net::CookieList& cookies) {
    for (const net::CanonicalCookie& cookie : cookies) {
      if (!CookieMatchesCapturedHosts(cookie, captured_hosts_)) {
        continue;
      }
      std::optional<base::DictValue> serialized = CookieToDict(cookie);
      if (!serialized.has_value()) {
        continue;
      }
      cookies_.Append(std::move(*serialized));
    }
    CompleteOne();
  }

  raw_ptr<Profile> profile_ = nullptr;
  std::set<std::string> captured_origins_;
  std::set<std::string> captured_hosts_;
  ClientStateSnapshotCallback callback_;
  size_t pending_ = 0;
  bool started_finishing_ = false;
  std::vector<mojo::Remote<blink::mojom::StorageArea>> storage_areas_;
  std::vector<WootzAuthoringStore::StorageState> storage_states_;
  base::ListValue cookies_;
};

class RestoreOperation : public base::RefCounted<RestoreOperation> {
 public:
  RestoreOperation(
      Profile* profile,
      std::vector<WootzAuthoringStore::StorageState> storage_states,
      std::optional<base::ListValue> cookie_state,
      ClientStateDoneCallback callback)
      : profile_(profile),
        storage_states_(std::move(storage_states)),
        cookie_state_(std::move(cookie_state)),
        callback_(std::move(callback)) {}

  RestoreOperation(const RestoreOperation&) = delete;
  RestoreOperation& operator=(const RestoreOperation&) = delete;

  void Start() {
    if (!profile_) {
      FinishIfReady();
      return;
    }

    content::StoragePartition* partition =
        profile_->GetDefaultStoragePartition();
    if (!partition) {
      FinishIfReady();
      return;
    }

    storage::mojom::LocalStorageControl* local_storage_control =
        partition->GetLocalStorageControl();
    if (local_storage_control) {
      for (const WootzAuthoringStore::StorageState& state : storage_states_) {
        const GURL origin_url(state.origin);
        if (!origin_url.is_valid() || !origin_url.SchemeIsHTTPOrHTTPS()) {
          continue;
        }

        mojo::Remote<blink::mojom::StorageArea>& area =
            storage_areas_.emplace_back();
        local_storage_control->BindStorageArea(
            blink::StorageKey::CreateFirstParty(
                url::Origin::Create(origin_url)),
            area.BindNewPipeAndPassReceiver());

        for (const WootzAuthoringStore::StorageEntry& entry :
             state.local_storage) {
          std::optional<std::vector<uint8_t>> key =
              base::Base64Decode(entry.key_base64);
          std::optional<std::vector<uint8_t>> value =
              base::Base64Decode(entry.value_base64);
          if (!key.has_value() || !value.has_value() || key->empty()) {
            continue;
          }
          AddPending();
          area->Put(std::move(*key), std::move(*value), std::nullopt,
                    nullptr,
                    base::BindOnce(&RestoreOperation::OnLocalStoragePut,
                                   base::WrapRefCounted(this)));
        }
      }
    }

    network::mojom::CookieManager* cookie_manager =
        partition->GetCookieManagerForBrowserProcess();
    if (cookie_manager && cookie_state_.has_value()) {
      for (const base::Value& cookie_value : *cookie_state_) {
        if (!cookie_value.is_dict()) {
          continue;
        }
        const base::DictValue& cookie_dict = cookie_value.GetDict();
        std::unique_ptr<net::CanonicalCookie> cookie =
            CookieFromDict(cookie_dict);
        if (!cookie) {
          continue;
        }
        const std::string* domain = cookie_dict.FindString("domain");
        const std::string* path = cookie_dict.FindString("path");
        if (!domain || !path) {
          continue;
        }
        const net::CookieSourceScheme source_scheme =
            static_cast<net::CookieSourceScheme>(
                ReadInt(cookie_dict, "source_scheme",
                        static_cast<int>(net::CookieSourceScheme::kUnset)));
        const GURL source_url = net::cookie_util::CookieDomainAndPathToURL(
            *domain, *path, source_scheme);
        if (!source_url.is_valid()) {
          continue;
        }

        AddPending();
        cookie_manager->SetCanonicalCookie(
            *cookie, source_url, net::CookieOptions::MakeAllInclusive(),
            base::BindOnce(&RestoreOperation::OnCookieSet,
                           base::WrapRefCounted(this)));
      }
    }

    started_finishing_ = true;
    FinishIfReady();
  }

 private:
  friend class base::RefCounted<RestoreOperation>;
  ~RestoreOperation() = default;

  void AddPending() { ++pending_; }

  void CompleteOne() {
    if (pending_ == 0) {
      return;
    }
    --pending_;
    FinishIfReady();
  }

  void FinishIfReady() {
    if (!started_finishing_ || pending_ != 0 || !callback_) {
      return;
    }
    LOG(ERROR) << " [WootzAuthoring] replay client state seeded local_origins="
               << storage_states_.size() << " cookies="
               << (cookie_state_.has_value() ? cookie_state_->size() : 0);
    std::move(callback_).Run();
  }

  void OnLocalStoragePut(bool success) {
    if (!success) {
      LOG(ERROR) << " [WootzAuthoring] localStorage Put failed during replay "
                    "state restore";
    }
    CompleteOne();
  }

  void OnCookieSet(net::CookieAccessResult access_result) {
    if (!access_result.status.IsInclude()) {
      LOG(ERROR) << " [WootzAuthoring] cookie restore blocked status="
                 << access_result.status.GetDebugString();
    }
    CompleteOne();
  }

  raw_ptr<Profile> profile_ = nullptr;
  std::vector<WootzAuthoringStore::StorageState> storage_states_;
  std::optional<base::ListValue> cookie_state_;
  ClientStateDoneCallback callback_;
  size_t pending_ = 0;
  bool started_finishing_ = false;
  std::vector<mojo::Remote<blink::mojom::StorageArea>> storage_areas_;
};

}  // namespace

void SnapshotClientStateForArchive(
    Profile* profile,
    const std::set<std::string>& captured_origins,
    const std::set<std::string>& captured_hosts,
    ClientStateSnapshotCallback callback) {
  scoped_refptr<SnapshotOperation> operation =
      base::MakeRefCounted<SnapshotOperation>(
          profile, captured_origins, captured_hosts, std::move(callback));
  operation->Start();
}

void ClearReplayClientState(Profile* profile,
                            ClientStateDoneCallback callback) {
  if (!profile) {
    std::move(callback).Run();
    return;
  }

  content::StoragePartition* partition = profile->GetDefaultStoragePartition();
  if (!partition) {
    std::move(callback).Run();
    return;
  }

  const uint32_t remove_mask =
      content::StoragePartition::REMOVE_DATA_MASK_INDEXEDDB |
      content::StoragePartition::REMOVE_DATA_MASK_CACHE_STORAGE |
      content::StoragePartition::REMOVE_DATA_MASK_SERVICE_WORKERS |
      content::StoragePartition::REMOVE_DATA_MASK_LOCAL_STORAGE |
      content::StoragePartition::REMOVE_DATA_MASK_COOKIES;
  LOG(ERROR) << " [WootzAuthoring] replay bootstrap: clearing app storage, "
                "cookies, and localStorage before archive state restore";
  auto cookie_deletion_filter = network::mojom::CookieDeletionFilter::New();
  partition->ClearData(
      remove_mask,
      nullptr, content::StoragePartition::StorageKeyPolicyMatcherFunction(),
      std::move(cookie_deletion_filter), false, base::Time(), base::Time::Max(),
      std::move(callback));
}

void RestoreClientStateForReplay(
    Profile* profile,
    std::vector<WootzAuthoringStore::StorageState> storage_states,
    std::optional<base::ListValue> cookie_state,
    ClientStateDoneCallback callback) {
  scoped_refptr<RestoreOperation> operation =
      base::MakeRefCounted<RestoreOperation>(profile, std::move(storage_states),
                                             std::move(cookie_state),
                                             std::move(callback));
  operation->Start();
}

}  // namespace wootz_authoring
