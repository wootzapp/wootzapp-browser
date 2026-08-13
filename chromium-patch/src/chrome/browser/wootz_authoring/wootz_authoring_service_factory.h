// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_SERVICE_FACTORY_H_
#define CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_SERVICE_FACTORY_H_

#include <memory>

#include "base/no_destructor.h"
#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class Profile;

namespace wootz_authoring {

class WootzAuthoringService;

class WootzAuthoringServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static WootzAuthoringService* GetForProfile(Profile* profile);
  static WootzAuthoringServiceFactory* GetInstance();

 private:
  friend class base::NoDestructor<WootzAuthoringServiceFactory>;

  WootzAuthoringServiceFactory();
  ~WootzAuthoringServiceFactory() override;

  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;

  bool ServiceIsCreatedWithBrowserContext() const override;
};

}  // namespace wootz_authoring

#endif  // CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_SERVICE_FACTORY_H_
