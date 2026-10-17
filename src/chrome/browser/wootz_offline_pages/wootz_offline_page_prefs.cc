// Copyright 2024 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wootz_offline_pages/wootz_offline_page_prefs.h"

#include "base/command_line.h"
#include "chrome/common/chrome_switches.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"

namespace wootz_offline_pages {
namespace prefs {

void RegisterProfilePrefs(PrefRegistrySimple* registry) {
  // Register offline page saving preference, default to disabled
  registry->RegisterBooleanPref(kOfflineBrowsingEnabled, false);
  
  // Register auto-opening preference, default to disabled
  registry->RegisterBooleanPref(kAutoOpenOfflinePages, false);
}

bool IsAutoOpenOfflinePagesEnabled(PrefService* prefs) {
  const bool switch_enabled =
      base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kWootzAutoOpenOfflinePages);

  return switch_enabled ||
         (prefs && prefs->GetBoolean(kAutoOpenOfflinePages));
}

}  // namespace prefs
}  // namespace wootz_offline_pages
