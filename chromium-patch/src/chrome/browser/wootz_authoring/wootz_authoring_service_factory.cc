// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wootz_authoring/wootz_authoring_service_factory.h"

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/wootz_authoring/wootz_authoring_service.h"

namespace wootz_authoring {

// static
WootzAuthoringService* WootzAuthoringServiceFactory::GetForProfile(
    Profile* profile) {
  return static_cast<WootzAuthoringService*>(
      GetInstance()->GetServiceForBrowserContext(profile, true));
}

// static
WootzAuthoringServiceFactory* WootzAuthoringServiceFactory::GetInstance() {
  static base::NoDestructor<WootzAuthoringServiceFactory> instance;
  return instance.get();
}

WootzAuthoringServiceFactory::WootzAuthoringServiceFactory()
    : ProfileKeyedServiceFactory(
          "WootzAuthoringService",
          ProfileSelections::Builder()
              .WithRegular(ProfileSelection::kOwnInstance)
              .Build()) {}

WootzAuthoringServiceFactory::~WootzAuthoringServiceFactory() = default;

std::unique_ptr<KeyedService>
WootzAuthoringServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<WootzAuthoringService>(
      Profile::FromBrowserContext(context));
}

bool WootzAuthoringServiceFactory::ServiceIsCreatedWithBrowserContext() const {
  return true;
}

}  // namespace wootz_authoring
