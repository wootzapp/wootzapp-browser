// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wootz_authoring/wootz_authoring_websocket.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/handle_signals_state.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/http/http_version.h"
#include "services/network/public/mojom/websocket.mojom.h"
#include "url/gurl.h"

namespace wootz_authoring {

namespace {

// A stand-in network::mojom::WebSocket used only during replay. The renderer is
// told the connection is established, then this object silently discards every
// outgoing frame and never delivers an incoming one. The page's realtime socket
// therefore looks "connected" while the device is offline.
//
// This is the mojo-layer equivalent of the extension's JavaScript pacifier that
// replaces window.WebSocket: report open, swallow sends, deliver nothing. The
// one difference is that we drive the real renderer-side WebSocket object, so we
// must complete a close (OnDropChannel) rather than ignore it, or the page would
// hang in the CLOSING state.
class ReplayWebSocket : public network::mojom::WebSocket {
 public:
  ReplayWebSocket(mojo::PendingRemote<network::mojom::WebSocketClient> client,
                  mojo::ScopedDataPipeProducerHandle incoming_producer,
                  mojo::ScopedDataPipeConsumerHandle outgoing_consumer)
      : client_(std::move(client)),
        incoming_producer_(std::move(incoming_producer)),
        outgoing_consumer_(std::move(outgoing_consumer)),
        outgoing_watcher_(FROM_HERE,
                          mojo::SimpleWatcher::ArmingPolicy::AUTOMATIC,
                          base::SequencedTaskRunner::GetCurrentDefault()) {
    // Keep the page's send pipe drained so it never blocks on backpressure.
    outgoing_watcher_.Watch(
        outgoing_consumer_.get(), MOJO_HANDLE_SIGNAL_READABLE,
        MOJO_WATCH_CONDITION_SATISFIED,
        base::BindRepeating(&ReplayWebSocket::DrainOutgoing,
                            base::Unretained(this)));
  }

  ReplayWebSocket(const ReplayWebSocket&) = delete;
  ReplayWebSocket& operator=(const ReplayWebSocket&) = delete;
  ~ReplayWebSocket() override = default;

  // network::mojom::WebSocket:
  void SendMessage(network::mojom::WebSocketMessageType type,
                   uint64_t data_length) override {
    // Swallow: the page's outgoing frames go nowhere. The bytes themselves are
    // discarded by DrainOutgoing().
    LOG(ERROR) << " [WootzAuthoringWebSocket] page sent frame type="
              << static_cast<int>(type) << " bytes=" << data_length;
  }

  void StartReceiving() override {
    // Nothing to receive; we never deliver a frame.
    LOG(ERROR) << " [WootzAuthoringWebSocket] StartReceiving";
  }

  void StartClosingHandshake(uint16_t code,
                             const std::string& reason) override {
    // Complete the close cleanly so the renderer's socket does not hang in the
    // CLOSING state waiting for the network service to confirm.
    LOG(ERROR) << " [WootzAuthoringWebSocket] close requested code=" << code
              << " reason=" << reason;
    if (client_) {
      client_->OnDropChannel(/*was_clean=*/true, code, reason);
    }
  }

 private:
  void DrainOutgoing(MojoResult result, const mojo::HandleSignalsState&) {
    if (result != MOJO_RESULT_OK) {
      LOG(ERROR) << " [WootzAuthoringWebSocket] outgoing pipe not readable "
                << "result=" << result;
      return;
    }
    base::span<const uint8_t> buffer;
    size_t total_drained = 0;
    while (outgoing_consumer_->BeginReadData(MOJO_READ_DATA_FLAG_NONE,
                                             buffer) == MOJO_RESULT_OK) {
      total_drained += buffer.size();
      outgoing_consumer_->EndReadData(buffer.size());
    }
    if (total_drained > 0) {
      LOG(ERROR) << " [WootzAuthoringWebSocket] drained outgoing bytes="
                << total_drained;
    }
  }

  mojo::Remote<network::mojom::WebSocketClient> client_;
  mojo::ScopedDataPipeProducerHandle incoming_producer_;  // held open, unused
  mojo::ScopedDataPipeConsumerHandle outgoing_consumer_;  // drained
  mojo::SimpleWatcher outgoing_watcher_;
};

}  // namespace

void CreateReplayWebSocket(
    const GURL& url,
    mojo::PendingRemote<network::mojom::WebSocketHandshakeClient>
        handshake_client) {
  LOG(ERROR) << " [WootzAuthoringWebSocket] fake connect url=" << url;

  // Synthesize a "101 Switching Protocols" handshake response.
  auto response = network::mojom::WebSocketHandshakeResponse::New();
  response->url = url;
  response->http_version = net::HttpVersion(1, 1);
  response->status_code = 101;
  response->status_text = "Switching Protocols";
  response->selected_protocol = std::string();
  response->extensions = std::string();

  // Incoming path (server -> page): keep the producer (we never write, so the
  // page receives no frames) and hand the consumer to the renderer.
  mojo::ScopedDataPipeProducerHandle incoming_producer;
  mojo::ScopedDataPipeConsumerHandle incoming_consumer;
  if (mojo::CreateDataPipe(nullptr, incoming_producer, incoming_consumer) !=
      MOJO_RESULT_OK) {
    LOG(ERROR) << " [WootzAuthoringWebSocket] failed to create incoming pipe "
               << "url=" << url;
    return;
  }
  // Outgoing path (page -> server): hand the producer to the renderer and keep
  // the consumer to drain.
  mojo::ScopedDataPipeProducerHandle outgoing_producer;
  mojo::ScopedDataPipeConsumerHandle outgoing_consumer;
  if (mojo::CreateDataPipe(nullptr, outgoing_producer, outgoing_consumer) !=
      MOJO_RESULT_OK) {
    LOG(ERROR) << " [WootzAuthoringWebSocket] failed to create outgoing pipe "
               << "url=" << url;
    return;
  }

  // WebSocketClient: the renderer binds the receiver to its own implementation;
  // we keep the remote (and never push a frame through it).
  mojo::PendingRemote<network::mojom::WebSocketClient> client_remote;
  mojo::PendingReceiver<network::mojom::WebSocketClient> client_receiver =
      client_remote.InitWithNewPipeAndPassReceiver();

  // WebSocket (send side): we own the stub implementation; the renderer gets the
  // remote. MakeSelfOwnedReceiver ties the stub's lifetime to that remote, so it
  // (and its pipes/watcher) is destroyed when the page closes the socket or
  // navigates away.
  mojo::PendingRemote<network::mojom::WebSocket> socket_remote;
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<ReplayWebSocket>(std::move(client_remote),
                                        std::move(incoming_producer),
                                        std::move(outgoing_consumer)),
      socket_remote.InitWithNewPipeAndPassReceiver());

  // Fire the one call that signals "open". OnConnectionEstablished is
  // fire-and-forget, so the message is queued on the pipe before this local
  // remote is destroyed.
  mojo::Remote<network::mojom::WebSocketHandshakeClient> client(
      std::move(handshake_client));
  client->OnConnectionEstablished(
      std::move(socket_remote), std::move(client_receiver), std::move(response),
      std::move(incoming_consumer), std::move(outgoing_producer));
  LOG(ERROR) << " [WootzAuthoringWebSocket] fake handshake established url="
            << url;
}

}  // namespace wootz_authoring
