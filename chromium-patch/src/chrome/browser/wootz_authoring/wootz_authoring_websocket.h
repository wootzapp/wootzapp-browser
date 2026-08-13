// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_WEBSOCKET_H_
#define CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_WEBSOCKET_H_

#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/websocket.mojom.h"

class GURL;

namespace wootz_authoring {

// Synthesizes a successful, silent WebSocket handshake without touching the
// network. Used during replay so a captured page (e.g. Slack) believes its
// realtime socket is connected while the device is offline: the page receives
// an "open" event, every frame it sends is discarded, and no frame is ever
// delivered back to it. The synthetic socket stays open until the page closes
// it or navigates away.
void CreateReplayWebSocket(
    const GURL& url,
    mojo::PendingRemote<network::mojom::WebSocketHandshakeClient>
        handshake_client);

}  // namespace wootz_authoring

#endif  // CHROME_BROWSER_WOOTZ_AUTHORING_WOOTZ_AUTHORING_WEBSOCKET_H_
