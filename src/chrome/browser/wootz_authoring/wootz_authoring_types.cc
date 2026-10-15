// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wootz_authoring/wootz_authoring_types.h"

namespace wootz_authoring {

CapturedRequestInfo::CapturedRequestInfo() = default;
CapturedRequestInfo::CapturedRequestInfo(CapturedRequestInfo&&) = default;
CapturedRequestInfo& CapturedRequestInfo::operator=(CapturedRequestInfo&&) =
    default;
CapturedRequestInfo::~CapturedRequestInfo() = default;

CapturedResponseInfo::CapturedResponseInfo() = default;
CapturedResponseInfo::CapturedResponseInfo(CapturedResponseInfo&&) = default;
CapturedResponseInfo& CapturedResponseInfo::operator=(CapturedResponseInfo&&) =
    default;
CapturedResponseInfo::~CapturedResponseInfo() = default;

ReplayResponse::ReplayResponse() = default;
ReplayResponse::ReplayResponse(ReplayResponse&&) = default;
ReplayResponse& ReplayResponse::operator=(ReplayResponse&&) = default;
ReplayResponse::~ReplayResponse() = default;

}  // namespace wootz_authoring
