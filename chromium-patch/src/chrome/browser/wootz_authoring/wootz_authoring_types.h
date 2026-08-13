// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_TYPES_H_
#define CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_TYPES_H_

#include <string>

#include "base/time/time.h"
#include "base/values.h"
#include "url/gurl.h"

namespace wootz_authoring {

enum class WootzAuthoringMode {
  kOff,
  kCapture,
  kReplay,
};

struct CapturedRequestInfo {
  CapturedRequestInfo();
  CapturedRequestInfo(const CapturedRequestInfo&) = delete;
  CapturedRequestInfo& operator=(const CapturedRequestInfo&) = delete;
  CapturedRequestInfo(CapturedRequestInfo&&);
  CapturedRequestInfo& operator=(CapturedRequestInfo&&);
  ~CapturedRequestInfo();

  std::string request_key;
  std::string normalized_request_key;
  std::string endpoint_key;
  GURL url;
  std::string method;
  std::string request_body_digest;
  base::DictValue query_fields;
  base::DictValue request_body_fields;
  base::DictValue request_headers;
  std::string resource_type;
  std::string request_destination;
  std::string factory_type;
  std::string request_initiator;
  base::Time timestamp;
};

struct CapturedResponseInfo {
  CapturedResponseInfo();
  CapturedResponseInfo(const CapturedResponseInfo&) = delete;
  CapturedResponseInfo& operator=(const CapturedResponseInfo&) = delete;
  CapturedResponseInfo(CapturedResponseInfo&&);
  CapturedResponseInfo& operator=(CapturedResponseInfo&&);
  ~CapturedResponseInfo();

  int status_code = 0;
  std::string mime_type;
  std::string charset;
  std::string raw_response_headers;
  base::DictValue response_headers;
  int net_error = 0;
};

struct ReplayResponse {
  ReplayResponse();
  ReplayResponse(const ReplayResponse&) = delete;
  ReplayResponse& operator=(const ReplayResponse&) = delete;
  ReplayResponse(ReplayResponse&&);
  ReplayResponse& operator=(ReplayResponse&&);
  ~ReplayResponse();

  int status_code = 0;
  std::string mime_type;
  std::string raw_response_headers;
  std::string payload;
  std::string redirect_url;
};

}  // namespace wootz_authoring

#endif  // CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_TYPES_H_
