// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_CLIENT_STATE_H_
#define CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_CLIENT_STATE_H_

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/values.h"
#include "chrome/browser/wootz_authoring/wootz_authoring_store.h"

class Profile;

namespace wootz_authoring {

using ClientStateDoneCallback = base::OnceClosure;
using ClientStateSnapshotCallback = base::OnceCallback<void(
    std::vector<WootzAuthoringStore::StorageState> storage_states,
    base::ListValue cookies)>;

void SnapshotClientStateForArchive(
    Profile* profile,
    const std::set<std::string>& captured_origins,
    const std::set<std::string>& captured_hosts,
    ClientStateSnapshotCallback callback);

void ClearReplayClientState(Profile* profile, ClientStateDoneCallback callback);

void RestoreClientStateForReplay(
    Profile* profile,
    std::vector<WootzAuthoringStore::StorageState> storage_states,
    std::optional<base::ListValue> cookie_state,
    ClientStateDoneCallback callback);

}  // namespace wootz_authoring

#endif  // CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_CLIENT_STATE_H_
