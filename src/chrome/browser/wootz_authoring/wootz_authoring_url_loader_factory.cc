// Copyright 2026 Wootzapp Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/wootz_authoring/wootz_authoring_url_loader_factory.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/self_deleting.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/wootz_authoring/wootz_authoring_service.h"
#include "chrome/browser/wootz_authoring/wootz_authoring_service_factory.h"
#include "chrome/browser/wootz_authoring/wootz_authoring_store.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/http_request_headers_update_params.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "third_party/blink/public/mojom/loader/resource_load_info.mojom-shared.h"
#include "url/origin.h"

namespace wootz_authoring {
namespace {

constexpr size_t kMaxCapturedBodyBytes = 50 * 1024 * 1024;
constexpr int kMaxReplayRedirects = 20;

enum class CaptureBodyResult {
  kSuccess,
  kOversized,
  kPipeError,
};

const char* MojoResultToString(MojoResult result) {
  switch (result) {
    case MOJO_RESULT_OK:
      return "OK";
    case MOJO_RESULT_CANCELLED:
      return "CANCELLED";
    case MOJO_RESULT_UNKNOWN:
      return "UNKNOWN";
    case MOJO_RESULT_INVALID_ARGUMENT:
      return "INVALID_ARGUMENT";
    case MOJO_RESULT_DEADLINE_EXCEEDED:
      return "DEADLINE_EXCEEDED";
    case MOJO_RESULT_NOT_FOUND:
      return "NOT_FOUND";
    case MOJO_RESULT_ALREADY_EXISTS:
      return "ALREADY_EXISTS";
    case MOJO_RESULT_PERMISSION_DENIED:
      return "PERMISSION_DENIED";
    case MOJO_RESULT_RESOURCE_EXHAUSTED:
      return "RESOURCE_EXHAUSTED";
    case MOJO_RESULT_FAILED_PRECONDITION:
      return "FAILED_PRECONDITION";
    case MOJO_RESULT_ABORTED:
      return "ABORTED";
    case MOJO_RESULT_OUT_OF_RANGE:
      return "OUT_OF_RANGE";
    case MOJO_RESULT_UNIMPLEMENTED:
      return "UNIMPLEMENTED";
    case MOJO_RESULT_INTERNAL:
      return "INTERNAL";
    case MOJO_RESULT_UNAVAILABLE:
      return "UNAVAILABLE";
    case MOJO_RESULT_DATA_LOSS:
      return "DATA_LOSS";
    case MOJO_RESULT_BUSY:
      return "BUSY";
    case MOJO_RESULT_SHOULD_WAIT:
      return "SHOULD_WAIT";
  }
  return "INVALID";
}

bool CreateDefaultDataPipe(mojo::ScopedDataPipeProducerHandle* producer,
                           mojo::ScopedDataPipeConsumerHandle* consumer) {
  MojoCreateDataPipeOptions options;
  options.struct_size = sizeof(MojoCreateDataPipeOptions);
  options.flags = MOJO_CREATE_DATA_PIPE_FLAG_NONE;
  options.element_num_bytes = 1;
  options.capacity_num_bytes = 0;
  return mojo::CreateDataPipe(&options, *producer, *consumer) == MOJO_RESULT_OK;
}

std::string FactoryTypeToString(
    content::ContentBrowserClient::URLLoaderFactoryType type) {
  using URLLoaderFactoryType =
      content::ContentBrowserClient::URLLoaderFactoryType;
  switch (type) {
    case URLLoaderFactoryType::kNavigation:
      return "navigation";
    case URLLoaderFactoryType::kDownload:
      return "download";
    case URLLoaderFactoryType::kDocumentSubResource:
      return "document_subresource";
    case URLLoaderFactoryType::kWorkerMainResource:
      return "worker_main_resource";
    case URLLoaderFactoryType::kWorkerSubResource:
      return "worker_subresource";
    case URLLoaderFactoryType::kServiceWorkerScript:
      return "service_worker_script";
    case URLLoaderFactoryType::kServiceWorkerSubResource:
      return "service_worker_subresource";
    case URLLoaderFactoryType::kPrefetch:
      return "prefetch";
    case URLLoaderFactoryType::kDevTools:
      return "devtools";
    case URLLoaderFactoryType::kEarlyHints:
      return "early_hints";
  }
}

bool ShouldProxyFactoryType(
    content::ContentBrowserClient::URLLoaderFactoryType type) {
  using URLLoaderFactoryType =
      content::ContentBrowserClient::URLLoaderFactoryType;
  switch (type) {
    case URLLoaderFactoryType::kNavigation:
    case URLLoaderFactoryType::kDocumentSubResource:
    case URLLoaderFactoryType::kWorkerMainResource:
    case URLLoaderFactoryType::kWorkerSubResource:
    case URLLoaderFactoryType::kServiceWorkerScript:
    case URLLoaderFactoryType::kServiceWorkerSubResource:
      return true;
    case URLLoaderFactoryType::kDownload:
    case URLLoaderFactoryType::kPrefetch:
    case URLLoaderFactoryType::kDevTools:
    case URLLoaderFactoryType::kEarlyHints:
      return false;
  }
}

base::DictValue RequestHeadersToDict(const net::HttpRequestHeaders& headers) {
  base::DictValue dict;
  for (const auto& header : headers.GetHeaderVector()) {
    dict.Set(header.key, header.value);
  }
  return dict;
}

base::DictValue ResponseHeadersToDict(
    const scoped_refptr<net::HttpResponseHeaders>& headers) {
  base::DictValue dict;
  if (!headers) {
    return dict;
  }

  size_t iterator = 0;
  std::string name;
  std::string value;
  while (headers->EnumerateHeaderLines(&iterator, &name, &value)) {
    dict.Set(name, value);
  }
  return dict;
}

CapturedRequestInfo BuildRequestInfo(const network::ResourceRequest& request,
                                     const std::string& factory_type,
                                     const url::Origin& request_initiator) {
  CapturedRequestInfo info;
  info.request_body_digest =
      WootzAuthoringStore::RequestBodyDigestForKey(request);
  info.request_body_fields =
      WootzAuthoringStore::RequestBodyFieldsForKey(request);
  info.request_key = WootzAuthoringStore::MakeRequestKey(
      request.method, request.url, info.request_body_digest);
  info.normalized_request_key = WootzAuthoringStore::MakeNormalizedRequestKey(
      request.method, request.url, info.request_body_fields,
      info.request_body_digest);
  info.endpoint_key = WootzAuthoringStore::MakeEndpointKey(request.url);
  info.query_fields = WootzAuthoringStore::QueryFieldsForUrl(request.url);
  info.url = request.url;
  info.method = request.method;
  info.request_headers = RequestHeadersToDict(request.headers);
  info.resource_type = base::NumberToString(request.resource_type);
  info.request_destination =
      base::NumberToString(static_cast<int>(request.destination));
  info.factory_type = factory_type;
  info.request_initiator = request_initiator.opaque()
                               ? std::string("opaque")
                               : request_initiator.Serialize();
  info.timestamp = base::Time::Now();
  return info;
}

CapturedResponseInfo BuildResponseInfo(
    const network::mojom::URLResponseHeadPtr& head) {
  CapturedResponseInfo info;
  if (!head) {
    return info;
  }
  info.mime_type = head->mime_type;
  info.charset = head->charset;
  if (head->headers) {
    info.status_code = head->headers->response_code();
    info.raw_response_headers = head->headers->raw_headers();
    info.response_headers = ResponseHeadersToDict(head->headers);
  }
  return info;
}

bool HasAcceptHeaderValue(const network::ResourceRequest& request,
                          std::string_view value) {
  std::optional<std::string> header_value =
      request.headers.GetHeader(net::HttpRequestHeaders::kAccept);
  return header_value && *header_value == value;
}

bool ShouldSkipAuthoringRequest(const network::ResourceRequest& request) {
  if (request.method == "OPTIONS" || request.method == "HEAD") {
    return true;
  }

  if (HasAcceptHeaderValue(request, "text/event-stream")) {
    return true;
  }

  switch (static_cast<blink::mojom::ResourceType>(request.resource_type)) {
    case blink::mojom::ResourceType::kPing:
      return true;
    default:
      break;
  }

  return false;
}

ReplayResponse BuildReplayNoContentResponse(const std::string& reason) {
  ReplayResponse response;
  response.status_code = 204;
  response.mime_type = "text/plain; charset=utf-8";
  const std::string header_string = base::StringPrintf(
      "HTTP/1.1 204 No Content\n"
      "Content-Type: text/plain; charset=utf-8\n"
      "Content-Length: 0\n"
      "X-Wootz-Authoring-Replay: synthetic-noise\n"
      "X-Wootz-Authoring-Reason: %s\n",
      reason.c_str());
  response.raw_response_headers =
      net::HttpUtil::AssembleRawHeaders(header_string);
  return response;
}

ReplayResponse BuildReplayMissResponse(const network::ResourceRequest& request,
                                       const std::string& factory_type) {
  ReplayResponse response;
  response.status_code = 404;
  response.mime_type = "text/plain; charset=utf-8";
  response.payload = base::StringPrintf(
      "Wootz authoring replay miss\n\n"
      "method: %s\n"
      "url: %s\n"
      "factory_type: %s\n"
      "resource_type: %i\n"
      "request_destination: %i\n",
      request.method.c_str(), request.url.spec().c_str(), factory_type.c_str(),
      static_cast<int>(request.resource_type),
      static_cast<int>(request.destination));

  const std::string header_string = base::StringPrintf(
      "HTTP/1.1 404 Not Found\n"
      "Content-Type: text/plain; charset=utf-8\n"
      "Content-Length: %s\n",
      base::NumberToString(response.payload.size()).c_str());
  response.raw_response_headers =
      net::HttpUtil::AssembleRawHeaders(header_string);
  return response;
}

network::mojom::URLResponseHeadPtr BuildReplayResponseHead(
    const ReplayResponse& response) {
  auto head = network::mojom::URLResponseHead::New();
  head->mime_type = response.mime_type;
  head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
      response.raw_response_headers.empty() ? std::string()
                                            : response.raw_response_headers);
  std::string original_content_encoding;
  std::string original_transfer_encoding;
  std::string original_content_length;
  if (head->headers) {
    if (auto value = head->headers->GetNormalizedHeader("content-encoding")) {
      original_content_encoding = *value;
    }
    if (auto value = head->headers->GetNormalizedHeader("transfer-encoding")) {
      original_transfer_encoding = *value;
    }
    if (auto value = head->headers->GetNormalizedHeader("content-length")) {
      original_content_length = *value;
    }
  }
  LOG(ERROR) << " [WootzAuthoring] BuildReplayResponseHead before status="
             << response.status_code << " mime_type=" << response.mime_type
             << " payload_bytes=" << response.payload.size()
             << " raw_header_bytes=" << response.raw_response_headers.size()
             << " original_content_encoding=" << original_content_encoding
             << " original_transfer_encoding=" << original_transfer_encoding
             << " original_content_length=" << original_content_length;
  if (response.raw_response_headers.empty()) {
    const int status_code =
        response.status_code == 0 ? 200 : response.status_code;
    head->headers->ReplaceStatusLine("HTTP/1.1 " +
                                     base::NumberToString(status_code) + " OK");
  }

  head->headers->RemoveHeader("Content-Encoding");
  head->headers->RemoveHeader("Transfer-Encoding");
  head->headers->RemoveHeader("Content-Length");
  head->headers->RemoveHeader("Connection");
  head->headers->RemoveHeader("Keep-Alive");
  head->headers->RemoveHeader("Alt-Svc");

  if (!response.mime_type.empty()) {
    head->headers->SetHeader("Content-Type", response.mime_type);
  }
  head->headers->SetHeader("Content-Length",
                           base::NumberToString(response.payload.size()));
  head->client_side_content_decoding_types.clear();
  head->content_length = response.payload.size();
  std::string final_content_type;
  std::string final_content_length;
  std::string final_content_encoding;
  if (head->headers) {
    if (auto value = head->headers->GetNormalizedHeader("content-type")) {
      final_content_type = *value;
    }
    if (auto value = head->headers->GetNormalizedHeader("content-length")) {
      final_content_length = *value;
    }
    if (auto value = head->headers->GetNormalizedHeader("content-encoding")) {
      final_content_encoding = *value;
    }
  }
  LOG(ERROR)
      << " [WootzAuthoring] BuildReplayResponseHead after final_content_type="
      << final_content_type << " final_content_length=" << final_content_length
      << " final_content_encoding=" << final_content_encoding
      << " decode_types_count="
      << head->client_side_content_decoding_types.size();
  return head;
}

network::mojom::URLResponseHeadPtr BuildReplayRedirectHead(
    const ReplayResponse& response) {
  auto head = network::mojom::URLResponseHead::New();
  head->encoded_data_length = 0;
  head->request_start = base::TimeTicks::Now();
  head->response_start = head->request_start;

  if (!response.raw_response_headers.empty()) {
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        response.raw_response_headers);
  } else {
    const int status_code =
        response.status_code == 0 ? 302 : response.status_code;
    const std::string header_string = base::StringPrintf(
        "HTTP/1.1 %i Redirect\n"
        "Location: %s\n",
        status_code, response.redirect_url.c_str());
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        net::HttpUtil::AssembleRawHeaders(header_string));
  }

  if (head->headers) {
    head->headers->RemoveHeader("Content-Encoding");
    head->headers->RemoveHeader("Transfer-Encoding");
    head->headers->SetHeader("Content-Length", "0");
    if (!response.redirect_url.empty()) {
      head->headers->SetHeader("Location", response.redirect_url);
    }
  }
  head->content_length = 0;
  return head;
}

class StringDataSource : public mojo::DataPipeProducer::DataSource {
 public:
  explicit StringDataSource(std::string data) : data_(std::move(data)) {}

  uint64_t GetLength() const override { return data_.size(); }

  ReadResult Read(uint64_t offset, base::span<char> buffer) override {
    ReadResult result;
    if (offset >= data_.size()) {
      result.result = MOJO_RESULT_FAILED_PRECONDITION;
      result.bytes_read = 0;
      return result;
    }

    const size_t copy_size =
        std::min(buffer.size(), data_.size() - static_cast<size_t>(offset));
    buffer.first(copy_size)
        .copy_from(base::span(data_).subspan(static_cast<size_t>(offset),
                                             copy_size));
    result.result = MOJO_RESULT_OK;
    result.bytes_read = copy_size;
    return result;
  }

 private:
  std::string data_;
};

class BodyCapture {
 public:
  using DoneCallback =
      base::OnceCallback<void(CaptureBodyResult result,
                              std::optional<std::string> payload)>;

  BodyCapture(mojo::ScopedDataPipeConsumerHandle source,
              mojo::ScopedDataPipeProducerHandle target,
              size_t max_capture_bytes,
              DoneCallback done_callback)
      : source_(std::move(source)),
        target_(std::move(target)),
        max_capture_bytes_(max_capture_bytes),
        done_callback_(std::move(done_callback)),
        source_watcher_(FROM_HERE,
                        mojo::SimpleWatcher::ArmingPolicy::MANUAL,
                        base::SequencedTaskRunner::GetCurrentDefault()),
        target_watcher_(FROM_HERE,
                        mojo::SimpleWatcher::ArmingPolicy::MANUAL,
                        base::SequencedTaskRunner::GetCurrentDefault()) {
    source_watcher_.Watch(source_.get(), MOJO_HANDLE_SIGNAL_READABLE,
                          MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
                          base::BindRepeating(&BodyCapture::OnSourceReadable,
                                              weak_factory_.GetWeakPtr()));
    target_watcher_.Watch(target_.get(), MOJO_HANDLE_SIGNAL_WRITABLE,
                          MOJO_TRIGGER_CONDITION_SIGNALS_SATISFIED,
                          base::BindRepeating(&BodyCapture::OnTargetWritable,
                                              weak_factory_.GetWeakPtr()));
    source_watcher_.ArmOrNotify();
  }

  BodyCapture(const BodyCapture&) = delete;
  BodyCapture& operator=(const BodyCapture&) = delete;
  ~BodyCapture() = default;

 private:
  void OnSourceReadable(MojoResult result,
                        const mojo::HandleSignalsState& state) {
    LOG(ERROR)
        << " [WootzAuthoringDEV_pipe] BodyCapture::OnSourceReadable result="
        << MojoResultToString(result) << " readable=" << state.readable()
        << " peer_closed=" << state.peer_closed()
        << " pending_write_bytes=" << pending_write_.size()
        << " pending_write_offset=" << pending_write_offset_
        << " captured_bytes=" << captured_.size();
    if (result == MOJO_RESULT_FAILED_PRECONDITION) {
      Finish(true);
      return;
    }
    if (result != MOJO_RESULT_OK) {
      Finish(false);
      return;
    }
    ReadAvailable();
  }

  void OnTargetWritable(MojoResult result,
                        const mojo::HandleSignalsState& state) {
    LOG(ERROR)
        << " [WootzAuthoringDEV_pipe] BodyCapture::OnTargetWritable result="
        << MojoResultToString(result) << " writable=" << state.writable()
        << " peer_closed=" << state.peer_closed()
        << " pending_write_bytes=" << pending_write_.size()
        << " pending_write_offset=" << pending_write_offset_
        << " captured_bytes=" << captured_.size();
    if (result != MOJO_RESULT_OK) {
      Finish(false);
      return;
    }
    WritePending();
    if (pending_write_.empty()) {
      ReadAvailable();
    }
  }

  void ReadAvailable() {
    while (!finished_ && pending_write_.empty()) {
      base::span<const uint8_t> data;
      MojoResult result =
          source_->BeginReadData(MOJO_READ_DATA_FLAG_NONE, data);
      LOG(ERROR)
          << " [WootzAuthoringDEV_pipe] BodyCapture::BeginReadData result="
          << MojoResultToString(result)
          << " bytes_available=" << (result == MOJO_RESULT_OK ? data.size() : 0)
          << " captured_bytes=" << captured_.size()
          << " pending_write_bytes=" << pending_write_.size()
          << " pending_write_offset=" << pending_write_offset_;
      if (result == MOJO_RESULT_SHOULD_WAIT) {
        source_watcher_.ArmOrNotify();
        return;
      }
      if (result == MOJO_RESULT_FAILED_PRECONDITION) {
        Finish(true);
        return;
      }
      if (result != MOJO_RESULT_OK) {
        Finish(false);
        return;
      }

      std::string_view chunk(reinterpret_cast<const char*>(data.data()),
                             data.size());
      pending_write_.assign(chunk);
      pending_write_offset_ = 0;
      LOG(ERROR) << " [WootzAuthoringDEV_pipe] BodyCapture::ReadChunk bytes="
                 << chunk.size() << " capture_too_large=" << capture_too_large_
                 << " captured_bytes_before=" << captured_.size();

      if (!capture_too_large_) {
        if (captured_.size() + chunk.size() <= max_capture_bytes_) {
          captured_.append(chunk);
        } else {
          capture_too_large_ = true;
          captured_.clear();
          LOG(ERROR)
              << " [WootzAuthoringDEV_pipe] BodyCapture exceeded max capture "
              << "bytes limit=" << max_capture_bytes_
              << " chunk_bytes=" << chunk.size();
        }
      }

      source_->EndReadData(data.size());
      LOG(ERROR) << " [WootzAuthoringDEV_pipe] BodyCapture::EndReadData bytes="
                 << data.size() << " captured_bytes_after=" << captured_.size()
                 << " pending_write_bytes=" << pending_write_.size();
      WritePending();
    }
  }

  void WritePending() {
    while (!finished_ && pending_write_offset_ < pending_write_.size()) {
      base::span<const uint8_t> data =
          base::as_byte_span(pending_write_).subspan(pending_write_offset_);
      size_t bytes_written = 0;
      MojoResult result =
          target_->WriteData(data, MOJO_WRITE_DATA_FLAG_NONE, bytes_written);
      LOG(ERROR) << " [WootzAuthoringDEV_pipe] BodyCapture::WriteData result="
                 << MojoResultToString(result)
                 << " bytes_requested=" << data.size()
                 << " bytes_written=" << bytes_written
                 << " pending_write_bytes=" << pending_write_.size()
                 << " pending_write_offset=" << pending_write_offset_
                 << " captured_bytes=" << captured_.size();
      if (result == MOJO_RESULT_SHOULD_WAIT) {
        target_watcher_.ArmOrNotify();
        return;
      }
      if (result != MOJO_RESULT_OK) {
        Finish(false);
        return;
      }
      pending_write_offset_ += bytes_written;
    }

    pending_write_.clear();
    pending_write_offset_ = 0;
  }

  void Finish(bool success) {
    if (finished_) {
      return;
    }
    LOG(ERROR) << " [WootzAuthoringDEV_pipe] BodyCapture::Finish success="
               << success << " capture_too_large=" << capture_too_large_
               << " captured_bytes=" << captured_.size()
               << " pending_write_bytes=" << pending_write_.size()
               << " pending_write_offset=" << pending_write_offset_;
    finished_ = true;
    source_watcher_.Cancel();
    target_watcher_.Cancel();
    source_.reset();
    target_.reset();

    CaptureBodyResult capture_result = CaptureBodyResult::kSuccess;
    std::optional<std::string> result;
    if (success) {
      if (capture_too_large_) {
        capture_result = CaptureBodyResult::kOversized;
      } else {
        result = std::move(captured_);
      }
    } else {
      capture_result = CaptureBodyResult::kPipeError;
    }

    if (!done_callback_.is_null()) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, base::BindOnce(std::move(done_callback_), capture_result,
                                    std::move(result)));
    }
  }

  mojo::ScopedDataPipeConsumerHandle source_;
  mojo::ScopedDataPipeProducerHandle target_;
  size_t max_capture_bytes_;
  DoneCallback done_callback_;
  mojo::SimpleWatcher source_watcher_;
  mojo::SimpleWatcher target_watcher_;
  std::string captured_;
  std::string pending_write_;
  size_t pending_write_offset_ = 0;
  bool capture_too_large_ = false;
  bool finished_ = false;
  base::WeakPtrFactory<BodyCapture> weak_factory_{this};
};

class CaptureRequest final : public network::mojom::URLLoader,
                             public network::mojom::URLLoaderClient {
 public:
  CaptureRequest(
      base::WeakPtr<WootzAuthoringService> service,
      network::mojom::URLLoaderFactory* target_factory,
      mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
      std::string factory_type,
      const url::Origin& request_initiator)
      : service_(std::move(service)),
        request_info_(
            BuildRequestInfo(request, factory_type, request_initiator)),
        current_url_(request.url),
        current_method_(request.method),
        current_body_digest_(request_info_.request_body_digest),
        target_client_(std::move(client)),
        loader_receiver_(this, std::move(loader_receiver)) {
    loader_receiver_.set_disconnect_handler(base::BindOnce(
        &CaptureRequest::OnClientDisconnected, weak_factory_.GetWeakPtr()));

    target_factory->CreateLoaderAndStart(
        target_loader_.BindNewPipeAndPassReceiver(), request_id, options,
        request, client_receiver_.BindNewPipeAndPassRemote(),
        traffic_annotation);
  }

  CaptureRequest(const CaptureRequest&) = delete;
  CaptureRequest& operator=(const CaptureRequest&) = delete;

  void FollowRedirect(network::HttpRequestHeadersUpdateParams headers_update_params,
                      const std::optional<GURL>& new_url) override {
    if (target_loader_) {
      target_loader_->FollowRedirect(std::move(headers_update_params), new_url);
    }
  }

  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {
    if (target_loader_) {
      target_loader_->SetPriority(priority, intra_priority_value);
    }
  }

  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr early_hints) override {
    if (target_client_) {
      target_client_->OnReceiveEarlyHints(std::move(early_hints));
    }
  }

  void OnReceiveResponse(
      network::mojom::URLResponseHeadPtr head,
      mojo::ScopedDataPipeConsumerHandle body,
      std::optional<mojo_base::BigBuffer> cached_metadata) override {
    received_response_ = true;
    response_info_ = BuildResponseInfo(head);

    if (!body.is_valid()) {
      body_complete_ = true;
      body_payload_ = std::string();
      if (target_client_) {
        target_client_->OnReceiveResponse(std::move(head), std::move(body),
                                          std::move(cached_metadata));
      }
      return;
    }

    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (!CreateDefaultDataPipe(&producer, &consumer)) {
      body_complete_ = true;
      if (target_client_) {
        target_client_->OnReceiveResponse(std::move(head), std::move(body),
                                          std::move(cached_metadata));
      }
      return;
    }

    body_capture_ = std::make_unique<BodyCapture>(
        std::move(body), std::move(producer), kMaxCapturedBodyBytes,
        base::BindOnce(&CaptureRequest::OnBodyCaptured,
                       weak_factory_.GetWeakPtr()));

    if (target_client_) {
      target_client_->OnReceiveResponse(std::move(head), std::move(consumer),
                                        std::move(cached_metadata));
    }
  }

  void OnReceiveRedirect(const net::RedirectInfo& redirect_info,
                         network::mojom::URLResponseHeadPtr head) override {
    if (service_) {
      CapturedRequestInfo redirect_request;
      redirect_request.request_key = WootzAuthoringStore::MakeRequestKey(
          current_method_, current_url_, current_body_digest_);
      redirect_request.url = current_url_;
      redirect_request.method = current_method_;
      redirect_request.request_body_digest = current_body_digest_;
      redirect_request.query_fields =
          WootzAuthoringStore::QueryFieldsForUrl(current_url_);
      if (current_body_digest_ != WootzAuthoringStore::kNoRequestBodyDigest) {
        redirect_request.request_body_fields =
            request_info_.request_body_fields.Clone();
      }
      redirect_request.normalized_request_key =
          WootzAuthoringStore::MakeNormalizedRequestKey(
              current_method_, current_url_,
              redirect_request.request_body_fields, current_body_digest_);
      redirect_request.endpoint_key =
          WootzAuthoringStore::MakeEndpointKey(current_url_);
      redirect_request.request_headers = request_info_.request_headers.Clone();
      redirect_request.resource_type = request_info_.resource_type;
      redirect_request.request_destination = request_info_.request_destination;
      redirect_request.factory_type = request_info_.factory_type;
      redirect_request.request_initiator = request_info_.request_initiator;
      redirect_request.timestamp = base::Time::Now();
      service_->SaveCapturedRedirect(redirect_request, BuildResponseInfo(head),
                                     redirect_info.new_url);
    }

    current_url_ = redirect_info.new_url;
    current_method_ = redirect_info.new_method;
    if (current_method_ == "GET" || current_method_ == "HEAD") {
      current_body_digest_ = WootzAuthoringStore::kNoRequestBodyDigest;
    }

    if (target_client_) {
      target_client_->OnReceiveRedirect(redirect_info, std::move(head));
    }
  }

  void OnUploadProgress(int64_t current_position,
                        int64_t total_size,
                        OnUploadProgressCallback callback) override {
    if (target_client_) {
      target_client_->OnUploadProgress(current_position, total_size,
                                       std::move(callback));
    }
  }

  void OnTransferSizeUpdated(int32_t transfer_size_diff) override {
    if (target_client_) {
      target_client_->OnTransferSizeUpdated(transfer_size_diff);
    }
  }

  void OnComplete(const network::URLLoaderCompletionStatus& status) override {
    response_info_.net_error = status.error_code;
    request_complete_ = true;
    if (!received_response_) {
      body_complete_ = true;
    }

    if (target_client_) {
      target_client_->OnComplete(status);
    }

    MaybeStoreAndDelete();
  }

 private:
  ~CaptureRequest() override = default;

  void OnClientDisconnected() {
    if (!request_complete_) {
      target_loader_.reset();
    }
  }

  void OnBodyCaptured(CaptureBodyResult result,
                      std::optional<std::string> payload) {
    body_capture_.reset();
    body_complete_ = true;
    body_capture_result_ = result;
    body_payload_ = std::move(payload);
    MaybeStoreAndDelete();
  }

  void MaybeStoreAndDelete() {
    if (!request_complete_ || !body_complete_) {
      return;
    }

    if (service_ && received_response_ && body_payload_.has_value() &&
        response_info_.net_error == net::OK) {
      request_info_.url = current_url_;
      request_info_.method = current_method_;
      request_info_.request_body_digest = current_body_digest_;
      request_info_.request_key = WootzAuthoringStore::MakeRequestKey(
          current_method_, current_url_, current_body_digest_);
      request_info_.query_fields =
          WootzAuthoringStore::QueryFieldsForUrl(current_url_);
      if (current_body_digest_ == WootzAuthoringStore::kNoRequestBodyDigest) {
        request_info_.request_body_fields = base::DictValue();
      }
      request_info_.normalized_request_key =
          WootzAuthoringStore::MakeNormalizedRequestKey(
              current_method_, current_url_, request_info_.request_body_fields,
              current_body_digest_);
      request_info_.endpoint_key =
          WootzAuthoringStore::MakeEndpointKey(current_url_);
      service_->SaveCapturedResponse(request_info_, response_info_,
                                     *body_payload_);
    } else if (received_response_ && !body_payload_.has_value()) {
      const char* reason = "unknown";
      switch (body_capture_result_) {
        case CaptureBodyResult::kSuccess:
          reason = "missing_payload";
          break;
        case CaptureBodyResult::kOversized:
          reason = "oversized";
          break;
        case CaptureBodyResult::kPipeError:
          reason = "pipe_error";
          break;
      }
      LOG(ERROR) << " [WootzAuthoringDEV_pipe] Skipping capture reason="
                 << reason << " url=" << request_info_.url
                 << " limit_bytes=" << kMaxCapturedBodyBytes
                 << " net_error=" << response_info_.net_error
                 << " factory_type=" << request_info_.factory_type;
    }

    delete this;
  }

  base::WeakPtr<WootzAuthoringService> service_;
  CapturedRequestInfo request_info_;
  CapturedResponseInfo response_info_;
  GURL current_url_;
  std::string current_method_;
  std::string current_body_digest_;
  mojo::Receiver<network::mojom::URLLoaderClient> client_receiver_{this};
  mojo::Remote<network::mojom::URLLoaderClient> target_client_;
  mojo::Receiver<network::mojom::URLLoader> loader_receiver_;
  mojo::Remote<network::mojom::URLLoader> target_loader_;
  std::unique_ptr<BodyCapture> body_capture_;
  std::optional<std::string> body_payload_;
  CaptureBodyResult body_capture_result_ = CaptureBodyResult::kSuccess;
  bool received_response_ = false;
  bool request_complete_ = false;
  bool body_complete_ = false;
  base::WeakPtrFactory<CaptureRequest> weak_factory_{this};
};

class ReplayRequest final : public network::mojom::URLLoader {
 public:
  ReplayRequest(
      base::WeakPtr<WootzAuthoringService> service,
      const network::ResourceRequest& request,
      mojo::PendingReceiver<network::mojom::URLLoader> loader_receiver,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      ReplayResponse response)
      : service_(std::move(service)),
        current_request_(request),
        client_(std::move(client)),
        loader_receiver_(this, std::move(loader_receiver)),
        response_(std::move(response)) {
    loader_receiver_.set_disconnect_handler(
        base::BindOnce(&ReplayRequest::DeleteSelf, weak_factory_.GetWeakPtr()));
    client_.set_disconnect_handler(
        base::BindOnce(&ReplayRequest::DeleteSelf, weak_factory_.GetWeakPtr()));
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&ReplayRequest::Start, weak_factory_.GetWeakPtr()));
  }

  ReplayRequest(const ReplayRequest&) = delete;
  ReplayRequest& operator=(const ReplayRequest&) = delete;

  void FollowRedirect(network::HttpRequestHeadersUpdateParams headers_update_params,
                      const std::optional<GURL>& new_url) override {
    if (response_.redirect_url.empty()) {
      Complete(net::ERR_FAILED);
      return;
    }

    current_request_.url = new_url.value_or(GURL(response_.redirect_url));
    current_request_.method = pending_redirect_method_.empty()
                                  ? current_request_.method
                                  : pending_redirect_method_;
    headers_update_params.Apply(current_request_.headers,
                                current_request_.cors_exempt_headers);

    if (!service_ ||
        !service_->LoadReplayResponse(current_request_, &response_)) {
      LOG(ERROR) << " [WootzAuthoring] Replay redirect target miss "
                 << current_request_.method << " " << current_request_.url;
      Complete(net::ERR_FAILED);
      return;
    }

    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&ReplayRequest::Start, weak_factory_.GetWeakPtr()));
  }

  void SetPriority(net::RequestPriority priority,
                   int32_t intra_priority_value) override {}

 private:
  ~ReplayRequest() override = default;

  void Start() {
    if (!replay_state_checked_) {
      replay_state_checked_ = true;
      if (service_) {
        service_->RunWhenReplayStateReady(
            base::BindOnce(&ReplayRequest::Start, weak_factory_.GetWeakPtr()));
        return;
      }
    }

    if (!client_) {
      DeleteSelf();
      return;
    }

    if (!response_.redirect_url.empty()) {
      if (++redirect_count_ > kMaxReplayRedirects) {
        Complete(net::ERR_TOO_MANY_REDIRECTS);
        return;
      }
      const int status_code =
          response_.status_code == 0 ? 302 : response_.status_code;
      const GURL redirect_url(response_.redirect_url);
      net::RedirectInfo redirect_info = net::RedirectInfo::ComputeRedirectInfo(
          current_request_.method, current_request_.url,
          current_request_.site_for_cookies,
          current_request_.update_first_party_url_on_redirect
              ? net::RedirectInfo::FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT
              : net::RedirectInfo::FirstPartyURLPolicy::NEVER_CHANGE_URL,
          current_request_.referrer_policy, current_request_.referrer.spec(),
          current_request_.request_initiator, status_code, redirect_url,
          /*referrer_policy_header=*/std::nullopt,
          /*insecure_scheme_was_upgraded=*/false);
      pending_redirect_method_ = redirect_info.new_method;
      LOG(ERROR) << " [WootzAuthoring] Replaying redirect "
                 << current_request_.url << " -> " << redirect_url
                 << " status=" << status_code;
      client_->OnReceiveRedirect(redirect_info,
                                 BuildReplayRedirectHead(response_));
      return;
    }

    auto head = BuildReplayResponseHead(response_);
    if (response_.payload.empty()) {
      client_->OnReceiveResponse(std::move(head), {}, std::nullopt);
      Complete(net::OK);
      return;
    }

    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (!CreateDefaultDataPipe(&producer, &consumer)) {
      Complete(net::ERR_FAILED);
      return;
    }

    client_->OnReceiveResponse(std::move(head), std::move(consumer),
                               std::nullopt);
    producer_ = std::make_unique<mojo::DataPipeProducer>(std::move(producer));
    producer_->Write(
        std::make_unique<StringDataSource>(std::move(response_.payload)),
        base::BindOnce(&ReplayRequest::OnBodyWritten,
                       weak_factory_.GetWeakPtr()));
  }

  void OnBodyWritten(MojoResult result) {
    producer_.reset();
    Complete(result == MOJO_RESULT_OK ? net::OK : net::ERR_FAILED);
  }

  void Complete(int error_code) {
    if (client_) {
      network::URLLoaderCompletionStatus status(error_code);
      client_->OnComplete(status);
    }
    DeleteSelf();
  }

  void DeleteSelf() { delete this; }

  base::WeakPtr<WootzAuthoringService> service_;
  network::ResourceRequest current_request_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  mojo::Receiver<network::mojom::URLLoader> loader_receiver_;
  ReplayResponse response_;
  std::string pending_redirect_method_;
  int redirect_count_ = 0;
  bool replay_state_checked_ = false;
  std::unique_ptr<mojo::DataPipeProducer> producer_;
  base::WeakPtrFactory<ReplayRequest> weak_factory_{this};
};

class WootzAuthoringURLLoaderFactory final
    : public network::SelfDeletingURLLoaderFactory {
 public:
  WootzAuthoringURLLoaderFactory(
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> target_factory,
      base::WeakPtr<WootzAuthoringService> service,
      std::string factory_type,
      url::Origin request_initiator,
      base::SelfDeletingPassKey key)
      : network::SelfDeletingURLLoaderFactory(std::move(receiver), key),
        service_(std::move(service)),
        factory_type_(std::move(factory_type)),
        request_initiator_(std::move(request_initiator)) {
    target_factory_.Bind(std::move(target_factory));
    target_factory_.set_disconnect_handler(base::BindOnce(
        &WootzAuthoringURLLoaderFactory::OnTargetFactoryDisconnected,
        base::Unretained(this)));
  }

  WootzAuthoringURLLoaderFactory(const WootzAuthoringURLLoaderFactory&) =
      delete;
  WootzAuthoringURLLoaderFactory& operator=(
      const WootzAuthoringURLLoaderFactory&) = delete;

  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t request_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& traffic_annotation)
      override {
    LOG(ERROR) << " [WootzAuthoring] request decode flag url=" << request.url
               << " method=" << request.method
               << " factory_type=" << factory_type_
               << " client_side_content_decoding_enabled="
               << request.client_side_content_decoding_enabled;

    if (!request.url.SchemeIsHTTPOrHTTPS() || !service_) {
      target_factory_->CreateLoaderAndStart(std::move(loader), request_id,
                                            options, request, std::move(client),
                                            traffic_annotation);
      return;
    }

    if (service_->IsReplayEnabled()) {
      if (ShouldSkipAuthoringRequest(request)) {
        LOG(ERROR) << " [WootzAuthoring] Skipping replay request "
                   << request.method << " " << request.url
                   << " factory_type=" << factory_type_ << " resource_type="
                   << static_cast<int>(request.resource_type)
                   << " request_destination="
                   << static_cast<int>(request.destination);
        new ReplayRequest(service_, request, std::move(loader),
                          std::move(client),
                          BuildReplayNoContentResponse("skipped"));
        return;
      }

      ReplayResponse response;
      if (service_->LoadReplayResponse(request, &response)) {
        LOG(ERROR) << " [WootzAuthoring] Replaying " << request.method << " "
                   << request.url;
        new ReplayRequest(service_, request, std::move(loader),
                          std::move(client), std::move(response));
        return;
      }
      LOG(ERROR) << " [WootzAuthoring] Strict replay miss " << request.method
                 << " " << request.url << " factory_type=" << factory_type_
                 << " resource_type=" << static_cast<int>(request.resource_type)
                 << " request_destination="
                 << static_cast<int>(request.destination);
      new ReplayRequest(service_, request, std::move(loader), std::move(client),
                        BuildReplayMissResponse(request, factory_type_));
      return;
    }

    if (service_->IsCaptureEnabled()) {
      if (ShouldSkipAuthoringRequest(request)) {
        LOG(ERROR) << " [WootzAuthoring] Skipping capture request "
                   << request.method << " " << request.url
                   << " factory_type=" << factory_type_ << " resource_type="
                   << static_cast<int>(request.resource_type)
                   << " request_destination="
                   << static_cast<int>(request.destination);
        target_factory_->CreateLoaderAndStart(
            std::move(loader), request_id, options, request, std::move(client),
            traffic_annotation);
        return;
      }

      network::ResourceRequest decoded_request = request;
      decoded_request.client_side_content_decoding_enabled = false;
      LOG(ERROR) << " [WootzAuthoring] forcing decoded capture url="
                 << decoded_request.url << " method=" << decoded_request.method
                 << " factory_type=" << factory_type_
                 << " incoming_decode_flag="
                 << request.client_side_content_decoding_enabled
                 << " outgoing_decode_flag="
                 << decoded_request.client_side_content_decoding_enabled;
      new CaptureRequest(service_, target_factory_.get(), std::move(loader),
                         request_id, options, decoded_request,
                         std::move(client), traffic_annotation, factory_type_,
                         request_initiator_);
      return;
    }

    target_factory_->CreateLoaderAndStart(std::move(loader), request_id,
                                          options, request, std::move(client),
                                          traffic_annotation);
  }

 private:
  ~WootzAuthoringURLLoaderFactory() override = default;

  void OnTargetFactoryDisconnected() { DisconnectReceiversAndDestroy(); }

  mojo::Remote<network::mojom::URLLoaderFactory> target_factory_;
  base::WeakPtr<WootzAuthoringService> service_;
  std::string factory_type_;
  url::Origin request_initiator_;
};

}  // namespace

void MaybeProxyAuthoringURLLoaderFactory(
    content::BrowserContext* browser_context,
    content::RenderFrameHost* frame,
    content::ContentBrowserClient::URLLoaderFactoryType type,
    network::URLLoaderFactoryBuilder& factory_builder) {
  if (!ShouldProxyFactoryType(type)) {
    return;
  }

  Profile* profile = Profile::FromBrowserContext(browser_context);
  if (!profile) {
    return;
  }

  WootzAuthoringService* service =
      WootzAuthoringServiceFactory::GetForProfile(profile);
  if (!service || !service->IsEnabled()) {
    return;
  }

  auto [receiver, remote] = factory_builder.Append();
  base::MakeSelfDeleting<WootzAuthoringURLLoaderFactory>(
      std::move(receiver), std::move(remote), service->GetWeakPtr(),
      FactoryTypeToString(type),
      frame ? frame->GetLastCommittedOrigin() : url::Origin());
}

}  // namespace wootz_authoring
