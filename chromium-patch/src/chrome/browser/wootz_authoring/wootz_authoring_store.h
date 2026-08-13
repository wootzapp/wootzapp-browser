// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_STORE_H_
#define CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_STORE_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/sequence_checker.h"
#include "base/values.h"
#include "chrome/browser/wootz_authoring/wootz_authoring_types.h"
#include "services/network/public/cpp/resource_request.h"

namespace wootz_authoring {

class WootzAuthoringStore {
 public:
  WootzAuthoringStore(const base::FilePath& profile_path,
                      std::string session_id);
  WootzAuthoringStore(const WootzAuthoringStore&) = delete;
  WootzAuthoringStore& operator=(const WootzAuthoringStore&) = delete;
  ~WootzAuthoringStore();

  static constexpr char kNoRequestBodyDigest[] = "none";

  struct StorageEntry {
    StorageEntry();
    StorageEntry(const StorageEntry&);
    StorageEntry& operator=(const StorageEntry&);
    StorageEntry(StorageEntry&&);
    StorageEntry& operator=(StorageEntry&&);
    ~StorageEntry();

    std::string key_base64;
    std::string value_base64;
  };

  struct StorageState {
    StorageState();
    StorageState(const StorageState&);
    StorageState& operator=(const StorageState&);
    StorageState(StorageState&&);
    StorageState& operator=(StorageState&&);
    ~StorageState();

    std::string origin;
    std::vector<StorageEntry> local_storage;
  };

  static std::string MakeRequestKey(const network::ResourceRequest& request);
  static std::string MakeRequestKey(std::string_view method, const GURL& url);
  static std::string MakeRequestKey(std::string_view method,
                                    const GURL& url,
                                    std::string_view request_body_digest);
  static std::string MakeEndpointKey(const GURL& url);
  static base::DictValue QueryFieldsForUrl(const GURL& url);
  static base::DictValue RequestBodyFieldsForKey(
      const network::ResourceRequest& request);
  static std::string MakeNormalizedRequestKey(
      std::string_view method,
      const GURL& url,
      const base::DictValue& request_body_fields,
      std::string_view request_body_digest);
  static std::string RequestBodyDigestForKey(
      const network::ResourceRequest& request);

  const std::string& session_id() const { return session_id_; }
  const base::FilePath& session_dir() const { return session_dir_; }

  bool SaveResponse(const CapturedRequestInfo& request,
                    const CapturedResponseInfo& response,
                    std::string_view payload);
  bool SaveRedirect(const CapturedRequestInfo& request,
                    const CapturedResponseInfo& response,
                    const GURL& redirect_url);
  bool LoadResponse(const network::ResourceRequest& request,
                    ReplayResponse* response);

  // Browser state captured outside the URL-loader response path. Stored in the
  // same index.jsonl as response records so moving an archive to another
  // profile carries login-critical state with it. localStorage bytes are raw
  // Blink storage bytes encoded as base64, including Blink's internal format
  // tag.
  bool SaveStorageState(const std::string& origin,
                        const std::vector<StorageEntry>& local_storage);
  bool SaveCookieState(base::ListValue cookies);
  std::vector<StorageState> LoadStorageStates();
  std::optional<base::ListValue> LoadCookieState();

  // Records a same-document (SPA) navigation observed by the tab helper:
  // |route_url| was reached via pushState/replaceState/hash from |shell_url|
  // without any network fetch. Persisted as a "same_document_route" record in
  // the existing index.jsonl so replay can serve the captured shell for SPA
  // deep-link reloads.
  void RecordSameDocumentRoute(const GURL& shell_url, const GURL& route_url);

 private:
  struct StoredEntry {
    StoredEntry();
    StoredEntry(const StoredEntry&) = delete;
    StoredEntry& operator=(const StoredEntry&) = delete;
    StoredEntry(StoredEntry&&);
    StoredEntry& operator=(StoredEntry&&);
    ~StoredEntry();

    int status_code = 0;
    GURL url;
    std::string method;
    std::string normalized_request_key;
    std::string endpoint_key;
    std::string request_body_digest;
    base::DictValue query_fields;
    base::DictValue request_body_fields;
    int64_t entry_order = 0;
    std::string mime_type;
    std::string raw_response_headers;
    std::string payload_digest;
    std::string redirect_url;
  };

  bool EnsureInitialized();
  bool LoadIndexIfNeeded();
  void AddEntryToIndexes(const std::string& request_key,
                         const StoredEntry& entry);
  std::map<std::string, StoredEntry>::const_iterator FindNormalizedResponse(
      const network::ResourceRequest& request,
      std::string_view request_body_digest,
      const base::DictValue& request_body_fields) const;
  std::map<std::string, StoredEntry>::const_iterator FindScoredResponse(
      const network::ResourceRequest& request,
      const base::DictValue& query_fields,
      const base::DictValue& request_body_fields) const;
  std::map<std::string, StoredEntry>::const_iterator FindDocumentFallback(
      const network::ResourceRequest& request,
      std::string_view request_body_digest) const;
  std::map<std::string, StoredEntry>::const_iterator FindSinglePathCandidate(
      const network::ResourceRequest& request) const;
  std::map<std::string, StoredEntry>::const_iterator
  FindStaticAssetQueryFallback(const network::ResourceRequest& request) const;
  std::map<std::string, StoredEntry>::const_iterator FindDocumentQueryFallback(
      const network::ResourceRequest& request) const;
  std::map<std::string, StoredEntry>::const_iterator FindSpaShellPrefixFallback(
      const network::ResourceRequest& request) const;
  base::FilePath PayloadPathForDigest(const std::string& payload_digest) const;

  std::string session_id_;
  base::FilePath session_dir_;
  base::FilePath payloads_dir_;
  base::FilePath index_path_;
  bool initialized_ = false;
  bool index_loaded_ = false;
  int64_t next_entry_order_ = 0;
  std::map<std::string, StoredEntry> entries_;
  std::map<std::string, std::string> normalized_entries_;
  std::map<std::string, std::vector<std::string>> entries_by_endpoint_;
  // Same-document (SPA) routes: route_url -> shell_url. Populated from
  // "same_document_route" records in index.jsonl; used by replay to serve the
  // captured shell for deep-link reloads that were never fetched as documents.
  std::map<std::string, std::string> same_document_routes_;
  std::map<std::string, StorageState> storage_states_by_origin_;
  std::optional<base::ListValue> cookie_state_;

  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace wootz_authoring

#endif  // CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_STORE_H_
