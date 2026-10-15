// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_URL_LOADER_FACTORY_H_
#define CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_URL_LOADER_FACTORY_H_

#include "content/public/browser/content_browser_client.h"

namespace content {
class BrowserContext;
class RenderFrameHost;
}  // namespace content

namespace network {
class URLLoaderFactoryBuilder;
}  // namespace network

namespace wootz_authoring {

void MaybeProxyAuthoringURLLoaderFactory(
    content::BrowserContext* browser_context,
    content::RenderFrameHost* frame,
    content::ContentBrowserClient::URLLoaderFactoryType type,
    network::URLLoaderFactoryBuilder& factory_builder);

}  // namespace wootz_authoring

#endif  // CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_URL_LOADER_FACTORY_H_
