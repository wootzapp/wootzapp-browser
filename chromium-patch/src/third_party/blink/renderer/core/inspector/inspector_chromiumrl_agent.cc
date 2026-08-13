// Copyright 2025 The Chromium Authors
// File: third_party/blink/renderer/core/inspector/inspector_chromiumrl_agent.cc

#include "third_party/blink/renderer/core/inspector/inspector_chromiumrl_agent.h"
#include "third_party/blink/renderer/core/css/css_property_name.h"

#include <vector>
#include <cmath>
#include <algorithm>
#include "base/base64.h"
#include "crypto/sha2.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_mutation_observer_init.h"
#include "third_party/blink/renderer/core/dom/mutation_record.h"
#include "third_party/blink/renderer/core/inspector/inspector_dom_agent.h"
#include "third_party/blink/renderer/core/inspector/protocol/chromium_rl.h"
#include "third_party/blink/renderer/core/dom/dom_node_ids.h"
#include "third_party/blink/renderer/core/inspector/identifiers_factory.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/layout/layout_box_model_object.h"
#include "third_party/blink/renderer/core/css/css_computed_style_declaration.h"
#include "third_party/blink/renderer/core/dom/node_traversal.h"
#include "third_party/blink/renderer/core/dom/element_traversal.h"
#include "third_party/blink/renderer/core/css/properties/longhands.h"
#include "third_party/blink/renderer/platform/graphics/color.h"

#include "base/logging.h"
#include "ui/gfx/geometry/point_conversions.h"
#include "third_party/blink/public/common/input/web_pointer_event.h"
#include "third_party/blink/renderer/core/inspector/identifiers_factory.h"
#include "cc/layers/layer.h"
#include "third_party/blink/renderer/platform/widget/frame_widget.h"
#include "third_party/blink/renderer/core/frame/web_frame_widget_impl.h"
#include "third_party/blink/renderer/core/inspector/inspected_frames.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/platform/wtf/uuid.h"
#include "third_party/blink/renderer/platform/heap/persistent.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_map.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/dom/node.h"
#include "third_party/blink/renderer/core/dom/document_timing.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/paint/timing/paint_timing.h"
#include "third_party/blink/renderer/core/html/forms/html_input_element.h"
#include "third_party/blink/renderer/core/html/forms/html_select_element.h"
#include "third_party/blink/renderer/core/html/forms/html_text_area_element.h"
#include "third_party/blink/renderer/core/html/forms/html_option_element.h"
#include "third_party/blink/renderer/core/html/html_element.h"
#include "third_party/blink/renderer/core/accessibility/ax_object_cache.h"
#include "third_party/blink/renderer/core/accessibility/ax_utilities_generated.h"
#include "third_party/blink/renderer/core/input/event_handler.h"
#include "third_party/blink/renderer/core/layout/hit_test_location.h"
#include "third_party/blink/renderer/core/layout/hit_test_request.h"
#include "third_party/blink/renderer/core/layout/hit_test_result.h"
#include "ui/accessibility/ax_enums.mojom-blink.h"

namespace blink {

namespace {
using AgentMap = GCedHeapHashMap<WeakMember<LocalFrame>, WeakMember<InspectorAgent>>;
Persistent<AgentMap>& GetAgentMap() {
  DEFINE_STATIC_LOCAL(Persistent<AgentMap>, map, (MakeGarbageCollected<AgentMap>()));
  return map;
}

constexpr int kStructuredDefaultMaxNodes = 700;
constexpr int kStructuredDefaultMaxTextChars = 24000;
constexpr unsigned kStructuredMaxDirectTextChars = 240;
constexpr unsigned kStructuredMaxSubtreeTextChars = 500;
constexpr unsigned kStructuredMaxAttributeValueChars = 160;
constexpr wtf_size_t kStructuredMaxAttributes = 12;
constexpr wtf_size_t kStructuredMaxChildRefs = 80;
constexpr int kAgentObservationContentScanMultiplier = 4;
constexpr int kAgentObservationContentScanSlack = 64;
constexpr int kAgentObservationMaxContentScanElements = 400;
constexpr unsigned kAgentObservationMaxContentTextChars = 700;

bool LooksLikeLargeStructuredValue(const String& value) {
  String trimmed = value.StripWhiteSpace();
  if (trimmed.length() > kStructuredMaxAttributeValueChars)
    return true;
  return trimmed.starts_with("{") || trimmed.starts_with("[") ||
         trimmed.contains("{\"") || trimmed.contains("function(") ||
         trimmed.contains("__NEXT_DATA__");
}

String TruncateStructuredString(const String& value,
                                unsigned max_chars,
                                bool* truncated) {
  String normalized = value.StripWhiteSpace();
  if (normalized.length() > max_chars) {
    if (truncated)
      *truncated = true;
    return normalized.substr(0, max_chars);
  }
  return normalized;
}
}

InspectorChromiumRLAgent::InspectorChromiumRLAgent(InspectedFrames* inspected_frames)
    : inspected_frames_(inspected_frames),
      trace_buffer_(MakeGarbageCollected<ChromiumRLTraceBuffer>()),
      capture_touch_traces_(&agent_state_, false),
      capture_layout_timings_(&agent_state_, false),
      capture_cls_attribution_(&agent_state_, false),
      capture_compositor_layers_(&agent_state_, false),
      enabled_(&agent_state_, false) {
  RegisterAgent();
}

InspectorChromiumRLAgent::~InspectorChromiumRLAgent() {
  UnregisterAgent();
}

void InspectorChromiumRLAgent::RegisterAgent() {
  GetAgentMap()->Set(inspected_frames_->Root(), this);
}

void InspectorChromiumRLAgent::UnregisterAgent() {
  GetAgentMap()->erase(inspected_frames_->Root());
}

InspectorChromiumRLAgent* InspectorChromiumRLAgent::GetAgentForFrame(LocalFrame* frame) {
  if (!frame) return nullptr;
  auto it = GetAgentMap()->find(frame);
  if (it != GetAgentMap()->end())
    return static_cast<InspectorChromiumRLAgent*>(it->value.Get());
  return nullptr;
}

void InspectorChromiumRLAgent::Restore() {
  if (enabled_.Get()) {
    enable(capture_touch_traces_.Get(), capture_layout_timings_.Get(), capture_cls_attribution_.Get(), capture_compositor_layers_.Get(),
           &current_session_id_);
  }
}

void InspectorChromiumRLAgent::DidCommitLoadForLocalFrame(LocalFrame* frame) {
  if (enabled_.Get() && frame == inspected_frames_->Root()) {
    // Re-register if frame changed
    RegisterAgent();

    // This is a reliable point to capture page-load metrics.
    if (capture_layout_timings_.Get() && frame->GetDocument()) {
      Document* doc = frame->GetDocument();
      FrameMetricsEntry metrics;

      // Capture Paint Timings
      PaintTiming& paint_timing = PaintTiming::From(*doc);
      metrics.first_contentful_paint = paint_timing.FirstContentfulPaintPresentation().since_origin().InMillisecondsF();
      // LCP requires a more complex detector, leaving as 0 for now.
      
      // Capture Document Timings
      const DocumentTiming& doc_timing = doc->GetTiming();
      metrics.dom_content_loaded = doc_timing.DomContentLoadedEventEnd().since_origin().InMillisecondsF();

      // Store these reliable metrics in the buffer for this frame.
      // We will merge them with layout data when getLayoutTimings is called.
      // A cleaner way is to have a dedicated metrics storage in the buffer.
      trace_buffer_->UpdateFrameMetrics(frame, metrics);
      LOG(INFO) << "ChromiumRL: DidCommitLoad - Captured FCP: " << metrics.first_contentful_paint;
    }
  }
}

protocol::Response InspectorChromiumRLAgent::enable(
    std::optional<bool> capture_touch_traces,
    std::optional<bool> capture_layout_timings,
    std::optional<bool> capture_cls_attribution,
    std::optional<bool> capture_compositor_layers,
    String* out_session_id) {
  
  if (enabled_.Get()) {
    *out_session_id = current_session_id_;
    return protocol::Response::Success();
  }
  enabled_.Set(true);
  capture_touch_traces_.Set(capture_touch_traces.value_or(false));
  capture_layout_timings_.Set(capture_layout_timings.value_or(false));
  capture_cls_attribution_.Set(capture_cls_attribution.value_or(false));
  capture_compositor_layers_.Set(capture_compositor_layers.value_or(false));
  
  current_session_id_ = IdentifiersFactory::CreateIdentifier();
  *out_session_id = current_session_id_;
  
  LOG(ERROR) << "ChromiumRL: [Lifecycle] Agent ENABLED agent=" << this 
             << " capture_compositor=" << capture_compositor_layers_.Get()
             << " session=" << current_session_id_;

  EnableInstrumentation();
  
  return protocol::Response::Success();
}

protocol::Response InspectorChromiumRLAgent::disable() {
  if (!enabled_.Get()) {
    return protocol::Response::ServerError("Not enabled");
  }
  enabled_.Set(false);
  DisableInstrumentation();
  trace_buffer_->Clear();
  current_session_id_ = String();
  
  return protocol::Response::Success();
}

protocol::Response InspectorChromiumRLAgent::getTouchTraces(
    std::optional<int> root_node_id,
    std::optional<double> start_time,
    std::optional<double> end_time,
    std::unique_ptr<protocol::Array<protocol::ChromiumRL::TouchTrace>>* traces,
    int* total_events,
    double* capture_duration) {
  
  *traces = std::make_unique<protocol::Array<protocol::ChromiumRL::TouchTrace>>();
  
  LOG(ERROR) << "ChromiumRL: [Command] getTouchTraces received.";
  const auto& stored_traces = trace_buffer_->GetTouchTraces();
  LOG(ERROR) << "ChromiumRL: [Command] Buffer contains " << stored_traces.size() << " traces. Converting...";

  *total_events = stored_traces.size();
  *capture_duration = 0; // Calculate if needed
  
  for (const auto& entry : stored_traces) {
    (*traces)->push_back(BuildTouchTrace(entry));
  }
  
  return protocol::Response::Success();
}

protocol::Response InspectorChromiumRLAgent::getLayoutTimings(
    const String& frame_id,
    std::optional<double> threshold_ms,
    std::optional<bool> include_zero_time,
    std::unique_ptr<protocol::Array<protocol::ChromiumRL::ElementLayoutTiming>>* elements,
    std::unique_ptr<protocol::ChromiumRL::FrameLayoutMetrics>* frame_metrics) {
  
  LocalFrame* frame = FrameForId(frame_id);
  if (!frame) return protocol::Response::ServerError("Frame not found");

  *elements = std::make_unique<protocol::Array<protocol::ChromiumRL::ElementLayoutTiming>>();
  const auto& timings = trace_buffer_->GetLayoutTimings(frame);
  
  for (const auto& entry : timings) {
    (*elements)->push_back(BuildLayoutTiming(entry));
  }
  
  const auto& metrics_entry = trace_buffer_->GetFrameMetrics(frame);
  auto metrics = protocol::ChromiumRL::FrameLayoutMetrics::create()
      .setFrameId(frame_id)
      .setTotalLayoutTimeMs(metrics_entry.total_layout_time_ms)
      .setTotalStyleRecalcTimeMs(metrics_entry.total_style_recalc_time_ms)
      .setTotalPaintTimeMs(metrics_entry.total_paint_time_ms)
      .setFirstContentfulPaint(metrics_entry.first_contentful_paint)
      .setLargestContentfulPaint(metrics_entry.largest_contentful_paint)
      .setTimeToFirstByte(metrics_entry.time_to_first_byte)
      .setDomContentLoaded(metrics_entry.dom_content_loaded)
      .setTotalBlockingTime(metrics_entry.total_blocking_time)
      .setLongTaskCount(metrics_entry.long_task_count)
      .build();
  *frame_metrics = std::move(metrics);

  return protocol::Response::Success();
}

protocol::Response InspectorChromiumRLAgent::getCLSAttribution(
    std::optional<double> min_shift_score,
    std::unique_ptr<protocol::Array<protocol::ChromiumRL::CLSEntry>>* entries,
    double* total_cls,
    bool* exceeds_good_threshold) {
  
  *entries = std::make_unique<protocol::Array<protocol::ChromiumRL::CLSEntry>>();
  const auto& stored = trace_buffer_->GetCLSEntries();
  
  for (const auto& entry : stored) {
    (*entries)->push_back(BuildCLSEntry(entry));
  }
  
  *total_cls = trace_buffer_->GetTotalCLS();
  *exceeds_good_threshold = *total_cls > 0.1; // Standard threshold
  
  return protocol::Response::Success();
}

protocol::Response InspectorChromiumRLAgent::getCompositorLayers(
    std::optional<bool> include_paint_info,
    std::optional<bool> include_transforms,
    std::unique_ptr<protocol::Array<protocol::ChromiumRL::CompositorLayer>>* layers,
    int* total_gpu_memory,
    int* pending_texture_uploads) {
  
  *layers = std::make_unique<protocol::Array<protocol::ChromiumRL::CompositorLayer>>();
  const auto& stored_layers = trace_buffer_->GetCompositorLayers();
  
  *total_gpu_memory = 0;
  *pending_texture_uploads = trace_buffer_->GetPendingTextureUploads();
  
  for (const auto& entry : stored_layers) {
    if (!entry.layer) continue;
    (*layers)->push_back(BuildCompositorLayer(
        entry.layer.get(), 
        include_paint_info.value_or(false), 
        include_transforms.value_or(false)
    ));
    *total_gpu_memory += entry.gpu_memory_bytes;
  }
  
  return protocol::Response::Success();
}

protocol::Response InspectorChromiumRLAgent::captureInteraction(
    const String& interaction_type,
    int target_node_id,
    std::optional<double> capture_duration_ms,
    std::unique_ptr<protocol::Array<protocol::ChromiumRL::TouchTrace>>* touch_traces,
    std::unique_ptr<protocol::Array<protocol::ChromiumRL::ElementLayoutTiming>>* layout_timings,
    std::unique_ptr<protocol::Array<protocol::ChromiumRL::CLSEntry>>* cls_entries,
    std::unique_ptr<protocol::Array<protocol::ChromiumRL::CompositorLayer>>* compositor_layers) {
  
  // 1. Get Touch Traces
  *touch_traces = std::make_unique<protocol::Array<protocol::ChromiumRL::TouchTrace>>();
  const auto& touches = trace_buffer_->GetTouchTraces();
  for (const auto& entry : touches) {
    (*touch_traces)->push_back(BuildTouchTrace(entry));
  }

  // 2. Get Layout Timings
  *layout_timings = std::make_unique<protocol::Array<protocol::ChromiumRL::ElementLayoutTiming>>();
  const auto& layouts = trace_buffer_->GetLayoutTimings(inspected_frames_->Root());
  for (const auto& entry : layouts) {
    (*layout_timings)->push_back(BuildLayoutTiming(entry));
  }

  // 3. Get CLS Entries
  *cls_entries = std::make_unique<protocol::Array<protocol::ChromiumRL::CLSEntry>>();
  const auto& cls_data = trace_buffer_->GetCLSEntries();
  for (const auto& entry : cls_data) {
    (*cls_entries)->push_back(BuildCLSEntry(entry));
  }

  // 4. Get Compositor Layers
  *compositor_layers = std::make_unique<protocol::Array<protocol::ChromiumRL::CompositorLayer>>();
  const auto& layers = trace_buffer_->GetCompositorLayers();
  for (const auto& entry : layers) {
    (*compositor_layers)->push_back(BuildCompositorLayer(entry.layer.get(), false, false));
  }

  return protocol::Response::Success();
}

protocol::Response InspectorChromiumRLAgent::getVisualHash(
    String* visual_hash,
    bool* has_visual_update) {
  *has_visual_update = false;
  int current_frame = 0;

  if (inspected_frames_->Root()) {
    if (auto* view = inspected_frames_->Root()->View()) {
      if (auto* root_layer = view->RootCcLayer()) {
        if (auto* host = root_layer->layer_tree_host()) {
          current_frame = host->SourceFrameNumber();
          trace_buffer_->RecordCompositorState(host);
        }
      }
    }
  }

  if (current_frame != last_source_frame_number_) {
    *has_visual_update = true;
    last_source_frame_number_ = current_frame;
  }

  StringBuilder builder;
  const auto& layers = trace_buffer_->GetCompositorLayers();

  for (const auto& entry : layers) {
    if (!entry.layer)
      continue;

    builder.AppendNumber(entry.layer->id());
    builder.AppendNumber(entry.layer->bounds().width());
    builder.AppendNumber(entry.layer->bounds().height());
    builder.AppendNumber(entry.layer->offset_to_transform_parent().x());
    builder.AppendNumber(entry.layer->offset_to_transform_parent().y());
    builder.AppendNumber(entry.gpu_memory_bytes);
  }

  std::string raw = builder.ToString().Utf8();
  std::string sha256 = crypto::SHA256HashString(raw);
  *visual_hash = String::FromUtf8(base::Base64Encode(sha256));

  return protocol::Response::Success();
}

void InspectorChromiumRLAgent::OnTouchEvent(const WebPointerEvent& event, Node* target_node) {
  // Check if we are even entering the hook
  LOG(INFO) << "ChromiumRL: [Hook] OnTouchEvent fired. Enabled: " << enabled_.Get() << ", Capture: " << capture_touch_traces_.Get();
  
  if (!enabled_.Get() || !capture_touch_traces_.Get()) return;
  
  TouchTraceEntry entry;
  entry.timestamp = base::Time::Now().InMillisecondsFSinceUnixEpoch();
  
  switch (event.GetType()) {
    case WebInputEvent::Type::kPointerDown: entry.event_type = "touchstart"; break;
    case WebInputEvent::Type::kPointerMove: entry.event_type = "touchmove"; break;
    case WebInputEvent::Type::kPointerUp: entry.event_type = "touchend"; break;
    case WebInputEvent::Type::kPointerCancel: entry.event_type = "touchcancel"; break;
    default: entry.event_type = "unknown"; break;
  }
  
  entry.x = event.PositionInWidget().x();
  entry.y = event.PositionInWidget().y();
  entry.pressure = event.force;
  entry.radius_x = event.width / 2.0;
  entry.radius_y = event.height / 2.0;
  entry.rotation_angle = event.rotation_angle;
  entry.pointer_id = event.id;
  
  if (target_node) {
    entry.node_id = IdentifiersFactory::IntIdForNode(target_node);
    entry.selector_path = BuildSelectorPath(target_node);
  }
  
  LOG(ERROR) << "ChromiumRL: [Capture] Data: " << entry.event_type << " @ (" << entry.x << "," << entry.y << ") Target: " << entry.selector_path;
  
  trace_buffer_->AddTouchTrace(std::move(entry));
}

void InspectorChromiumRLAgent::OnLayoutComplete(LocalFrame* frame) {
  if (!enabled_.Get() || !capture_layout_timings_.Get()) return;
  if (frame->View()) {
    LOG(INFO) << "ChromiumRL: OnLayoutComplete triggered.";
    
    // NOTE: FrameMetrics are now captured in DidCommitLoadForLocalFrame.
    FrameMetricsEntry empty_metrics; // Pass empty struct for now.
    
    trace_buffer_->RecordLayoutComplete(frame, frame->View()->GetLayoutView(), empty_metrics);
  }
}

void InspectorChromiumRLAgent::OnLayoutShift(
    double score,
    bool had_recent_input,
    const Vector<LayoutShiftTracker::Attribution>& attributions) {
  if (!enabled_.Get() || !capture_cls_attribution_.Get())
    return;

  CLSTraceEntry entry;
  entry.timestamp = base::TimeTicks::Now().since_origin().InSecondsF();
  entry.score = score;
  entry.had_recent_input = had_recent_input;

  for (const auto& attribution : attributions) {
    ShiftedElementEntry element;
    element.node_id = static_cast<int>(attribution.node_id);
    element.previous_rect = gfx::RectF(attribution.old_visual_rect);
    element.current_rect = gfx::RectF(attribution.new_visual_rect);
    // Rough estimate of contribution if not provided
    element.score_contribution = score / std::max(static_cast<int>(attributions.size()), 1);
    
    if (element.node_id != 0) {
      if (Node* node = DOMNodeIds::NodeForId(element.node_id)) {
        element.selector_path = BuildSelectorPath(node);
      }
    }
    
    entry.shifted_elements.push_back(std::move(element));
  }

  // Fire event to frontend
  if (GetFrontend()) {
    GetFrontend()->layoutShiftDetected(
        BuildCLSEntry(entry),
        trace_buffer_->GetTotalCLS() + entry.score);
  }

  trace_buffer_->AddCLSEntry(std::move(entry));
}

void InspectorChromiumRLAgent::OnCompositorCommit(const cc::LayerTreeHost* layer_tree_host) {
  LOG(ERROR) << "ChromiumRL: [Hook] OnCompositorCommit agent=" << this 
             << " enabled=" << enabled_.Get() 
             << " capture_compositor=" << capture_compositor_layers_.Get();
  if (!enabled_.Get() || !capture_compositor_layers_.Get()) return;
  trace_buffer_->RecordCompositorState(layer_tree_host);

  if (GetFrontend()) {
    const auto& layers = trace_buffer_->GetCompositorLayers();
    int total_memory = 0;
    for (const auto& l : layers) total_memory += l.gpu_memory_bytes;
    
    // We pass total_memory as the delta for now, or 0 if we don't track diffs.
    GetFrontend()->layerTreeChanged(layers.size(), total_memory);
  }
}

void InspectorChromiumRLAgent::Trace(Visitor* visitor) const {
  visitor->Trace(inspected_frames_);
  visitor->Trace(trace_buffer_);
  visitor->Trace(dom_diff_observer_);
  visitor->Trace(dom_diff_delegate_);
  InspectorBaseAgent::Trace(visitor);
}

// Helpers

std::unique_ptr<protocol::ChromiumRL::TouchTrace> 
InspectorChromiumRLAgent::BuildTouchTrace(const TouchTraceEntry& entry) {
  std::unique_ptr<protocol::ChromiumRL::TouchTrace> trace = 
      protocol::ChromiumRL::TouchTrace::create()
      .setTimestamp(entry.timestamp)
      .setEventType(entry.event_type)
      .setX(entry.x)
      .setY(entry.y)
      .setWasHandled(entry.was_handled)
      .setTriggeredScroll(entry.triggered_scroll)
      .setTriggeredNavigation(entry.triggered_navigation)
      .setPointerId(entry.pointer_id)
      .build();

  if (entry.node_id != 0)
    trace->setNodeId(entry.node_id);
  if (!entry.selector_path.empty())
    trace->setSelectorPath(entry.selector_path);

  return trace;
}

std::unique_ptr<protocol::ChromiumRL::ElementLayoutTiming> 
InspectorChromiumRLAgent::BuildLayoutTiming(const LayoutTimingEntry& entry) {
  auto element = protocol::ChromiumRL::ElementLayoutTiming::create()
      .setNodeId(entry.node_id)
      .setSelectorPath(entry.selector_path)
      .setTagName(entry.tag_name)
      .setLayoutTimeMs(entry.layout_time_ms)
      .setLayoutInvalidations(entry.layout_invalidations)
      .setStyleRecalcTimeMs(entry.style_recalc_time_ms)
      .setStyleRecalcCount(entry.style_recalc_count)
      .setPaintTimeMs(entry.paint_time_ms)
      .setBoundingBox(protocol::DOM::Rect::create()
          .setX(entry.bounding_box.x())
          .setY(entry.bounding_box.y())
          .setWidth(entry.bounding_box.width())
          .setHeight(entry.bounding_box.height())
          .build())
      .setIsInCriticalPath(entry.is_in_critical_path)
      .setCausedSiblingReflow(entry.caused_sibling_reflow)
      .build();
      
  if (entry.parent_node_id != 0) {
      element->setParentNodeId(entry.parent_node_id);
  }
  return element;
}

std::unique_ptr<protocol::ChromiumRL::CLSEntry> 
InspectorChromiumRLAgent::BuildCLSEntry(const CLSTraceEntry& entry) {
  auto shifted_elements = std::make_unique<protocol::Array<protocol::ChromiumRL::ShiftedElement>>();
  for (const auto& element : entry.shifted_elements) {
    shifted_elements->push_back(protocol::ChromiumRL::ShiftedElement::create()
        .setNodeId(element.node_id)
        .setSelectorPath(element.selector_path)
        .setPreviousRect(protocol::DOM::Rect::create()
            .setX(element.previous_rect.x())
            .setY(element.previous_rect.y())
            .setWidth(element.previous_rect.width())
            .setHeight(element.previous_rect.height())
            .build())
        .setCurrentRect(protocol::DOM::Rect::create()
            .setX(element.current_rect.x())
            .setY(element.current_rect.y())
            .setWidth(element.current_rect.width())
            .setHeight(element.current_rect.height())
            .build())
        .setShiftDistance(element.shift_distance)
        .setScoreContribution(element.score_contribution)
        .build());
  }
  
  auto sources = std::make_unique<protocol::Array<protocol::ChromiumRL::ShiftSource>>();
  
  return protocol::ChromiumRL::CLSEntry::create()
      .setTimestamp(entry.timestamp)
      .setScore(entry.score)
      .setHadRecentInput(entry.had_recent_input)
      .setShiftedElements(std::move(shifted_elements))
      .setSources(std::move(sources))
      .setSessionWindowId(entry.session_window_id)
      .build();
}

std::unique_ptr<protocol::ChromiumRL::CompositorLayer> 
InspectorChromiumRLAgent::BuildCompositorLayer(const cc::Layer* layer, bool paint_info, bool transforms) {
  return protocol::ChromiumRL::CompositorLayer::create()
      .setLayerId(String::Number(layer->id()))
      .setBounds(protocol::DOM::Rect::create()
          .setX(layer->offset_to_transform_parent().x())
          .setY(layer->offset_to_transform_parent().y())
          .setWidth(layer->bounds().width())
          .setHeight(layer->bounds().height())
          .build())
      .setGpuMemoryBytes(0)
      .setCompositingReasons(std::make_unique<protocol::Array<protocol::ChromiumRL::CompositingReason>>())
      .setLayerType(protocol::ChromiumRL::LayerTypeEnum::Picture)
      .setIsVisible(!layer->hide_layer_and_subtree())
      .setIsOpaque(layer->contents_opaque())
      .setPaintCount(0)
      .setUsesGpuRasterization(false)
      .build();
}

LocalFrame* InspectorChromiumRLAgent::FrameForId(const String& frame_id) {
  return IdentifiersFactory::FrameById(inspected_frames_, frame_id);
}

void InspectorChromiumRLAgent::EnableInstrumentation() {
  // Hook up to probes/instrumentation if needed.
  // Currently the hooks are manual calls from other parts of code.
}

void InspectorChromiumRLAgent::DisableInstrumentation() {
  // Disconnect.
}

String InspectorChromiumRLAgent::BuildSelectorPath(Node* node) {
  if (!node) return String();
  
  Node* current = node;
  StringBuilder builder;
  Vector<String> path;
  
  while (current && !current->IsDocumentNode()) {
    if (auto* element = DynamicTo<Element>(current)) {
      StringBuilder step;
      
      // 1. ID (Strongest)
      if (element->HasID()) {
          step.Append(element->tagName().ToAsciiLower());
          step.Append("#");
          step.Append(element->GetIdAttribute());
      } 
      // 2. Class (Medium)
      else if (element->HasClass() && element->ClassNames().size() > 0) {
          step.Append(element->tagName().ToAsciiLower());
          step.Append(".");
          step.Append(element->ClassNames()[0]);
          
          int same_class_count = 0;
          int my_index = 0;
          
          if (element->parentNode()) {
              for (Element* sibling = ElementTraversal::FirstChild(*element->parentNode()); sibling; sibling = ElementTraversal::NextSibling(*sibling)) {
                 if (sibling->HasClass() && sibling->ClassNames().size() > 0 && sibling->ClassNames()[0] == element->ClassNames()[0]) {
                     same_class_count++;
                     if (sibling == element) {
                         my_index = same_class_count;
                     }
                 }
              }
          }
          
          if (same_class_count > 1) {
             step.Append(":nth-of-type(");
             step.AppendNumber(my_index);
             step.Append(")");
          }
      } 
      // 3. Tag + nth-of-type (Fallback)
      else {
        step.Append(element->tagName().ToAsciiLower());

        int index = 1;
        bool unique = true;
        
        if (element->parentNode()) {
            for (Element* sibling = ElementTraversal::PreviousSibling(*element); sibling;
                 sibling = ElementTraversal::PreviousSibling(*sibling)) {
              if (sibling->tagName() == element->tagName()) {
                index++;
                unique = false;
              }
            }
            if (unique) {
                 for (Element* sibling = ElementTraversal::NextSibling(*element); sibling;
                 sibling = ElementTraversal::NextSibling(*sibling)) {
                    if (sibling->tagName() == element->tagName()) {
                        unique = false;
                        break;
                    }
                }
            }
        }

        if (!unique) {
            step.Append(":nth-of-type(");
            step.AppendNumber(index);
            step.Append(")");
        }
      }
      path.push_back(step.ToString());
    }
    current = current->parentNode();
  }
  
  for (int i = path.size() - 1; i >= 0; i--) {
    builder.Append(path[i]);
    if (i > 0) builder.Append(" > ");
  }
  
  return builder.ToString();
}

String InspectorChromiumRLAgent::GetNodeFingerprint(Node* node) {
  StringBuilder builder;
  builder.Append(node->nodeName());

  if (auto* element = DynamicTo<Element>(node)) {
    if (element->HasID()) {
      builder.Append("#");
      builder.Append(element->GetIdAttribute());
    }
    
    // Check for "role"
    const AtomicString& role = element->getAttribute(html_names::kRoleAttr);
    if (!role.IsNull()) {
        builder.Append("|role:");
        builder.Append(role);
    }
    
    // Check for "aria-label"
    const AtomicString& aria = element->getAttribute(html_names::kAriaLabelAttr);
    if (!aria.IsNull()) {
        builder.Append("|aria:");
        builder.Append(aria);
    }

    // Include text content for elements to differentiate <button>A</button> vs <button>B</button>
    // Expanded to Headings/Paragraphs to fix "Banana" matching "Ingredients" in Pass 1.
    bool should_use_text = !ElementTraversal::FirstChild(*element) || 
        element->HasTagName(html_names::kATag) || 
        element->HasTagName(html_names::kButtonTag) ||
        element->HasTagName(html_names::kH1Tag) ||
        element->HasTagName(html_names::kH2Tag) ||
        element->HasTagName(html_names::kH3Tag) ||
        element->HasTagName(html_names::kH4Tag) ||
        element->HasTagName(html_names::kH5Tag) ||
        element->HasTagName(html_names::kH6Tag) ||
        element->HasTagName(html_names::kPTag) ||
        element->HasTagName(html_names::kStrongTag) ||
        element->HasTagName(html_names::kSpanTag) ||
        element->HasTagName(html_names::kLiTag) ||
        element->HasTagName(html_names::kLabelTag);

    if (should_use_text) {
        String text = node->textContent();
        if (!text.empty()) {
            builder.Append("|t:");
            builder.Append(text.StripWhiteSpace().substr(0, 30));
        }
    }
  } else if (node->IsTextNode()) {
      String text = node->nodeValue().StripWhiteSpace();
      if (!text.empty()) {
          builder.Append("|text:");
          // Limit text length to avoid huge keys, but keep enough for uniqueness
          builder.Append(text.substr(0, 50)); 
      }
  }
  
  return builder.ToString();
}

// ===========================================================================
// DOM Diff Implementation
// ===========================================================================

class InspectorChromiumRLAgent::DOMDiffObserver final
    : public MutationObserver::Delegate {
 public:
  explicit DOMDiffObserver(InspectorChromiumRLAgent* agent) : agent_(agent) {}

  ExecutionContext* GetExecutionContext() const override {
    return agent_->inspected_frames_->Root()->GetDocument()->GetExecutionContext();
  }

  void Deliver(const MutationRecordVector& records,
               MutationObserver&) override {
    agent_->OnDOMMutation(records);
  }

  void Trace(Visitor* visitor) const override {
    visitor->Trace(agent_);
    MutationObserver::Delegate::Trace(visitor);
  }

 private:
  Member<InspectorChromiumRLAgent> agent_;
};

protocol::Response InspectorChromiumRLAgent::startDOMDiff() {
  if (dom_diff_observer_)
    return protocol::Response::Success();

  Document* document = inspected_frames_->Root()->GetDocument();
  if (!document)
    return protocol::Response::ServerError("No document to observe");

  dom_diff_delegate_ = MakeGarbageCollected<DOMDiffObserver>(this);
  dom_diff_observer_ = MutationObserver::Create(dom_diff_delegate_);

  MutationObserverInit* init = MutationObserverInit::Create();
  init->setChildList(true);
  init->setAttributes(true);
  init->setCharacterData(true);
  init->setSubtree(true);
  init->setAttributeOldValue(true);
  init->setCharacterDataOldValue(true);

  dom_diff_observer_->observe(document, init, ASSERT_NO_EXCEPTION);

  return protocol::Response::Success();
}

protocol::Response InspectorChromiumRLAgent::stopDOMDiff() {
  if (dom_diff_observer_) {
    dom_diff_observer_->disconnect();
    dom_diff_observer_ = nullptr;
    dom_diff_delegate_ = nullptr;
  }
  return protocol::Response::Success();
}

void InspectorChromiumRLAgent::OnDOMMutation(
    const HeapVector<Member<MutationRecord>>& records) {
  auto diff =
      std::make_unique<protocol::Array<protocol::ChromiumRL::NodeDiff>>();

  for (const auto& record : records) {
    Node* target = record->target();
    if (!target)
      continue;

    int node_id = IdentifiersFactory::IntIdForNode(target);
    String selector_path = BuildSelectorPath(target);

    auto node_diff = protocol::ChromiumRL::NodeDiff::create()
                         .setNodeId(node_id)
                         .setSelectorPath(selector_path)
                         .build();

    bool has_change = false;

    if (record->type() == "attributes") {
      if (auto* element = DynamicTo<Element>(target)) {
        auto attr_mods = std::make_unique<
            protocol::Array<protocol::ChromiumRL::AttributeModification>>();
        attr_mods->push_back(
            protocol::ChromiumRL::AttributeModification::create()
                .setName(record->attributeName())
                .setOldValue(record->oldValue())
                .setNewValue(element->getAttribute(record->attributeName()))
                .build());
        node_diff->setAttributeModifications(std::move(attr_mods));
        has_change = true;
      }
    } else if (record->type() == "characterData") {
      node_diff->setCharacterDataModification(
          protocol::ChromiumRL::CharacterDataModification::create()
              .setOldData(record->oldValue())
              .setNewData(target->nodeValue())
              .build());
      has_change = true;
    } else if (record->type() == "childList") {
      // Handle added nodes
      if (record->addedNodes() && record->addedNodes()->length() > 0) {
        auto added = std::make_unique<protocol::Array<protocol::ChromiumRL::NodeStructure>>();
        for (unsigned i = 0; i < record->addedNodes()->length(); i++) {
          Node* node = record->addedNodes()->item(i);
          auto structure = protocol::ChromiumRL::NodeStructure::create()
              .setNodeId(IdentifiersFactory::IntIdForNode(node))
              .setNodeName(node->nodeName())
              .build();
          if (node->nodeValue()) {
              structure->setNodeValue(node->nodeValue());
          }
          added->push_back(std::move(structure));
        }
        node_diff->setAddedNodes(std::move(added));
        has_change = true;
      }

      // Handle removed nodes
      if (record->removedNodes() && record->removedNodes()->length() > 0) {
        auto removed = std::make_unique<protocol::Array<protocol::ChromiumRL::NodeStructure>>();
        for (unsigned i = 0; i < record->removedNodes()->length(); i++) {
          Node* node = record->removedNodes()->item(i);
          auto structure = protocol::ChromiumRL::NodeStructure::create()
              .setNodeId(IdentifiersFactory::IntIdForNode(node))
              .setNodeName(node->nodeName())
              .build();
          if (node->nodeValue()) {
              structure->setNodeValue(node->nodeValue());
          }
          removed->push_back(std::move(structure));
        }
        node_diff->setRemovedNodes(std::move(removed));
        has_change = true;
      }
    }

    if (has_change) {
      diff->push_back(std::move(node_diff));
    }
  }

  if (diff->size() > 0 && GetFrontend()) {
    GetFrontend()->domDiffOccurred(std::move(diff));
  }
}

// ===========================================================================
// State Snapshot Implementation
// ===========================================================================

protocol::Response InspectorChromiumRLAgent::captureStateSnapshot(
    std::unique_ptr<protocol::Array<protocol::ChromiumRL::SnapshotNode>>*
        snapshot) {
  *snapshot =
      std::make_unique<protocol::Array<protocol::ChromiumRL::SnapshotNode>>();
  Document* document = inspected_frames_->Root()->GetDocument();
  if (!document)
    return protocol::Response::ServerError("No document");

  for (Node& node : NodeTraversal::StartsAt(*document)) {
    // Skip internal nodes (shadow roots etc unless needed)
    if (node.IsDocumentNode()) continue;
    
    // Skip whitespace-only text nodes and script/style text children
    if (!node.IsElementNode()) {
      if (node.nodeValue()) {
        String normalized = NormalizeTextContent(node.nodeValue());
        if (normalized.length() == 0) {
          continue;
        }
        if (node.parentNode() && node.parentNode()->IsElementNode()) {
          Element* parent = To<Element>(node.parentNode());
          if (parent->HasTagName(html_names::kScriptTag) ||
              parent->HasTagName(html_names::kStyleTag))
            continue;
        }
      } else {
        continue;
      }
    }
    
    (*snapshot)->push_back(BuildSnapshotNode(&node));
  }

  return protocol::Response::Success();
}

// Helper to normalize text for comparison and fingerprinting
String NormalizeTextForDiff(const String& text) {
    if (text.IsNull()) return String();
    return text.StripWhiteSpace();
}

std::unique_ptr<protocol::ChromiumRL::SnapshotNode> 
InspectorChromiumRLAgent::BuildSnapshotNode(Node* node) {
  auto rect = protocol::DOM::Rect::create()
      .setX(0).setY(0).setWidth(0).setHeight(0).build();

  if (node->GetLayoutObject()) {
    gfx::RectF bounding_box = node->GetLayoutObject()->AbsoluteBoundingBoxRectF();
    rect->setX(bounding_box.x());
    rect->setY(bounding_box.y());
    rect->setWidth(bounding_box.width());
    rect->setHeight(bounding_box.height());
  }

  auto snapshot = protocol::ChromiumRL::SnapshotNode::create()
      .setNodeId(IdentifiersFactory::IntIdForNode(node))
      .setNodeName(node->nodeName())
      .setSelectorPath(BuildSelectorPath(node))
      .setFingerprint(GetNodeFingerprint(node))
      .setRect(std::move(rect))
      .build();

  if (node->parentNode()) {
    snapshot->setParentId(IdentifiersFactory::IntIdForNode(node->parentNode()));
  }
  
  if (node->nodeValue()) {
    String normalized_value = NormalizeTextContent(node->nodeValue());
    if (normalized_value.length() > 0) {
      snapshot->setNodeValue(normalized_value);
    }
  }

  if (auto* element = DynamicTo<Element>(node)) {
    auto attrs = std::make_unique<protocol::Array<String>>();
    for (const auto& attr : element->Attributes()) {
      attrs->push_back(attr.GetName().ToString() + "=" + attr.Value());
    }
    snapshot->setAttributes(std::move(attrs));

    // Capture basic styles if computed style is available
    if (const ComputedStyle* style = element->GetComputedStyle()) {
        auto style_snap = protocol::ChromiumRL::SnapshotStyle::create()
            .setDisplay(style->Display() == EDisplay::kNone ? "none" : "visible") // Simplified
            .setVisibility(style->Visibility() == EVisibility::kVisible ? "visible" : "hidden")
            .setOpacity(style->Opacity())
            .setColor(style->VisitedDependentColor(GetCSSPropertyColor()).SerializeAsCSSColor())
            .setBackgroundColor(style->VisitedDependentColor(GetCSSPropertyBackgroundColor()).SerializeAsCSSColor())
            .build();
        snapshot->setStyle(std::move(style_snap));
    }
  }

  return snapshot;
}

double InspectorChromiumRLAgent::CalculateIoU(protocol::DOM::Rect* r1, protocol::DOM::Rect* r2) {

  // If both are empty/invisible, they match perfectly.                                                                                                                                               │
  if ((r1->getWidth() <= 0 || r1->getHeight() <= 0) &&
      (r2->getWidth() <= 0 || r2->getHeight() <= 0)) {
    return 1.0;
  } 
  double x1 = std::max(r1->getX(), r2->getX());
  double y1 = std::max(r1->getY(), r2->getY());
  double x2 = std::min(r1->getX() + r1->getWidth(), r2->getX() + r2->getWidth());
  double y2 = std::min(r1->getY() + r1->getHeight(), r2->getY() + r2->getHeight());

  if (x2 < x1 || y2 < y1) return 0.0;

  double intersection = (x2 - x1) * (y2 - y1);
  double area1 = r1->getWidth() * r1->getHeight();
  double area2 = r2->getWidth() * r2->getHeight();
  double union_area = area1 + area2 - intersection;

  if (union_area <= 0) return 0.0;
  return intersection / union_area;
}

protocol::Response InspectorChromiumRLAgent::computeStateDiff(
    std::unique_ptr<protocol::Array<protocol::ChromiumRL::SnapshotNode>>
        target_snapshot,
    std::unique_ptr<protocol::ChromiumRL::SnapshotDiff>* diff) {

  // 1. Index the target snapshot by Selector Path
  HashMap<String, protocol::ChromiumRL::SnapshotNode*> target_map;
  for (const auto& node : *target_snapshot) {
    target_map.Set(node->getSelectorPath(), node.get());
  }

  Document* document = inspected_frames_->Root()->GetDocument();
  if (!document)
    return protocol::Response::ServerError("No document");

  // Ensure layout is up-to-date before comparison
  document->UpdateStyleAndLayout(DocumentUpdateReason::kInspector);

  auto visual_mismatches = std::make_unique<protocol::Array<protocol::ChromiumRL::VisualMismatch>>();
  auto content_mismatches = std::make_unique<protocol::Array<protocol::ChromiumRL::ContentMismatch>>();
  auto missing_nodes = std::make_unique<protocol::Array<String>>();
  auto extra_nodes = std::make_unique<protocol::Array<String>>();

  // Collect live snapshot as we traverse (reuse instead of building twice)
  auto live_snapshot = std::make_unique<protocol::Array<protocol::ChromiumRL::SnapshotNode>>();

  double total_iou = 0;
  int matched_nodes = 0;

  // 2. Traverse Live DOM
  for (Node& node : NodeTraversal::StartsAt(*document)) {
    if (node.IsDocumentNode()) continue;

    // Skip whitespace-only text nodes (same logic as captureStateSnapshot)
    if (!node.IsElementNode()) {
      if (node.nodeValue()) {
        String normalized = NormalizeTextContent(node.nodeValue());
        if (normalized.length() == 0) {
          continue; // Skip whitespace-only text nodes
        }
      } else {
        continue; // Skip text nodes with no value
      }
    }

    // Build snapshot node once (reuse for comparison and final snapshot)
    auto live_node_snap = BuildSnapshotNode(&node);
    String selector = live_node_snap->getSelectorPath();
    
    // Add to live snapshot (for ALL nodes: matched + extra)
    live_snapshot->push_back(live_node_snap->Clone());
    
    auto it = target_map.find(selector);

    if (it == target_map.end()) {
      extra_nodes->push_back(selector);
      continue; // Still add to live_snapshot above, but skip comparison
    }

    // Match Found!
    protocol::ChromiumRL::SnapshotNode* target_node = it->value;
    target_map.erase(it); // Mark as visited
    matched_nodes++;

    // A. Visual Diff (IoU) - reuse the snapshot we already built
    double iou = CalculateIoU(live_node_snap->getRect(), target_node->getRect());
    total_iou += iou;

    // Only report visual mismatch if IoU is significantly different
    // AND both rects have non-zero dimensions (to avoid false positives from layout timing)
    bool target_has_size = target_node->getRect()->getWidth() > 0 && target_node->getRect()->getHeight() > 0;
    bool live_has_size = live_node_snap->getRect()->getWidth() > 0 && live_node_snap->getRect()->getHeight() > 0;

    if (iou < 0.95 && target_has_size && live_has_size) { // Threshold for visual mismatch
      visual_mismatches->push_back(protocol::ChromiumRL::VisualMismatch::create()
          .setSelectorPath(selector)
          .setIou(iou)
          .setExpectedRect(target_node->getRect()->Clone())
          .setActualRect(live_node_snap->getRect()->Clone())
          .build());
    }

    // B. Content Diff (Text)
    String live_text = node.nodeValue();
    String target_text = target_node->hasNodeValue() ? target_node->getNodeValue("") : String();
    
    // Normalize nulls
    if (live_text.IsNull()) live_text = "";
    if (target_text.IsNull()) target_text = "";
    
    // Normalize text content (same normalization as in BuildSnapshotNode)
    String normalized_live = NormalizeTextContent(live_text);
    String normalized_target = NormalizeTextContent(target_text);
    
    // If both are whitespace-only (normalized to empty), skip comparison
    if (normalized_live.length() == 0 && normalized_target.length() == 0) {
      continue;
    }
    
    // Compare normalized versions
    if (normalized_live != normalized_target) {
      content_mismatches->push_back(protocol::ChromiumRL::ContentMismatch::create()
          .setSelectorPath(selector)
          .setExpectedValue(target_text)
          .setActualValue(live_text)
          .build());
    }
  }

  // 3. Remaining items in map are missing
  for (auto& entry : target_map) {
    missing_nodes->push_back(entry.key);
  }

  // 4. Compute Score
  double similarity = 0;
  if (matched_nodes > 0) {
    similarity = total_iou / (matched_nodes + missing_nodes->size() + extra_nodes->size());
  }

  // Use the live snapshot we collected during traversal (no need to traverse again)
  *diff = protocol::ChromiumRL::SnapshotDiff::create()
      .setSimilarityScore(similarity)
      .setVisualMismatches(std::move(visual_mismatches))
      .setContentMismatches(std::move(content_mismatches))
      .setMissingNodes(std::move(missing_nodes))
      .setExtraNodes(std::move(extra_nodes))
      .setTargetSnapshot(std::move(live_snapshot))  // Use collected live snapshot (not target!)
      .build();

  return protocol::Response::Success();
}

// ===========================================================================
// Advanced DOM Diff Implementation (3-Pass Weighted Scorer)
// ===========================================================================

String InspectorChromiumRLAgent::BuildXPath(Node* node) {
  if (!node || node->IsDocumentNode()) return "/";
  
  StringBuilder builder;
  Vector<String> path;
  Node* current = node;

  while (current && !current->IsDocumentNode()) {
    if (auto* element = DynamicTo<Element>(current)) {
      StringBuilder step;
      step.Append("/");
      step.Append(element->tagName().ToAsciiLower());
      
      int index = 1;
      for (Element* sibling = ElementTraversal::PreviousSibling(*element); sibling;
           sibling = ElementTraversal::PreviousSibling(*sibling)) {
        if (sibling->tagName() == element->tagName()) {
          index++;
        }
      }
      step.Append("[");
      step.AppendNumber(index);
      step.Append("]");
      path.push_back(step.ToString());
    }
    current = current->parentNode();
  }

  for (int i = path.size() - 1; i >= 0; i--) {
    builder.Append(path[i]);
  }
  
  return builder.ToString();
}

String InspectorChromiumRLAgent::NormalizeTextContent(const String& text) {
  if (text.length() == 0) return text; 
  
  String trimmed = text.StripWhiteSpace();
  if (trimmed.length() == 0) return String();
  
  StringBuilder normalized;
  bool last_was_whitespace = false;
  
  for (unsigned i = 0; i < trimmed.length(); i++) {
    UChar c = trimmed[i];
    bool is_whitespace = (c == ' ' || c == '\n' || c == '\r' || c == '\t');
    
    if (is_whitespace) {
      if (!last_was_whitespace) {
        normalized.Append(' ');
        last_was_whitespace = true;
      }
    } else {
      normalized.Append(c);
      last_was_whitespace = false;
    }
  }

  return normalized.ToString().StripWhiteSpace();
}

// Helper for Lightweight Context (Avoids JSON Bloat)
std::unique_ptr<protocol::ChromiumRL::RichNode> 
InspectorChromiumRLAgent::BuildLightweightNode(Node* node) {
  int node_id = IdentifiersFactory::IntIdForNode(node);
  String tag_name = node->nodeName();
  
  // Minimal text content
  String text_content;
  if (node->IsTextNode()) {
      text_content = node->nodeValue();
  } else if (node->IsElementNode()) {
      Element* element = DynamicTo<Element>(node);
      if (!ElementTraversal::FirstChild(*element) || 
          element->HasTagName(html_names::kButtonTag) || 
          element->HasTagName(html_names::kATag) || 
          element->HasTagName(html_names::kSpanTag)) {
          text_content = node->textContent();
      }
  }
  if (text_content.length() > 50) text_content = text_content.substr(0, 50) + "...";

  auto rich_node = protocol::ChromiumRL::RichNode::create()
    .setNodeId(node_id)
    .setTagName(tag_name)
    .setTextContent(NormalizeTextContent(text_content))
    .build();

  // Only add identifying attributes (ID, Class, Name, Value)
  if (auto* element = DynamicTo<Element>(node)) {
    auto attributes = std::make_unique<protocol::Array<protocol::ChromiumRL::DOMAttribute>>();
    for (const auto& attr : element->Attributes()) {
      String name = attr.GetName().ToString();
      if (name == "id" || name == "class" || name == "name" || name == "value" || name == "type" || name == "placeholder") {
          attributes->push_back(protocol::ChromiumRL::DOMAttribute::create()
            .setName(name)
            .setValue(attr.Value())
            .build());
      }
    }
    rich_node->setAttributes(std::move(attributes));
  }
  
  return rich_node;
}

std::unique_ptr<protocol::ChromiumRL::RichNode> 
InspectorChromiumRLAgent::BuildRichNode(Node* node) {
  int node_id = IdentifiersFactory::IntIdForNode(node);
  String tag_name = node->nodeName();
  
  auto default_styles = protocol::ChromiumRL::DOMComputedStyle::create()
        .setDisplay("none")
        .setVisibility("hidden")
        .setOpacity("0")
        .setColor("")
        .setBackgroundColor("")
        .setFontSize("0")
        .setFontWeight("normal")
        .setPadding("")
        .setMargin("")
        .setWidth("")
        .setHeight("")
        .setPosition("static")
        .setOverflow("visible")
        .setWhiteSpace("normal")
        .setTextAlign("start")
        .setLineHeight("normal")
        .setTextDecoration("none")
        .setBorder("")
        .setBorderRadius("")
        .build();

  // Bounds
  auto bounds = protocol::DOM::Rect::create().setX(0).setY(0).setWidth(0).setHeight(0).build();
  bool is_in_viewport = false;
  if (node->GetLayoutObject()) {
    gfx::RectF box = node->GetLayoutObject()->AbsoluteBoundingBoxRectF();
    bounds->setX(box.x());
    bounds->setY(box.y());
    bounds->setWidth(box.width());
    bounds->setHeight(box.height());
    is_in_viewport = (box.width() > 0 && box.height() > 0);
  }

  // Text Content: Use textContent() safely
  String text_content;
  if (node->IsTextNode()) {
      text_content = node->nodeValue();
  } else if (node->IsElementNode()) {
      // Only fetch aggregated text for specific content elements or leaves
      // to avoid OOM/Crash on large containers like <body>
      Element* element = DynamicTo<Element>(node);
      bool is_code_tag = element->HasTagName(html_names::kScriptTag) ||
                         element->HasTagName(html_names::kStyleTag);

      bool is_content_tag = element->HasTagName(html_names::kButtonTag) ||
                            element->HasTagName(html_names::kATag) ||
                            element->HasTagName(html_names::kH1Tag) ||
                            element->HasTagName(html_names::kH2Tag) ||
                            element->HasTagName(html_names::kH3Tag) ||
                            element->HasTagName(html_names::kH4Tag) ||
                            element->HasTagName(html_names::kH5Tag) ||
                            element->HasTagName(html_names::kH6Tag) ||
                            element->HasTagName(html_names::kPTag) ||
                            element->HasTagName(html_names::kSpanTag) ||
                            element->HasTagName(html_names::kLiTag) ||
                            element->HasTagName(html_names::kLabelTag) ||
                            element->HasTagName(html_names::kOptionTag);

      if (is_code_tag) {
          String raw = node->textContent();
          if (!raw.empty()) {
              std::string hash = crypto::SHA256HashString(raw.Utf8());
              text_content = "sha256:" + String::FromUtf8(base::Base64Encode(hash));
          }
      } else if (is_content_tag || !ElementTraversal::FirstChild(*element)) {
          text_content = node->textContent();
          if (text_content.length() > 1000) text_content = text_content.substr(0, 1000);
      }
  }

  auto rich_node = protocol::ChromiumRL::RichNode::create()
    .setNodeId(node_id)
    .setTagName(tag_name)
    .setStablePath(BuildSelectorPath(node))
    .setCssSelector(BuildSelectorPath(node))
    .setXpath(BuildXPath(node))
    .setFingerprint(GetNodeFingerprint(node))
    .setIsVisible(false)
    .setIsInViewport(is_in_viewport)
    .setZIndex(0)
    .setTextContent(NormalizeTextContent(text_content)) // Use aggregated text
    .setSiblingIndex(0)
    .setAttributes(std::make_unique<protocol::Array<protocol::ChromiumRL::DOMAttribute>>()) 
    .setKeyStyles(std::move(default_styles)) 
    .setParentId(0) 
    .setBounds(std::move(bounds))
    .build();

  if (node->parentNode()) {
    rich_node->setParentId(IdentifiersFactory::IntIdForNode(node->parentNode()));
  }

  if (auto* element = DynamicTo<Element>(node)) {
    auto attributes = std::make_unique<protocol::Array<protocol::ChromiumRL::DOMAttribute>>();
    for (const auto& attr : element->Attributes()) {
      attributes->push_back(protocol::ChromiumRL::DOMAttribute::create()
        .setName(attr.GetName().ToString())
        .setValue(attr.Value())
        .build());
    }
    rich_node->setAttributes(std::move(attributes));
    
    if (const ComputedStyle* style = element->GetComputedStyle()) {
      rich_node->setIsVisible(style->Visibility() == EVisibility::kVisible && style->Display() != EDisplay::kNone);
      rich_node->setZIndex(style->ZIndex());
      
      auto* computed = MakeGarbageCollected<CSSComputedStyleDeclaration>(element);
      auto key_styles = protocol::ChromiumRL::DOMComputedStyle::create()
        .setDisplay(computed->GetPropertyValue(CSSPropertyID::kDisplay))
        .setVisibility(computed->GetPropertyValue(CSSPropertyID::kVisibility))
        .setOpacity(computed->GetPropertyValue(CSSPropertyID::kOpacity))
        .setColor(computed->GetPropertyValue(CSSPropertyID::kColor))
        .setBackgroundColor(computed->GetPropertyValue(CSSPropertyID::kBackgroundColor))
        .setFontSize(computed->GetPropertyValue(CSSPropertyID::kFontSize))
        .setFontWeight(computed->GetPropertyValue(CSSPropertyID::kFontWeight))
        .setPadding(computed->GetPropertyValue(CSSPropertyID::kPadding))
        .setMargin(computed->GetPropertyValue(CSSPropertyID::kMargin))
        .setWidth(computed->GetPropertyValue(CSSPropertyID::kWidth))
        .setHeight(computed->GetPropertyValue(CSSPropertyID::kHeight))
        .setPosition(computed->GetPropertyValue(CSSPropertyID::kPosition))
        .setOverflow(computed->GetPropertyValue(CSSPropertyID::kOverflow))
        .setWhiteSpace(computed->GetPropertyValue(CSSPropertyID::kWhiteSpace))
        .setTextAlign(computed->GetPropertyValue(CSSPropertyID::kTextAlign))
        .setLineHeight(computed->GetPropertyValue(CSSPropertyID::kLineHeight))
        .setTextDecoration(computed->GetPropertyValue(CSSPropertyID::kTextDecoration))
        .setBorder(computed->GetPropertyValue(CSSPropertyID::kBorder))
        .setBorderRadius(computed->GetPropertyValue(CSSPropertyID::kBorderRadius))
        .build();
        
      rich_node->setKeyStyles(std::move(key_styles));
    }
  }

  return rich_node;
}

protocol::Response InspectorChromiumRLAgent::saveDOMState(
    std::unique_ptr<protocol::ChromiumRL::RichDOMState>* out_state) {
    
  Document* document = inspected_frames_->Root()->GetDocument();
  if (!document) return protocol::Response::ServerError("No document");
  
  document->UpdateStyleAndLayout(DocumentUpdateReason::kInspector);

  auto nodes = std::make_unique<protocol::Array<protocol::ChromiumRL::RichNode>>();
  
  for (Node& node : NodeTraversal::StartsAt(*document)) {
    if (node.IsDocumentNode()) continue;
    if (node.IsTextNode()) {
      if (NormalizeTextContent(node.nodeValue()).empty()) continue;
      if (node.parentNode() && node.parentNode()->IsElementNode()) {
        Element* parent = To<Element>(node.parentNode());
        if (parent->HasTagName(html_names::kScriptTag) ||
            parent->HasTagName(html_names::kStyleTag))
          continue;
      }
    }
    
    nodes->push_back(BuildRichNode(&node));
  }
  
  auto viewport = protocol::DOM::Rect::create().setX(0).setY(0).setWidth(0).setHeight(0).build();
  if (document->View()) {
     gfx::Rect rect =
         document->View()->LayoutViewport()->VisibleContentRect(kExcludeScrollbars);
     viewport->setX(rect.x()); viewport->setY(rect.y());
     viewport->setWidth(rect.width()); viewport->setHeight(rect.height());
  }

  *out_state = protocol::ChromiumRL::RichDOMState::create() 
    .setNodes(std::move(nodes))
    .setViewport(std::move(viewport))
    .setTitle(document->title())
    .setUrl(document->Url().GetString())
    .build();

  return protocol::Response::Success();
}

// Helper for Euclidean Distance
double CalculateDistance(protocol::DOM::Rect* r1, protocol::DOM::Rect* r2) {
    double x1 = r1->getX(), y1 = r1->getY();
    double x2 = r2->getX(), y2 = r2->getY();
    return std::sqrt(std::pow(x2 - x1, 2) + std::pow(y2 - y1, 2));
}

// Helper for Text Similarity (Jaccard over word tokens)
double ComputeTextSimilarity(const String& s1, const String& s2) {
    if (s1.empty() && s2.empty()) return 1.0;
    if (s1.empty() || s2.empty()) return 0.0;
    if (s1 == s2) return 1.0;

    const std::string str1 = s1.Utf8();
    const std::string str2 = s2.Utf8();

    HashSet<String> words1;
    HashSet<String> words2;

    auto tokenize = [](const std::string& str, HashSet<String>& words) {
        size_t start = 0;
        for (size_t i = 0; i <= str.size(); ++i) {
            if (i == str.size() || str[i] == ' ' || str[i] == '\n' ||
                str[i] == '\t' || str[i] == '\r') {
                if (i > start) {
                    words.insert(String::FromUtf8(
                        std::string_view(str).substr(start, i - start)));
                }
                start = i + 1;
            }
        }
    };

    tokenize(str1, words1);
    tokenize(str2, words2);

    if (words1.empty() && words2.empty()) return 1.0;

    unsigned intersection = 0;
    for (const auto& w : words1) {
        if (words2.Contains(w)) intersection++;
    }

    unsigned union_size = words1.size() + words2.size() - intersection;
    if (union_size == 0) return 1.0;

    return static_cast<double>(intersection) / static_cast<double>(union_size);
}

double ComputeStyleSimilarity(protocol::ChromiumRL::RichNode* ref_node, 
                              protocol::ChromiumRL::RichNode* live_node,
                              const Vector<protocol::ChromiumRL::RichNode*>& all_ref_nodes,
                              const Vector<protocol::ChromiumRL::RichNode*>& all_live_nodes) {
    auto* ref_style = ref_node->getKeyStyles(nullptr);
    auto* live_style = live_node->getKeyStyles(nullptr);

    // If nodes are text nodes, find their parents and use their styles instead.
    if (ref_node->getTagName() == "#text" && ref_node->hasParentId()) {
        for (auto* node : all_ref_nodes) {
            if (node->getNodeId() == ref_node->getParentId(0)) {
                ref_style = node->getKeyStyles(nullptr);
                break;
            }
        }
    }
    if (live_node->getTagName() == "#text" && live_node->hasParentId()) {
        for (auto* node : all_live_nodes) {
            if (node->getNodeId() == live_node->getParentId(0)) {
                live_style = node->getKeyStyles(nullptr);
                break;
            }
        }
    }
    
    if (!ref_style || !live_style) return 0.0;

    // Parse "Npx" → double. Returns -1 if not a px value.
    auto ParsePx = [](const String& s) -> double {
        if (!s.ends_with("px")) return -1.0;
        std::string s_utf8 = s.substr(0, s.length() - 2).Utf8();
        double v = 0;
        bool ok = base::StringToDouble(s_utf8, &v);
        return ok ? v : -1.0;
    };

    // Numeric similarity: 1 - |a-b| / max(|b|, 1), clamped [0,1].
    auto NumericSim = [](double a, double b) -> double {
        return std::max(0.0, 1.0 - std::abs(a - b) / std::max(std::abs(b), 1.0));
    };

    // Parse "rgb(r, g, b)" → r,g,b in [0,255]. Returns false if unparseable.
    auto ParseColor = [](const String& s, double& r, double& g, double& b) -> bool {
        if (!s.starts_with("rgb(") || !s.ends_with(")")) return false;
        String inner = s.substr(4, s.length() - 5);
        Vector<String> parts = inner.Split(",");
        if (parts.size() < 3) return false;
        std::string r_utf8 = parts[0].StripWhiteSpace().Utf8();
        std::string g_utf8 = parts[1].StripWhiteSpace().Utf8();
        std::string b_utf8 = parts[2].StripWhiteSpace().Utf8();
        bool ok1 = base::StringToDouble(r_utf8, &r);
        bool ok2 = base::StringToDouble(g_utf8, &g);
        bool ok3 = base::StringToDouble(b_utf8, &b);
        return ok1 && ok2 && ok3;
    };

    // Color similarity via RGB euclidean distance.
    auto ColorSim = [&ParseColor](const String& a, const String& b) -> double {
        if (a == b) return 1.0;
        double r1, g1, b1, r2, g2, b2;
        if (!ParseColor(a, r1, g1, b1) || !ParseColor(b, r2, g2, b2))
            return 0.0;
        double dist = std::sqrt((r1-r2)*(r1-r2) + (g1-g2)*(g1-g2) + (b1-b2)*(b1-b2));
        return std::max(0.0, 1.0 - dist / (std::sqrt(3.0) * 255.0));
    };

    // Numeric property similarity (px values), fallback to exact match.
    auto PropSim = [&ParsePx, &NumericSim](const String& a, const String& b) -> double {
        if (a == b) return 1.0;
        double va = ParsePx(a), vb = ParsePx(b);
        if (va >= 0 && vb >= 0) return NumericSim(va, vb);
        return 0.0;
    };

    double score = 0.0;
    double total = 0.0;

    auto Add = [&](double s) { score += s; total += 1.0; };

    Add(ColorSim(ref_style->getColor(), live_style->getColor()));
    Add(ColorSim(ref_style->getBackgroundColor(), live_style->getBackgroundColor()));
    Add(PropSim(ref_style->getFontSize(), live_style->getFontSize()));
    Add(PropSim(ref_style->getFontWeight(), live_style->getFontWeight()));
    {
        const String& oa = ref_style->getOpacity();
        const String& ob = live_style->getOpacity();
        if (oa == ob) { Add(1.0); }
        else {
            std::string oa_utf8 = oa.Utf8();
            std::string ob_utf8 = ob.Utf8();
            double va = 0;
            double vb = 0;
            bool ok1 = base::StringToDouble(oa_utf8, &va);
            bool ok2 = base::StringToDouble(ob_utf8, &vb);
            Add((ok1 && ok2) ? NumericSim(va, vb) : 0.0);
        }
    }
    Add((ref_style->getVisibility() == live_style->getVisibility()) ? 1.0 : 0.0);
    Add((ref_style->getDisplay() == live_style->getDisplay()) ? 1.0 : 0.0);
    Add((ref_style->getPosition() == live_style->getPosition()) ? 1.0 : 0.0);
    Add((ref_style->getOverflow() == live_style->getOverflow()) ? 1.0 : 0.0);
    Add((ref_style->getWhiteSpace() == live_style->getWhiteSpace()) ? 1.0 : 0.0);
    Add((ref_style->getTextAlign() == live_style->getTextAlign()) ? 1.0 : 0.0);
    Add((ref_style->getTextDecoration() == live_style->getTextDecoration()) ? 1.0 : 0.0);
    Add(PropSim(ref_style->getPadding(), live_style->getPadding()));
    Add(PropSim(ref_style->getMargin(), live_style->getMargin()));
    Add(PropSim(ref_style->getWidth(), live_style->getWidth()));
    Add(PropSim(ref_style->getHeight(), live_style->getHeight()));
    Add(PropSim(ref_style->getLineHeight(), live_style->getLineHeight()));
    Add((ref_style->getBorder() == live_style->getBorder()) ? 1.0 : 0.0);
    Add((ref_style->getBorderRadius() == live_style->getBorderRadius()) ? 1.0 : 0.0);

    return total > 0 ? score / total : 1.0;
}

protocol::Response InspectorChromiumRLAgent::compareDOMState(
    std::unique_ptr<protocol::ChromiumRL::RichDOMState> reference_state,
    std::unique_ptr<protocol::Array<protocol::ChromiumRL::DeltaNode>> delta_nodes_param,
    std::unique_ptr<protocol::ChromiumRL::DOMDiffResult>* out_result) {
    
  Document* document = inspected_frames_->Root()->GetDocument();
  if (!document) return protocol::Response::ServerError("No document");
  
  document->UpdateStyleAndLayout(DocumentUpdateReason::kInspector);
  auto empty_rect = protocol::DOM::Rect::create()
      .setX(0)
      .setY(0)
      .setWidth(0)
      .setHeight(0)
      .build();
  
  // --- 1. Collection Phase ---
  // Collect Reference Nodes
  Vector<protocol::ChromiumRL::RichNode*> ref_nodes;
  for (size_t i = 0; i < reference_state->getNodes()->size(); ++i) {
    ref_nodes.push_back(reference_state->getNodes()->at(i).get());
  }

  // Collect Live Nodes
  HeapVector<Member<Node>> live_dom_nodes; 
  Vector<std::unique_ptr<protocol::ChromiumRL::RichNode>> live_rich_nodes_storage;
  Vector<protocol::ChromiumRL::RichNode*> live_nodes; 

  for (Node& node : NodeTraversal::StartsAt(*document)) {
    if (node.IsDocumentNode()) continue;
    if (node.IsTextNode()) {
      if (NormalizeTextContent(node.nodeValue()).empty()) continue;
      if (node.parentNode() && node.parentNode()->IsElementNode()) {
        Element* parent = To<Element>(node.parentNode());
        if (parent->HasTagName(html_names::kScriptTag) ||
            parent->HasTagName(html_names::kStyleTag))
          continue;
      }
    }
    
    live_dom_nodes.push_back(&node);
    live_rich_nodes_storage.push_back(BuildRichNode(&node));
    live_nodes.push_back(live_rich_nodes_storage.back().get());
  }

  // Matching Status Tracking
  HashSet<int> matched_ref_indices;
  HashSet<int> matched_live_indices;
  Vector<std::pair<int, int>> matches; 

  // --- Pass 1: Strict Match (Fingerprint) ---
  for (int i = 0; i < static_cast<int>(ref_nodes.size()); ++i) {
      if (matched_ref_indices.Contains(i)) continue;
      for (int j = 0; j < static_cast<int>(live_nodes.size()); ++j) {
          if (matched_live_indices.Contains(j)) continue;
          
          if (ref_nodes[i]->getFingerprint(String()) == live_nodes[j]->getFingerprint(String())) {
              matched_ref_indices.insert(i);
              matched_live_indices.insert(j);
              matches.push_back(std::make_pair(i, j));
              break; 
          }
      }
  }

  // --- Pass 2: Content Match + Proximity ---
  for (int i = 0; i < static_cast<int>(ref_nodes.size()); ++i) {
      if (matched_ref_indices.Contains(i)) continue;
      if (!ref_nodes[i]) continue;
      
      int best_j = -1;
      double min_dist = std::numeric_limits<double>::max();

      for (int j = 0; j < static_cast<int>(live_nodes.size()); ++j) {
          if (matched_live_indices.Contains(j)) continue;
          if (!live_nodes[j]) continue;

          String ref_text = ref_nodes[i]->getTextContent();
          String live_text = live_nodes[j]->getTextContent();
          
          if (!ref_text.empty() && ref_text == live_text) {
              auto* ref_bounds = ref_nodes[i]->getBounds(empty_rect.get());
              auto* live_bounds = live_nodes[j]->getBounds(empty_rect.get());
              if (!ref_bounds || !live_bounds) continue;
              
              double dist = CalculateDistance(ref_bounds, live_bounds);
              if (dist < min_dist) {
                  min_dist = dist;
                  best_j = j;
              }
          }
      }

      if (best_j != -1) {
          matched_ref_indices.insert(i);
          matched_live_indices.insert(best_j);
          matches.push_back(std::make_pair(i, best_j));
      }
  }

  // --- Pass 3: Visual Fuzzy Match (Weighted Scorer) ---
  struct CandidateMatch {
      int ref_idx;
      int live_idx;
      double score;
  };
  Vector<CandidateMatch> candidates;

  const double WEIGHT_TEXT = 0.70; // Tuned for stricter text matching
  const double WEIGHT_IOU  = 0.20;
  const double WEIGHT_TAG  = 0.10;
  const double MATCH_THRESHOLD = 0.75; // Stricter threshold

  for (int i = 0; i < static_cast<int>(ref_nodes.size()); ++i) {
      if (matched_ref_indices.Contains(i)) continue;
      if (!ref_nodes[i]) continue;
      
      for (int j = 0; j < static_cast<int>(live_nodes.size()); ++j) {
          if (matched_live_indices.Contains(j)) continue;
          if (!live_nodes[j]) continue;

          auto* r = ref_nodes[i];
          auto* l = live_nodes[j];

          double text_sim = 0;
          String t1 = r->getTextContent();
          String t2 = l->getTextContent();
          if (!t1.empty() || !t2.empty()) {
              text_sim = ComputeTextSimilarity(t1, t2);
          } else {
              text_sim = 1.0; 
          }

          auto* r_bounds = r->getBounds(empty_rect.get());
          auto* l_bounds = l->getBounds(empty_rect.get());
          if (!r_bounds || !l_bounds) continue;
          
          double iou = CalculateIoU(r_bounds, l_bounds);
          double tag_match = (r->getTagName() == l->getTagName()) ? 1.0 : 0.0;

          if (text_sim > 0.9 && t1.length() > 5) {
              text_sim += 0.2; 
          }

          double score = (text_sim * WEIGHT_TEXT) + (iou * WEIGHT_IOU) + (tag_match * WEIGHT_TAG);
          
          if (score > MATCH_THRESHOLD) {
              candidates.push_back(CandidateMatch{i, j, score});
          }
      }
  }

  std::sort(candidates.begin(), candidates.end(), [](const CandidateMatch& a, const CandidateMatch& b) {
      return a.score > b.score;
  });

  for (const auto& c : candidates) {
      if (matched_ref_indices.Contains(c.ref_idx)) continue;
      if (matched_live_indices.Contains(c.live_idx)) continue;
      
      // Validate indices
      if (c.ref_idx < 0 || c.ref_idx >= static_cast<int>(ref_nodes.size())) continue;
      if (c.live_idx < 0 || c.live_idx >= static_cast<int>(live_nodes.size())) continue;
      if (!ref_nodes[c.ref_idx] || !live_nodes[c.live_idx]) continue;

      matched_ref_indices.insert(c.ref_idx);
      matched_live_indices.insert(c.live_idx);
      matches.push_back(std::make_pair(c.ref_idx, c.live_idx));
  }

    // --- 4. Diff Generation ---
    auto insertions = std::make_unique<protocol::Array<protocol::ChromiumRL::DiffOperation>>();
    auto deletions = std::make_unique<protocol::Array<protocol::ChromiumRL::DiffOperation>>();
    auto moves = std::make_unique<protocol::Array<protocol::ChromiumRL::DiffOperation>>();
    auto attr_changes = std::make_unique<protocol::Array<protocol::ChromiumRL::DiffOperation>>();
    auto text_changes = std::make_unique<protocol::Array<protocol::ChromiumRL::DiffOperation>>();
    auto layout_changes = std::make_unique<protocol::Array<protocol::ChromiumRL::DiffOperation>>();
    auto style_changes = std::make_unique<protocol::Array<protocol::ChromiumRL::DiffOperation>>();
    auto type_changes = std::make_unique<protocol::Array<protocol::ChromiumRL::DiffOperation>>();
    double total_text_sim = 0.0;
    double total_layout_sim = 0.0;
    double total_style_sim = 0.0;
  
    for (const auto& pair : matches) {
        // Validate indices
        if (pair.first < 0 || pair.first >= static_cast<int>(ref_nodes.size())) continue;
        if (pair.second < 0 || pair.second >= static_cast<int>(live_nodes.size())) continue;
        if (pair.second >= static_cast<int>(live_dom_nodes.size())) continue;
        
        auto* ref = ref_nodes[pair.first];
        auto* live = live_nodes[pair.second];
        if (!ref || !live) continue;
        
        int live_id = live->getNodeId();
        
        // Check bounds before using
        auto* ref_bounds = ref->getBounds(empty_rect.get());
        auto* live_bounds = live->getBounds(empty_rect.get());
        if (ref_bounds && live_bounds) {
            total_text_sim += ComputeTextSimilarity(ref->getTextContent(), live->getTextContent());
            total_layout_sim += CalculateIoU(ref_bounds, live_bounds);
        }
        total_style_sim += ComputeStyleSimilarity(ref, live, ref_nodes, live_nodes);

              // Skip #text nodes if they are children of content-holding elements (STYLE, TITLE, etc.)
              if (live->getTagName() == "#text") {
                  Node* n = live_dom_nodes[pair.second];
                  if (n && n->parentNode() && n->parentNode()->IsElementNode()) {
                      Element* parent = To<Element>(n->parentNode());
                      if (parent && (parent->HasTagName(html_names::kStyleTag) || 
                          parent->HasTagName(html_names::kTitleTag) ||
                          parent->HasTagName(html_names::kScriptTag) ||
                          parent->HasTagName(html_names::kTextareaTag) ||
                          parent->HasTagName(html_names::kOptionTag))) {
                          continue;
                      }
                  }
              }  
              
        // Use Lightweight Context
        Node* live_node = live_dom_nodes[pair.second];
        if (!live_node) continue;
        auto live_details = BuildLightweightNode(live_node);
  
        // Type Change
        if (ref->getTagName() != live->getTagName()) {
            type_changes->push_back(protocol::ChromiumRL::DiffOperation::create()
              .setNodeId(live_id)
              .setType("type")
              .setTagName(live->getTagName())
              .setStablePath(live->getStablePath(String()))
              .setCssSelector(live->getCssSelector(String()))
              .setXpath(live->getXpath(String()))
              .setOldValue(ref->getTagName())
              .setNewValue(live->getTagName())
              .setNodeDetails(std::move(live_details)) // Add Context
              .build());
              
            // Rebuild details
            if (live_node) {
                live_details = BuildLightweightNode(live_node);
            }
        }
  
        // Text Change
        if (ComputeTextSimilarity(ref->getTextContent(), live->getTextContent()) < 1.0) {
             text_changes->push_back(protocol::ChromiumRL::DiffOperation::create()
              .setNodeId(live_id)
              .setType("text")
              .setTagName(live->getTagName())
              .setStablePath(live->getStablePath(String()))
              .setCssSelector(live->getCssSelector(String()))
              .setXpath(live->getXpath(String()))
              .setOldValue(ref->getTextContent())
              .setNewValue(live->getTextContent())
              .setNodeDetails(std::move(live_details)) // Add Context
              .build());
             if (live_node) {
                 live_details = BuildLightweightNode(live_node);
             }
        }
  
        // Layout Change (Position)
        auto* ref_bounds_for_layout = ref->getBounds(empty_rect.get());
        auto* live_bounds_for_layout = live->getBounds(empty_rect.get());
        if (ref_bounds_for_layout && live_bounds_for_layout && 
            CalculateIoU(ref_bounds_for_layout, live_bounds_for_layout) < 0.95) {
             layout_changes->push_back(protocol::ChromiumRL::DiffOperation::create()
              .setNodeId(live_id)
              .setType("layout")
              .setTagName(live->getTagName())
              .setStablePath(live->getStablePath(String()))
              .setCssSelector(live->getCssSelector(String()))
              .setXpath(live->getXpath(String()))
              .setLayout(protocol::ChromiumRL::DiffLayout::create()
                  .setOldBounds(ref_bounds_for_layout->Clone())
                  .setNewBounds(live_bounds_for_layout->Clone())
                  .build())
              .setNodeDetails(std::move(live_details)) // Add Context
              .build());
             if (live_node) {
                 live_details = BuildLightweightNode(live_node);
             }
        }
        
        // Attribute Diff
        HashSet<String> checked_attrs;
        if (ref->getAttributes(nullptr)) {
            for (const auto& attr : *ref->getAttributes(nullptr)) {
                String name = attr->getName();
                checked_attrs.insert(name);
                String ref_val = attr->getValue();
                String live_val = String();
                bool found = false;
                
                if (live->getAttributes(nullptr)) {
                    for (const auto& l_attr : *live->getAttributes(nullptr)) {
                        if (l_attr->getName() == name) {
                            live_val = l_attr->getValue();
                            found = true;
                            break;
                        }
                    }
                }
                
                if (!found) {
                     attr_changes->push_back(protocol::ChromiumRL::DiffOperation::create()
                      .setNodeId(live_id)
                      .setType("attribute_delete")
                      .setTagName(live->getTagName())
                      .setStablePath(live->getStablePath(String()))
                      .setCssSelector(live->getCssSelector(String()))
                      .setXpath(live->getXpath(String()))
                      .setOldValue(name + "=\"" + ref_val + "\"")
                      .setNewValue("")
                      .setNodeDetails(std::move(live_details)) // Context
                      .build());
                     if (live_node) {
                         live_details = BuildLightweightNode(live_node);
                     }
                } else if (ref_val != live_val) {
                     attr_changes->push_back(protocol::ChromiumRL::DiffOperation::create()
                      .setNodeId(live_id)
                      .setType("attribute_modify")
                      .setTagName(live->getTagName())
                      .setStablePath(live->getStablePath(String()))
                      .setCssSelector(live->getCssSelector(String()))
                      .setXpath(live->getXpath(String()))
                      .setOldValue(name + "=\"" + ref_val + "\"")
                      .setNewValue(name + "=\"" + live_val + "\"")
                      .setNodeDetails(std::move(live_details)) // Context
                      .build());
                     if (live_node) {
                         live_details = BuildLightweightNode(live_node);
                     }
                }
            }
        }
        
        if (live->getAttributes(nullptr)) {
            for (const auto& l_attr : *live->getAttributes(nullptr)) {
                if (!checked_attrs.Contains(l_attr->getName())) {
                     attr_changes->push_back(protocol::ChromiumRL::DiffOperation::create()
                      .setNodeId(live_id)
                      .setType("attribute_insert")
                      .setTagName(live->getTagName())
                      .setStablePath(live->getStablePath(String()))
                      .setCssSelector(live->getCssSelector(String()))
                      .setXpath(live->getXpath(String()))
                      .setOldValue("")
                      .setNewValue(l_attr->getName() + "=\"" + l_attr->getValue() + "\"")
                      .setNodeDetails(std::move(live_details)) // Context
                      .build());
                     if (live_node) {
                         live_details = BuildLightweightNode(live_node);
                     }
                }
            }
        }

        // Style Diff
        auto* ref_style_node = ref;
        auto* live_style_node = live;

        // For text nodes, find their parent element to get styles
        if (ref->getTagName() == "#text" && ref->hasParentId()) {
            for (auto* node : ref_nodes) {
                if (node && node->getNodeId() == ref->getParentId(0)) {
                    ref_style_node = node;
                    break;
                }
            }
        }
        if (live->getTagName() == "#text" && live->hasParentId()) {
            for (auto* node : live_nodes) {
                if (node && node->getNodeId() == live->getParentId(0)) {
                    live_style_node = node;
                    break;
                }
            }
        }

        if (!ref_style_node || !live_style_node) continue;  // Safety check
        
        auto* ref_style = ref_style_node->getKeyStyles(nullptr);
        auto* live_style = live_style_node->getKeyStyles(nullptr);

        if (ref_style && live_style) {
            auto diff_styles = std::make_unique<protocol::Array<protocol::ChromiumRL::DiffStyle>>();

            auto DiffProp = [&](const char* name, const String& old_val, const String& new_val) {
                if (old_val != new_val) {
                    diff_styles->push_back(protocol::ChromiumRL::DiffStyle::create().setProperty(name).setOld(old_val).setNew(new_val).build());
                }
            };

            DiffProp("color", ref_style->getColor(), live_style->getColor());
            DiffProp("background-color", ref_style->getBackgroundColor(), live_style->getBackgroundColor());
            DiffProp("font-size", ref_style->getFontSize(), live_style->getFontSize());
            DiffProp("font-weight", ref_style->getFontWeight(), live_style->getFontWeight());
            DiffProp("opacity", ref_style->getOpacity(), live_style->getOpacity());
            DiffProp("visibility", ref_style->getVisibility(), live_style->getVisibility());
            DiffProp("display", ref_style->getDisplay(), live_style->getDisplay());
            DiffProp("padding", ref_style->getPadding(), live_style->getPadding());
            DiffProp("margin", ref_style->getMargin(), live_style->getMargin());
            DiffProp("width", ref_style->getWidth(), live_style->getWidth());
            DiffProp("height", ref_style->getHeight(), live_style->getHeight());
            DiffProp("position", ref_style->getPosition(), live_style->getPosition());
            DiffProp("overflow", ref_style->getOverflow(), live_style->getOverflow());
            DiffProp("white-space", ref_style->getWhiteSpace(), live_style->getWhiteSpace());
            DiffProp("text-align", ref_style->getTextAlign(), live_style->getTextAlign());
            DiffProp("line-height", ref_style->getLineHeight(), live_style->getLineHeight());
            DiffProp("text-decoration", ref_style->getTextDecoration(), live_style->getTextDecoration());
            DiffProp("border", ref_style->getBorder(), live_style->getBorder());
            DiffProp("border-radius", ref_style->getBorderRadius(), live_style->getBorderRadius());

            if (!diff_styles->empty()) {
                style_changes->push_back(protocol::ChromiumRL::DiffOperation::create()
                    .setNodeId(live_id)
                    .setType("style")
                    .setTagName(live->getTagName())
                    .setStablePath(live->getStablePath(String()))
                    .setCssSelector(live->getCssSelector(String()))
                    .setXpath(live->getXpath(String()))
                    .setStyles(std::move(diff_styles))
                    .setNodeDetails(std::move(live_details))
                    .build());
                if (live_node) {
                    live_details = BuildLightweightNode(live_node);
                }
            }
        }
    }
  // Insertions
  for (int i = 0; i < static_cast<int>(live_nodes.size()); ++i) {
      if (!matched_live_indices.Contains(i)) {
          if (i >= static_cast<int>(live_dom_nodes.size())) continue;
          Node* n = live_dom_nodes[i];
          if (!n) continue;
          insertions->push_back(protocol::ChromiumRL::DiffOperation::create()
            .setNodeId(IdentifiersFactory::IntIdForNode(n))
            .setType("insert")
            .setTagName(n->nodeName())
            .setStablePath(BuildSelectorPath(n))
            .setCssSelector(BuildSelectorPath(n))
            .setXpath(BuildXPath(n))
            .setNodeDetails(BuildRichNode(n)) 
            .build());
      }
  }

  // Deletions
  for (int i = 0; i < static_cast<int>(ref_nodes.size()); ++i) {
      if (!matched_ref_indices.Contains(i)) {
          auto* r = ref_nodes[i];
          deletions->push_back(protocol::ChromiumRL::DiffOperation::create()
            .setNodeId(r->getNodeId())
            .setType("delete")
            .setTagName(r->getTagName())
            .setStablePath(r->getStablePath(String()))
            .setCssSelector(r->getCssSelector(String()))
            .setXpath(r->getXpath(String()))
            .build());
      }
  }

  // ---------------------------------------------------------------------------
  // Scoped Scoring: score only nodes that differ between gold and agent.
  //
  // Instead of averaging across all ~4000 nodes (where 3995 identical nodes
  // drown the signal from 5 changed nodes), we:
  //   1. Collect all changed node paths (from every diff type)
  //   2. Cluster by path proximity (first-5-segment prefix)
  //   3. Compute one LCA per cluster (longest common prefix of paths)
  //   4. Score only within each LCA subtree
  //   5. Weighted-average across clusters
  //
  // If no changed nodes → perfect match → all scores = 1.0
  // If LCA subtree > 500 nodes → too broad → fall back to full-DOM score
  // ---------------------------------------------------------------------------

  // Step 1: Collect stablePaths of all changed nodes.
  Vector<String> changed_paths;

  // From matched pairs: flag as changed if any dimension differs.
  for (const auto& pair : matches) {
    if (pair.first < 0 || pair.first >= static_cast<int>(ref_nodes.size())) continue;
    if (pair.second < 0 || pair.second >= static_cast<int>(live_nodes.size())) continue;
    auto* r = ref_nodes[pair.first];
    auto* l = live_nodes[pair.second];
    if (!r || !l) continue;

    bool text_diff = r->getTextContent() != l->getTextContent();

    // Compare actual attribute values, not just count.
    bool attr_diff = false;
    auto* r_attrs = r->getAttributes(nullptr);
    auto* l_attrs = l->getAttributes(nullptr);
    if (r_attrs && l_attrs) {
      if (r_attrs->size() != l_attrs->size()) {
        attr_diff = true;
      } else {
        for (size_t ai = 0; ai < r_attrs->size(); ++ai) {
          if (r_attrs->at(ai)->getName() != l_attrs->at(ai)->getName() ||
              r_attrs->at(ai)->getValue() != l_attrs->at(ai)->getValue()) {
            attr_diff = true;
            break;
          }
        }
      }
    } else if (r_attrs || l_attrs) {
      attr_diff = true;
    }

    auto* rb = r->getBounds(empty_rect.get());
    auto* lb = l->getBounds(empty_rect.get());
    bool layout_diff = rb && lb && CalculateIoU(rb, lb) < 0.95;

    auto* rs = r->getKeyStyles(nullptr);
    auto* ls = l->getKeyStyles(nullptr);
    bool style_diff = rs && ls && (
        rs->getColor() != ls->getColor() ||
        rs->getBackgroundColor() != ls->getBackgroundColor() ||
        rs->getFontSize() != ls->getFontSize() ||
        rs->getFontWeight() != ls->getFontWeight() ||
        rs->getOpacity() != ls->getOpacity() ||
        rs->getVisibility() != ls->getVisibility() ||
        rs->getDisplay() != ls->getDisplay() ||
        rs->getPadding() != ls->getPadding() ||
        rs->getMargin() != ls->getMargin() ||
        rs->getWidth() != ls->getWidth() ||
        rs->getHeight() != ls->getHeight() ||
        rs->getPosition() != ls->getPosition() ||
        rs->getOverflow() != ls->getOverflow() ||
        rs->getWhiteSpace() != ls->getWhiteSpace() ||
        rs->getTextAlign() != ls->getTextAlign() ||
        rs->getLineHeight() != ls->getLineHeight() ||
        rs->getTextDecoration() != ls->getTextDecoration() ||
        rs->getBorder() != ls->getBorder() ||
        rs->getBorderRadius() != ls->getBorderRadius());

    if (text_diff || attr_diff || layout_diff || style_diff) {
      String sp = r->getXpath(String());
      // Skip root nodes — they always "change" due to child text/layout
      // and would expand LCA scope to the entire page.
      String tag = r->getTagName().ToAsciiLower();
      if (!sp.empty() && tag != "html" && tag != "body" && tag != "head") {
        changed_paths.push_back(sp);
        LOG(WARNING) << "ChromiumRL LCA: changed node xpath=" << sp.Utf8()
                     << " text_diff=" << text_diff
                     << " attr_diff=" << attr_diff
                     << " layout_diff=" << layout_diff
                     << " style_diff=" << style_diff;
      }
    }
  }

  // Insertions: live node not in gold.
  for (int i = 0; i < static_cast<int>(live_nodes.size()); ++i) {
    if (!matched_live_indices.Contains(i) && live_nodes[i]) {
      String sp = live_nodes[i]->getXpath(String());
      if (!sp.empty()) changed_paths.push_back(sp);
      LOG(WARNING) << "ChromiumRL LCA: insertion xpath=" << sp.Utf8();
    }
  }

  // Deletions: gold node not in live.
  for (int i = 0; i < static_cast<int>(ref_nodes.size()); ++i) {
    if (!matched_ref_indices.Contains(i) && ref_nodes[i]) {
      String sp = ref_nodes[i]->getXpath(String());
      if (!sp.empty()) changed_paths.push_back(sp);
      LOG(WARNING) << "ChromiumRL LCA: deletion xpath=" << sp.Utf8();
    }
  }

  LOG(WARNING) << "ChromiumRL LCA: total changed_paths=" << changed_paths.size()
               << " matches=" << matches.size()
               << " ref_nodes=" << ref_nodes.size()
               << " live_nodes=" << live_nodes.size();

  // Helper: get first N xpath segments as cluster key (xpath uses '/' separator).
  auto PathPrefix = [](const String& path, int n) -> String {
    int count = 0;
    unsigned pos = 1; // skip leading '/'
    while (pos < path.length()) {
      unsigned next = path.find('/', pos);
      if (next == kNotFound) break;
      count++;
      if (count >= n) return path.substr(0, next);
      pos = next + 1;
    }
    return path;
  };

  // Helper: longest common prefix of two xpaths (segment-aligned).
  auto LCAPath = [](const String& a, const String& b) -> String {
    String lca;
    unsigned pa = 1, pb = 1;
    while (pa < a.length() && pb < b.length()) {
      unsigned na = a.find('/', pa);
      unsigned nb = b.find('/', pb);
      unsigned ea = (na == kNotFound) ? a.length() : na;
      unsigned eb = (nb == kNotFound) ? b.length() : nb;
      String sa = a.substr(pa, ea - pa);
      String sb = b.substr(pb, eb - pb);
      if (sa != sb) break;
      lca = a.substr(0, ea);
      if (na == kNotFound || nb == kNotFound) break;
      pa = na + 1;
      pb = nb + 1;
    }
    return lca;
  };

  double structural_sim, avg_text_sim, avg_layout_sim, avg_style_sim;

  if (changed_paths.empty()) {
    // Perfect match.
    structural_sim = 1.0;
    avg_text_sim = 1.0;
    avg_layout_sim = 1.0;
    avg_style_sim = 1.0;
  } else {
    // Step 2: Cluster changed paths by first-5-segment prefix.
    HashMap<String, Vector<String>> clusters;
    for (const String& p : changed_paths) {
      String key = PathPrefix(p, 5);
      clusters.insert(key, Vector<String>()).stored_value->value.push_back(p);
    }

    // Step 3 & 4: Per-cluster LCA + scoped scoring.
    double w_structural = 0, w_text = 0, w_layout = 0, w_style = 0, total_weight = 0;

    for (auto& entry : clusters) {
      const Vector<String>& paths = entry.value;

      // Compute LCA for this cluster.
      String lca = paths[0];
      for (size_t i = 1; i < paths.size(); ++i)
        lca = LCAPath(lca, paths[static_cast<wtf_size_t>(i)]);
      if (lca.empty()) lca = paths[0];

      // Score only matched pairs within this LCA subtree.
      double c_structural, c_text, c_layout, c_style;
      int scoped_matched = 0, scoped_ref = 0, scoped_live = 0;
      double scoped_text = 0, scoped_layout = 0, scoped_style = 0;

      for (const auto& pair : matches) {
        if (pair.first < 0 || pair.first >= static_cast<int>(ref_nodes.size())) continue;
        if (pair.second < 0 || pair.second >= static_cast<int>(live_nodes.size())) continue;
        auto* r = ref_nodes[pair.first];
        auto* l = live_nodes[pair.second];
        if (!r || !l) continue;
        if (!r->getXpath(String()).starts_with(lca)) continue;

        scoped_matched++;
        scoped_text += ComputeTextSimilarity(r->getTextContent(), l->getTextContent());
        auto* rb = r->getBounds(empty_rect.get());
        auto* lb = l->getBounds(empty_rect.get());
        if (rb && lb) scoped_layout += CalculateIoU(rb, lb);
        scoped_style += ComputeStyleSimilarity(r, l, ref_nodes, live_nodes);
      }

      for (auto* n : ref_nodes)
        if (n && n->getXpath(String()).starts_with(lca)) scoped_ref++;
      for (auto* n : live_nodes)
        if (n && n->getXpath(String()).starts_with(lca)) scoped_live++;

      int denom = std::max(scoped_ref, scoped_live);
      c_structural = denom > 0 ? (double)scoped_matched / denom : 1.0;
      c_text = scoped_matched > 0 ? scoped_text / scoped_matched : 1.0;
      c_layout = scoped_matched > 0 ? scoped_layout / scoped_matched : 1.0;
      c_style = scoped_matched > 0 ? scoped_style / scoped_matched : 1.0;

      double weight = (double)paths.size();
      w_structural += c_structural * weight;
      w_text       += c_text       * weight;
      w_layout     += c_layout     * weight;
      w_style      += c_style      * weight;
      total_weight += weight;
    }

    structural_sim = total_weight > 0 ? w_structural / total_weight : 1.0;
    avg_text_sim   = total_weight > 0 ? w_text       / total_weight : 1.0;
    avg_layout_sim = total_weight > 0 ? w_layout     / total_weight : 1.0;
    avg_style_sim  = total_weight > 0 ? w_style      / total_weight : 1.0;

    LOG(WARNING) << "ChromiumRL LCA: SCOPED scores"
                 << " structural=" << structural_sim
                 << " text=" << avg_text_sim
                 << " layout=" << avg_layout_sim
                 << " style=" << avg_style_sim
                 << " clusters=" << clusters.size()
                 << " total_weight=" << total_weight;
  }

  // ---------------------------------------------------------------------------
  // Delta node scoring — 4 dimensions scoped to delta xpaths only
  // Uses same scoring functions as full comparison but restricted to nodes
  // that differ between buggy and gold.
  // ---------------------------------------------------------------------------
  double delta_score = -1.0;
  double delta_style_score = -1.0;
  double delta_text_score = -1.0;
  double delta_layout_score = -1.0;
  double delta_structural_score = -1.0;
  int delta_nodes_total = 0;
  int delta_nodes_found = 0;

  if (delta_nodes_param && delta_nodes_param->size() > 0) {
    auto& delta_nodes_arr = *delta_nodes_param;
    delta_nodes_total = static_cast<int>(delta_nodes_arr.size());

    // Collect delta xpaths
    HashSet<String> delta_xpaths;
    for (size_t i = 0; i < delta_nodes_arr.size(); ++i) {
      delta_xpaths.insert(delta_nodes_arr[i]->getXpath());
    }

    // Build xpath → live Element for CSS property lookup
    HeapHashMap<String, Member<Element>> xpath_to_live_element;
    for (const auto& pair : matches) {
      if (pair.first < 0 || pair.first >= static_cast<int>(ref_nodes.size())) continue;
      if (pair.second < 0 || pair.second >= static_cast<int>(live_dom_nodes.size())) continue;
      String xp = ref_nodes[pair.first]->getXpath(String());
      Node* node = live_dom_nodes[pair.second];
      if (auto* el = DynamicTo<Element>(node)) {
        xpath_to_live_element.Set(xp, el);
      }
    }

    // --- Style score: per-property CSS comparison ---
    double style_total = 0.0;
    int style_count = 0;
    for (size_t i = 0; i < delta_nodes_arr.size(); ++i) {
      auto* dn = delta_nodes_arr[i].get();
      auto el_it = xpath_to_live_element.find(dn->getXpath());
      if (el_it == xpath_to_live_element.end()) continue;
      auto* computed = MakeGarbageCollected<CSSComputedStyleDeclaration>(el_it->value);
      auto* props = dn->getProperties();
      if (!props || props->empty()) { style_total += 1.0; style_count++; continue; }
      double ns = 0.0;
      int pc = 0;
      for (size_t j = 0; j < props->size(); ++j) {
        CSSPropertyID pid = CssPropertyID(el_it->value->GetExecutionContext(), props->at(j)->getCssProperty());
        if (pid == CSSPropertyID::kInvalid) continue;
        if (computed->GetPropertyValue(pid) == props->at(j)->getGoldValue()) ns += 1.0;
        pc++;
      }
      style_total += pc > 0 ? ns / pc : 1.0;
      style_count++;
    }
    delta_style_score = style_count > 0 ? style_total / style_count : 1.0;

    // --- Text, Layout, Structural scores: from matched pairs on delta xpaths ---
    double text_total = 0.0, layout_total = 0.0;
    int delta_matched = 0, delta_in_ref = 0, delta_in_live = 0;

    for (const auto& pair : matches) {
      if (pair.first < 0 || pair.first >= static_cast<int>(ref_nodes.size())) continue;
      if (pair.second < 0 || pair.second >= static_cast<int>(live_nodes.size())) continue;
      String xp = ref_nodes[pair.first]->getXpath(String());
      if (!delta_xpaths.Contains(xp)) continue;
      auto* r = ref_nodes[pair.first];
      auto* l = live_nodes[pair.second];
      delta_matched++;
      text_total += ComputeTextSimilarity(r->getTextContent(), l->getTextContent());
      auto* rb = r->getBounds(empty_rect.get());
      auto* lb = l->getBounds(empty_rect.get());
      if (rb && lb) layout_total += CalculateIoU(rb, lb);
      else layout_total += 1.0;
    }

    // Count delta xpaths present in ref and live for structural score
    for (auto* n : ref_nodes)
      if (n && delta_xpaths.Contains(n->getXpath(String()))) delta_in_ref++;
    for (auto* n : live_nodes)
      if (n && delta_xpaths.Contains(n->getXpath(String()))) delta_in_live++;

    int delta_denom = std::max(delta_in_ref, delta_in_live);
    delta_structural_score = delta_denom > 0 ? (double)delta_matched / delta_denom : 1.0;
    delta_text_score = delta_matched > 0 ? text_total / delta_matched : 1.0;
    delta_layout_score = delta_matched > 0 ? layout_total / delta_matched : 1.0;

    delta_nodes_found = delta_matched;
    delta_score = (delta_style_score + delta_text_score + delta_layout_score + delta_structural_score) / 4.0;
  }

  auto summary = protocol::ChromiumRL::DiffSummary::create() 
    .setTotalNodesReference(ref_nodes.size())
    .setTotalNodesGenerated(live_nodes.size())
    .setMatchedNodes(matches.size())
    .setUnmatchedReference(static_cast<int>(deletions->size()))
    .setUnmatchedGenerated(static_cast<int>(insertions->size()))
    .setStructuralSimilarity(structural_sim)
    .setTextSimilarity(avg_text_sim)
    .setLayoutSimilarity(avg_layout_sim)
    .setStyleSimilarity(avg_style_sim)
    .build();

  if (delta_score >= 0.0) {
    summary->setDeltaScore(delta_score);
    summary->setDeltaStyleScore(delta_style_score);
    summary->setDeltaTextScore(delta_text_score);
    summary->setDeltaLayoutScore(delta_layout_score);
    summary->setDeltaStructuralScore(delta_structural_score);
    summary->setDeltaNodesTotal(delta_nodes_total);
    summary->setDeltaNodesFound(delta_nodes_found);
  }

  *out_result = protocol::ChromiumRL::DOMDiffResult::create() 
    .setSummary(std::move(summary))
    .setInsertions(std::move(insertions))
    .setDeletions(std::move(deletions))
    .setMoves(std::move(moves))
    .setAttributeChanges(std::move(attr_changes))
    .setTextChanges(std::move(text_changes))
    .setLayoutChanges(std::move(layout_changes))
    .setStyleChanges(std::move(style_changes))
    .setTypeChanges(std::move(type_changes))
    .build();

  return protocol::Response::Success();
}

String InspectorChromiumRLAgent::GetAriaRoleName(Element* element) {
  // Check explicit ARIA role first
  const AtomicString& aria_role = element->FastGetAttribute(html_names::kRoleAttr);
  if (!aria_role.IsNull()) {
    ax::mojom::blink::Role role = AriaRoleToInternalRole(aria_role);
    if (role != ax::mojom::blink::Role::kUnknown) {
      const AtomicString& name = InternalRoleToAriaRole(role);
      if (!name.empty()) return name;
    }
    return aria_role;
  }

  // Native role from element type
  ax::mojom::blink::Role role = ax::mojom::blink::Role::kGenericContainer;

  if (auto* input = DynamicTo<HTMLInputElement>(element)) {
    const AtomicString& type = input->FastGetAttribute(html_names::kTypeAttr);
    if (type.IsNull() || type == "text" || type == "search" || type == "email" ||
        type == "url" || type == "tel" || type == "password" || type == "number")
      role = ax::mojom::blink::Role::kTextField;
    else if (type == "submit" || type == "button" || type == "reset")
      role = ax::mojom::blink::Role::kButton;
    else if (type == "checkbox")
      role = ax::mojom::blink::Role::kCheckBox;
    else if (type == "radio")
      role = ax::mojom::blink::Role::kRadioButton;
    else if (type == "range")
      role = ax::mojom::blink::Role::kSlider;
    else
      role = ax::mojom::blink::Role::kTextField;
  } else if (IsA<HTMLSelectElement>(element)) {
    role = ax::mojom::blink::Role::kComboBoxSelect;
  } else if (IsA<HTMLTextAreaElement>(element)) {
    role = ax::mojom::blink::Role::kTextField;
  } else if (element->HasTagName(html_names::kButtonTag)) {
    role = ax::mojom::blink::Role::kButton;
  } else if (element->HasTagName(html_names::kATag)) {
    role = ax::mojom::blink::Role::kLink;
  } else {
    if (auto* html_el = DynamicTo<HTMLElement>(element)) {
      if (html_el->contentEditableNormalized() == ContentEditableType::kContentEditable)
        role = ax::mojom::blink::Role::kTextField;
    }
  }

  const AtomicString& name = InternalRoleToAriaRole(role);
  return name.empty() ? element->tagName().ToAsciiLower() : String(name);
}

bool InspectorChromiumRLAgent::IsInteractiveElement(Element* element) {
  String tag = element->tagName().ToAsciiLower();
  if (tag == "html" || tag == "body")
    return false;

  if (element->IsFormControlElement()) return true;
  if (element->HasTagName(html_names::kATag)) return true;
  if (element->HasTagName(html_names::kButtonTag)) return true;
  if (element->FastHasAttribute(html_names::kTabindexAttr)) return true;
  if (element->FastHasAttribute(html_names::kOnclickAttr)) return true;

  if (auto* html_el = DynamicTo<HTMLElement>(element)) {
    if (html_el->contentEditableNormalized() == ContentEditableType::kContentEditable)
      return true;
  }

  const AtomicString& role = element->FastGetAttribute(html_names::kRoleAttr);
  if (!role.IsNull()) {
    if (role == "application" &&
        (tag == "div" || tag == "main" || tag == "section"))
      return false;
    if (role == "button" || role == "link" || role == "tab" ||
        role == "menuitem" || role == "option" || role == "switch" ||
        role == "checkbox" || role == "radio" || role == "combobox" ||
        role == "searchbox" || role == "slider" || role == "spinbutton" ||
        role == "textbox")
      return true;
  }

  return false;
}

bool InspectorChromiumRLAgent::IsContentElement(Element* element) {
  return element->HasTagName(html_names::kH1Tag) ||
         element->HasTagName(html_names::kH2Tag) ||
         element->HasTagName(html_names::kH3Tag) ||
         element->HasTagName(html_names::kH4Tag) ||
         element->HasTagName(html_names::kH5Tag) ||
         element->HasTagName(html_names::kH6Tag) ||
         element->HasTagName(html_names::kPTag) ||
         element->HasTagName(html_names::kUlTag) ||
         element->HasTagName(html_names::kOlTag) ||
         element->HasTagName(html_names::kTableTag) ||
         element->HasTagName(html_names::kBlockquoteTag) ||
         element->HasTagName(html_names::kPreTag);
}

bool InspectorChromiumRLAgent::IsSemanticBoundary(Element* element) {
  if (element->HasTagName(html_names::kLiTag) ||
      element->HasTagName(html_names::kTrTag) ||
      element->HasTagName(html_names::kTdTag) ||
      element->HasTagName(html_names::kThTag) ||
      element->HasTagName(html_names::kArticleTag) ||
      element->HasTagName(html_names::kSectionTag) ||
      element->HasTagName(html_names::kNavTag) ||
      element->HasTagName(html_names::kHeaderTag) ||
      element->HasTagName(html_names::kFooterTag) ||
      element->HasTagName(html_names::kFormTag) ||
      element->HasTagName(html_names::kFieldsetTag) ||
      element->HasTagName(html_names::kDetailsTag) ||
      element->HasTagName(html_names::kMainTag) ||
      element->HasTagName(html_names::kAsideTag))
    return true;

  if (element->FastHasAttribute(html_names::kRoleAttr)) return true;
  if (element->HasID()) return true;

  // Div with class and multiple children = likely a container
  if (element->HasTagName(html_names::kDivTag) && element->HasClass()) {
    int child_count = 0;
    for (Element* c = ElementTraversal::FirstChild(*element); c;
         c = ElementTraversal::NextSibling(*c)) {
      if (++child_count >= 2) return true;
    }
  }

  return false;
}

bool InspectorChromiumRLAgent::IsElementInViewport(const gfx::RectF& box,
                                                   double scroll_top,
                                                   double viewport_width,
                                                   double viewport_height) {
  if (box.width() <= 0 || box.height() <= 0)
    return false;

  return box.right() >= -100 && box.x() <= viewport_width + 100 &&
         box.bottom() >= scroll_top - 100 &&
         box.y() <= scroll_top + viewport_height + 100;
}

bool InspectorChromiumRLAgent::IsElementHitTestable(Element* element,
                                                    const gfx::RectF& box) {
  LocalFrame* frame = element ? element->GetDocument().GetFrame() : nullptr;
  if (!frame || !frame->View() || box.width() <= 0 || box.height() <= 0)
    return false;

  gfx::PointF center = box.CenterPoint();
  HitTestLocation location(
      frame->View()->ConvertFromRootFrame(gfx::ToFlooredPoint(center)));
  HitTestResult result = frame->GetEventHandler().HitTestResultAtLocation(
      location, HitTestRequest::kReadOnly | HitTestRequest::kActive);
  result.SetToShadowHostIfInUAShadowRoot();
  Node* hit_node = result.InnerNode();
  return hit_node &&
         (hit_node == element || hit_node->IsDescendantOf(element));
}

String InspectorChromiumRLAgent::GetAccessibleName(Node* node) {
  if (!node)
    return String();

  Document& document = node->GetDocument();
  if (AXObjectCache* cache = document.ExistingAXObjectCache()) {
    String computed_name = NormalizeTextContent(cache->ComputedNameForNode(node));
    if (!computed_name.empty())
      return computed_name;
  }

  if (auto* element = DynamicTo<Element>(node)) {
    const AtomicString& aria_label =
        element->FastGetAttribute(html_names::kAriaLabelAttr);
    if (!aria_label.IsNull() && !aria_label.empty())
      return NormalizeTextContent(aria_label);

    const AtomicString& title = element->FastGetAttribute(html_names::kTitleAttr);
    if (!title.IsNull() && !title.empty())
      return NormalizeTextContent(title);

    if (element->HasTagName(html_names::kImgTag)) {
      const AtomicString& alt = element->FastGetAttribute(html_names::kAltAttr);
      if (!alt.IsNull() && !alt.empty())
        return NormalizeTextContent(alt);
    }

    for (Element& descendant : ElementTraversal::DescendantsOf(*element)) {
      if (descendant.HasTagName(html_names::kImgTag)) {
        const AtomicString& alt =
            descendant.FastGetAttribute(html_names::kAltAttr);
        if (!alt.IsNull() && !alt.empty())
          return NormalizeTextContent(alt);
      }

      const AtomicString& descendant_aria_label =
          descendant.FastGetAttribute(html_names::kAriaLabelAttr);
      if (!descendant_aria_label.IsNull() &&
          !descendant_aria_label.empty()) {
        return NormalizeTextContent(descendant_aria_label);
      }
    }
  }

  return String();
}

String InspectorChromiumRLAgent::BuildObservationDedupKey(Element* element) {
  String label = NormalizeTextContent(element->textContent()).substr(0, 120).ToAsciiLower();
  String accessible_name = GetAccessibleName(element).ToAsciiLower();
  if (label.empty()) label = accessible_name;

  if (element->HasTagName(html_names::kATag)) {
    String href = element->FastGetAttribute(html_names::kHrefAttr).ToAsciiLower();
    if (!href.empty()) {
      return "href:" + href + "|label:" + label;
    }
  }

  String role = GetAriaRoleName(element).ToAsciiLower();
  String selector = BuildSelectorPath(element);
  if (!role.empty() && !label.empty()) {
    return "role:" + role + "|label:" + label + "|selector:" + selector;
  }
  if (!label.empty()) {
    return "label:" + label + "|tag:" + element->tagName().ToAsciiLower() + "|selector:" + selector;
  }
  return "selector:" + selector;
}

bool InspectorChromiumRLAgent::HasHumanReadableLabel(Element* element) {
  if (!NormalizeTextContent(element->textContent()).empty())
    return true;
  if (!GetAccessibleName(element).empty())
    return true;

  const AtomicString& aria_label =
      element->FastGetAttribute(html_names::kAriaLabelAttr);
  if (!aria_label.IsNull() && !aria_label.empty())
    return true;
  const AtomicString& title = element->FastGetAttribute(html_names::kTitleAttr);
  if (!title.IsNull() && !title.empty())
    return true;
  const AtomicString& placeholder =
      element->FastGetAttribute(html_names::kPlaceholderAttr);
  if (!placeholder.IsNull() && !placeholder.empty())
    return true;
  const AtomicString& value = element->FastGetAttribute(html_names::kValueAttr);
  if (!value.IsNull() && !value.empty())
    return true;
  const AtomicString& name = element->FastGetAttribute(html_names::kNameAttr);
  if (!name.IsNull() && !name.empty())
    return true;

  for (Element* child = ElementTraversal::FirstChild(*element); child;
       child = ElementTraversal::NextSibling(*child)) {
    if (child->HasTagName(html_names::kImgTag)) {
      const AtomicString& alt = child->FastGetAttribute(html_names::kAltAttr);
      if (!alt.IsNull() && !alt.empty())
        return true;
    }
  }

  return false;
}

bool InspectorChromiumRLAgent::IsJunkContentText(const String& text) {
  String normalized = NormalizeTextContent(text);
  if (normalized.length() < 3)
    return true;
  if (normalized.length() > 1200)
    return true;

  unsigned braces = 0;
  unsigned css_punctuation = 0;
  for (unsigned i = 0; i < normalized.length(); ++i) {
    UChar c = normalized[i];
    if (c == '{' || c == '}' || c == '[' || c == ']')
      braces++;
    if (c == ';' || c == ':' || c == '#' || c == '.' || c == '>')
      css_punctuation++;
  }

  double len = static_cast<double>(normalized.length());
  if (braces / len > 0.08)
    return true;
  if (css_punctuation / len > 0.20)
    return true;
  std::string normalized_utf8 = normalized.Utf8();
  if (normalized_utf8.find("{\"") != std::string::npos ||
      normalized_utf8.find("function(") != std::string::npos ||
      normalized_utf8.find("webpack") != std::string::npos ||
      normalized_utf8.find("__NEXT_DATA__") != std::string::npos)
    return true;

  return false;
}

double InspectorChromiumRLAgent::ScoreInteractiveElement(Element* element,
                                                        bool is_in_viewport,
                                                        bool is_hit_testable) {
  double score = 0.0;
  if (is_in_viewport) score += 100.0;
  if (is_hit_testable) score += 80.0;
  if (HasHumanReadableLabel(element)) score += 35.0;

  String role = GetAriaRoleName(element).ToAsciiLower();
  if (element->IsFormControlElement()) score += 35.0;
  if (element->HasTagName(html_names::kInputTag) ||
      element->HasTagName(html_names::kTextareaTag) ||
      role == "searchbox" || role == "textbox") {
    score += 45.0;
  }
  if (element->HasTagName(html_names::kButtonTag) || role == "button")
    score += 30.0;
  if (element->HasTagName(html_names::kATag) || role == "link")
    score += 18.0;

  const AtomicString& href = element->FastGetAttribute(html_names::kHrefAttr);
  if (!href.IsNull() && !href.empty()) score += 8.0;

  gfx::RectF box = element->GetLayoutObject()
                       ? element->GetLayoutObject()->AbsoluteBoundingBoxRectF()
                       : gfx::RectF();
  if (box.width() < 8 || box.height() < 8)
    score -= 35.0;
  if (element->HasTagName(html_names::kFooterTag) ||
      element->HasTagName(html_names::kNavTag))
    score -= 12.0;

  return score;
}

double InspectorChromiumRLAgent::ScoreContentElement(Element* element,
                                                    bool is_in_viewport,
                                                    const String& text) {
  double score = is_in_viewport ? 80.0 : 0.0;
  if (element->HasTagName(html_names::kH1Tag)) score += 60.0;
  else if (element->HasTagName(html_names::kH2Tag)) score += 50.0;
  else if (element->HasTagName(html_names::kH3Tag)) score += 42.0;
  else if (element->HasTagName(html_names::kPTag)) score += 30.0;
  else if (element->HasTagName(html_names::kTableTag)) score += 28.0;
  else if (element->HasTagName(html_names::kUlTag) ||
           element->HasTagName(html_names::kOlTag)) score += 22.0;

  if (text.length() >= 40) score += 20.0;
  if (text.length() > 500) score -= 20.0;
  if (element->HasTagName(html_names::kFooterTag) ||
      element->HasTagName(html_names::kNavTag) ||
      element->HasTagName(html_names::kAsideTag))
    score -= 25.0;
  return score;
}

String InspectorChromiumRLAgent::GetElementContext(Element* element) {
  Node* container = element->parentNode();
  int depth = 0;

  while (container && depth < 5) {
    if (auto* el = DynamicTo<Element>(container)) {
      if (IsSemanticBoundary(el)) break;
    }
    if (container->IsDocumentNode()) break;
    container = container->parentNode();
    depth++;
  }

  if (!container || container->IsDocumentNode()) {
    container = element->parentNode();
  }
  if (auto* el = DynamicTo<Element>(container)) {
    if (el->HasTagName(html_names::kBodyTag))
      container = element->parentNode();
  }
  if (!container) return String();

  String element_text = element->textContent().StripWhiteSpace();
  StringBuilder builder;

  for (Node& desc : NodeTraversal::DescendantsOf(*container)) {
    if (!desc.IsTextNode()) continue;
    String text = NormalizeTextContent(desc.nodeValue());
    if (text.empty() || text.length() <= 1) continue;
    if (text == element_text) continue;
    if (builder.length() > 0) builder.Append(" | ");
    builder.Append(text);
    if (builder.length() > 200) break;
  }

  String result = builder.ToString();
  if (result.length() > 200) result = result.substr(0, 200);
  return result;
}

std::unique_ptr<protocol::ChromiumRL::ObservedElement>
InspectorChromiumRLAgent::BuildObservedElement(Element* element,
                                               int idx,
                                               const gfx::RectF& box,
                                               bool is_in_viewport,
                                               bool is_hit_testable) {
  int node_id = IdentifiersFactory::IntIdForNode(element);
  String tag_name = element->tagName().ToAsciiLower();
  String computed_role = GetAriaRoleName(element);
  String fingerprint = GetNodeFingerprint(element);
  String accessible_name = GetAccessibleName(element);
  String text = element->textContent().StripWhiteSpace();
  if (text.length() > 80) text = text.substr(0, 80);

  // Fall back to accessible labeling when visible text is absent.
  if (text.empty()) {
    if (!accessible_name.empty()) {
      text = accessible_name;
    } else if (element->HasTagName(html_names::kATag) ||
               element->HasTagName(html_names::kButtonTag)) {
      for (Element* child = ElementTraversal::FirstChild(*element); child;
           child = ElementTraversal::NextSibling(*child)) {
        if (child->HasTagName(html_names::kImgTag)) {
          const AtomicString& alt = child->FastGetAttribute(html_names::kAltAttr);
          if (!alt.IsNull() && !alt.empty()) { text = alt; break; }
        }
      }
    }
  }

  auto obs = protocol::ChromiumRL::ObservedElement::create()
    .setIdx(idx)
    .setNodeId(node_id)
    .setTag(tag_name)
    .setSelector(BuildSelectorPath(element))
    .setBounds(protocol::DOM::Rect::create()
        .setX(box.x())
        .setY(box.y())
        .setWidth(box.width())
        .setHeight(box.height())
        .build())
    .setCenterX(box.CenterPoint().x())
    .setCenterY(box.CenterPoint().y())
    .setIsVisible(true)
    .setIsInViewport(is_in_viewport)
    .setIsHitTestable(is_hit_testable)
    .build();

  if (!fingerprint.empty()) obs->setFingerprint(fingerprint);
  if (!computed_role.empty()) obs->setRole(computed_role);
  if (!accessible_name.empty()) obs->setAccessibleName(accessible_name);
  if (!text.empty()) obs->setText(text);

  // Context
  String context = GetElementContext(element);
  if (!context.empty()) obs->setContext(context);

  // Aria states
  const AtomicString& expanded = element->FastGetAttribute(html_names::kAriaExpandedAttr);
  if (!expanded.IsNull()) obs->setExpanded(expanded == "true");
  const AtomicString& selected = element->FastGetAttribute(html_names::kAriaSelectedAttr);
  if (!selected.IsNull()) obs->setSelected(selected == "true");

  // Disabled
  if (element->FastHasAttribute(html_names::kDisabledAttr))
    obs->setDisabled(true);
  if (element->FastHasAttribute(html_names::kRequiredAttr))
    obs->setRequired(true);
  if (element->FastHasAttribute(html_names::kReadonlyAttr))
    obs->setReadonly(true);
  const AtomicString& autocomplete =
      element->FastGetAttribute(html_names::kAutocompleteAttr);
  if (!autocomplete.IsNull() && !autocomplete.empty())
    obs->setAutocomplete(autocomplete);

  // Link href
  if (element->HasTagName(html_names::kATag)) {
    String href = element->FastGetAttribute(html_names::kHrefAttr);
    if (!href.empty()) {
      if (href.length() > 200) href = href.substr(0, 200);
      obs->setHref(href);
    }
  }

  // HTMLInputElement — Fix #6: only set type for textbox/searchbox when meaningful
  if (auto* input = DynamicTo<HTMLInputElement>(element)) {
    const AtomicString& input_type = input->FastGetAttribute(html_names::kTypeAttr);
    if (computed_role == "textbox" || computed_role == "searchbox") {
      if (!input_type.IsNull() && input_type != "text")
        obs->setType(input_type);
    }
    obs->setValue(input->Value());
    const AtomicString& ph = input->FastGetAttribute(html_names::kPlaceholderAttr);
    if (!ph.IsNull()) obs->setPlaceholder(ph);
    if (input_type == "checkbox" || input_type == "radio")
      obs->setChecked(input->Checked());
    if (input->willValidate() && !input->validationMessage().empty())
      obs->setInvalid(true);
  }

  // HTMLSelectElement — Fix #4: use selected option text, not raw textContent
  if (auto* select = DynamicTo<HTMLSelectElement>(element)) {
    obs->setValue(select->Value());
    // Set text to selected option's display text
    for (auto& option : select->GetOptionList()) {
      if (option.Selected()) {
        obs->setText(option.text().StripWhiteSpace());
        break;
      }
    }
    auto options = std::make_unique<protocol::Array<String>>();
    for (auto& option : select->GetOptionList()) {
      String opt_text = option.text().StripWhiteSpace();
      if (!opt_text.empty()) options->push_back(opt_text);
      if (options->size() >= 10) break;
    }
    obs->setOptions(std::move(options));
    if (select->willValidate() && !select->validationMessage().empty())
      obs->setInvalid(true);
  }

  // HTMLTextAreaElement
  if (auto* textarea = DynamicTo<HTMLTextAreaElement>(element)) {
    obs->setValue(textarea->Value());
    const AtomicString& ph = textarea->FastGetAttribute(html_names::kPlaceholderAttr);
    if (!ph.IsNull()) obs->setPlaceholder(ph);
    if (textarea->willValidate() && !textarea->validationMessage().empty())
      obs->setInvalid(true);
  }

  return obs;
}

std::unique_ptr<protocol::ChromiumRL::TableInfo>
InspectorChromiumRLAgent::BuildTableInfo(Element* element,
                                         String* table_text) {
  constexpr int kMaxTableRows = 20;
  constexpr int kMaxTableColumns = 10;
  constexpr unsigned kMaxTableCellChars = 80;
  constexpr unsigned kMaxTableTextChars = 4000;

  int row_count = 0;
  int column_count = 0;
  int shown_row_count = 0;
  int shown_column_count = 0;
  bool structure_truncated = false;
  bool cell_text_truncated = false;
  bool table_text_truncated = false;

  auto rows = std::make_unique<protocol::Array<protocol::ChromiumRL::TableRow>>();
  StringBuilder body_text;

  for (Element& row : ElementTraversal::DescendantsOf(*element)) {
    if (!row.HasTagName(html_names::kTrTag))
      continue;

    auto cells = std::make_unique<protocol::Array<String>>();
    StringBuilder row_text;
    int row_column_count = 0;
    int shown_row_column_count = 0;

    for (Element* cell = ElementTraversal::FirstChild(row); cell;
         cell = ElementTraversal::NextSibling(*cell)) {
      if (!cell->HasTagName(html_names::kTdTag) &&
          !cell->HasTagName(html_names::kThTag)) {
        continue;
      }

      String cell_text = NormalizeTextContent(cell->textContent());
      if (cell_text.length() > kMaxTableCellChars) {
        cell_text = cell_text.substr(0, kMaxTableCellChars);
        cell_text_truncated = true;
      }

      if (row_column_count < kMaxTableColumns && row_count < kMaxTableRows) {
        cells->push_back(cell_text);
        if (row_text.length() > 0)
          row_text.Append(" | ");
        row_text.Append(cell_text);
        shown_row_column_count++;
      } else {
        structure_truncated = true;
      }

      row_column_count++;
    }

    column_count = std::max(column_count, row_column_count);

    if (row_count < kMaxTableRows) {
      shown_column_count = std::max(shown_column_count, shown_row_column_count);
      rows->push_back(protocol::ChromiumRL::TableRow::create()
                          .setCells(std::move(cells))
                          .build());
      if (row_text.length() > 0) {
        if (body_text.length() > 0)
          body_text.Append("\n");
        body_text.Append(row_text.ToString());
      }
      shown_row_count++;
    } else {
      structure_truncated = true;
    }

    row_count++;
  }

  if (row_count == 0) {
    if (table_text)
      *table_text = String();
    return protocol::ChromiumRL::TableInfo::create()
        .setRowCount(0)
        .setColumnCount(0)
        .setShownRowCount(0)
        .setShownColumnCount(0)
        .setShownRowStart(0)
        .setShownRowEnd(-1)
        .setShownColumnStart(0)
        .setShownColumnEnd(-1)
        .setTruncated(false)
        .setCellTextTruncated(false)
        .setTableTextTruncated(false)
        .setRows(std::move(rows))
        .build();
  }

  String body_text_value = body_text.ToString();
  if (body_text_value.length() > kMaxTableTextChars) {
    body_text_value = body_text_value.substr(0, kMaxTableTextChars);
    table_text_truncated = true;
  }

  if (table_text) {
    StringBuilder summary;
    summary.Append("[table rows=");
    summary.AppendNumber(row_count);
    summary.Append(" columns=");
    summary.AppendNumber(column_count);
    summary.Append(" shown_rows=");
    summary.AppendNumber(shown_row_count);
    summary.Append(" shown_columns=");
    summary.AppendNumber(shown_column_count);
    summary.Append(" truncated=");
    summary.Append(structure_truncated ? "true" : "false");
    summary.Append(" cell_text_truncated=");
    summary.Append(cell_text_truncated ? "true" : "false");
    summary.Append(" table_text_truncated=");
    summary.Append(table_text_truncated ? "true" : "false");
    summary.Append("]\n");
    summary.Append(body_text_value);
    *table_text = summary.ToString();
  }

  return protocol::ChromiumRL::TableInfo::create()
      .setRowCount(row_count)
      .setColumnCount(column_count)
      .setShownRowCount(shown_row_count)
      .setShownColumnCount(shown_column_count)
      .setShownRowStart(0)
      .setShownRowEnd(shown_row_count - 1)
      .setShownColumnStart(0)
      .setShownColumnEnd(shown_column_count - 1)
      .setTruncated(structure_truncated)
      .setCellTextTruncated(cell_text_truncated)
      .setTableTextTruncated(table_text_truncated)
      .setRows(std::move(rows))
      .build();
}

std::unique_ptr<protocol::ChromiumRL::ContentBlock>
InspectorChromiumRLAgent::BuildContentBlock(Element* element,
                                            const String& precomputed_text) {
  String text;
  std::unique_ptr<protocol::ChromiumRL::TableInfo> table_info;

  // For lists, aggregate li children
  if (element->HasTagName(html_names::kTableTag)) {
    table_info = BuildTableInfo(element, &text);
  } else if (element->HasTagName(html_names::kUlTag) || element->HasTagName(html_names::kOlTag)) {
    StringBuilder sb;
    for (Element* li = ElementTraversal::FirstChild(*element); li;
         li = ElementTraversal::NextSibling(*li)) {
      if (li->HasTagName(html_names::kLiTag)) {
        String item = li->textContent().StripWhiteSpace();
        if (!item.empty()) {
          if (sb.length() > 0) sb.Append(" | ");
          sb.Append(item.substr(0, 100));
        }
      }
    }
    text = sb.ToString();
  } else {
    text = precomputed_text;
  }

  if (text.empty() || (!table_info && IsJunkContentText(text))) return nullptr;
  if (!table_info && text.length() > 500) text = text.substr(0, 500);

  auto block = protocol::ChromiumRL::ContentBlock::create()
    .setNodeId(IdentifiersFactory::IntIdForNode(element))
    .setFingerprint(GetNodeFingerprint(element))
    .setTag(element->tagName().ToAsciiLower())
    .setSelector(BuildSelectorPath(element))
    .setText(text)
    .build();
  if (table_info)
    block->setTable(std::move(table_info));
  return block;
}

bool InspectorChromiumRLAgent::IsDecorativeOrUnsupported(Element* element) {
  if (!element)
    return true;

  String tag = element->tagName().ToAsciiLower();
  if (tag == "script" || tag == "style" || tag == "noscript" ||
      tag == "template" || tag == "head" || tag == "meta" ||
      tag == "link" || tag == "title") {
    return true;
  }

  if (tag == "path" || tag == "rect" || tag == "g" || tag == "circle" ||
      tag == "ellipse" || tag == "line" || tag == "polyline" ||
      tag == "polygon" || tag == "use" || tag == "defs" ||
      tag == "clippath" || tag == "mask" || tag == "pattern" ||
      tag == "tspan") {
    Node* parent = element->parentNode();
    while (parent) {
      if (auto* parent_element = DynamicTo<Element>(parent)) {
        if (IsInteractiveElement(parent_element) ||
            !GetAccessibleName(parent_element).empty()) {
          return true;
        }
      }
      parent = parent->parentNode();
    }
    return true;
  }

  return false;
}

bool InspectorChromiumRLAgent::IsScrollableElement(Element* element) {
  if (!element)
    return false;
  const ComputedStyle* style = element->GetComputedStyle();
  return style && style->ScrollsOverflow();
}

String InspectorChromiumRLAgent::ComputeDirectText(Node* node,
                                                   unsigned max_chars,
                                                   bool* truncated) {
  if (!node)
    return String();

  StringBuilder builder;
  for (Node* child = node->firstChild(); child; child = child->nextSibling()) {
    if (!child->IsTextNode())
      continue;
    String text = NormalizeTextContent(child->nodeValue());
    if (text.empty())
      continue;
    if (builder.length() > 0)
      builder.Append(" ");
    builder.Append(text);
    if (builder.length() > max_chars)
      break;
  }

  return TruncateStructuredString(builder.ToString(), max_chars, truncated);
}

String InspectorChromiumRLAgent::ComputeSubtreeTextCapped(Node* node,
                                                          unsigned max_chars,
                                                          bool* truncated) {
  if (!node)
    return String();

  StringBuilder builder;
  for (Node& desc : NodeTraversal::DescendantsOf(*node)) {
    if (!desc.IsTextNode())
      continue;
    if (desc.parentNode() && desc.parentNode()->IsElementNode()) {
      Element* parent = To<Element>(desc.parentNode());
      if (parent->HasTagName(html_names::kScriptTag) ||
          parent->HasTagName(html_names::kStyleTag)) {
        continue;
      }
    }
    String text = NormalizeTextContent(desc.nodeValue());
    if (text.empty())
      continue;
    if (builder.length() > 0)
      builder.Append(" ");
    builder.Append(text);
    if (builder.length() > max_chars) {
      if (truncated)
        *truncated = true;
      break;
    }
  }

  return TruncateStructuredString(builder.ToString(), max_chars, truncated);
}

String InspectorChromiumRLAgent::DetectSemanticBoundary(Element* element) {
  if (!element)
    return String();

  String tag = element->tagName().ToAsciiLower();
  String role = GetAriaRoleName(element).ToAsciiLower();
  if (tag == "li" || role == "listitem")
    return "listitem";
  if (tag == "tr" || role == "row")
    return "row";
  if (tag == "td" || tag == "th" || role == "cell" || role == "gridcell")
    return "cell";
  if (tag == "article" || role == "article")
    return "article";
  if (tag == "section")
    return "section";
  if (tag == "main" || role == "main")
    return "landmark";
  if (IsScrollableElement(element))
    return "scrollable";
  return String();
}

std::unique_ptr<protocol::Array<protocol::ChromiumRL::NodeAttribute>>
InspectorChromiumRLAgent::CollectSelectedAttributes(Element* element) {
  auto attrs =
      std::make_unique<protocol::Array<protocol::ChromiumRL::NodeAttribute>>();
  if (!element)
    return attrs;

  for (const auto& attr : element->Attributes()) {
    String name = attr.GetName().ToString().ToAsciiLower();
    String value = attr.Value();
    bool keep = false;

    if (name == "role" || name == "href" || name == "src" ||
        name == "alt" || name == "title" || name == "name" ||
        name == "type" || name == "placeholder" || name == "datetime") {
      keep = true;
    } else if (name == "value") {
      const AtomicString& type =
          element->FastGetAttribute(html_names::kTypeAttr);
      keep = type != "password";
    } else if (name.starts_with("aria-")) {
      keep = true;
    }

    if (!keep || LooksLikeLargeStructuredValue(value))
      continue;

    bool truncated = false;
    value = TruncateStructuredString(value, kStructuredMaxAttributeValueChars,
                                     &truncated);
    attrs->push_back(protocol::ChromiumRL::NodeAttribute::create()
                         .setName(name)
                         .setValue(value)
                         .build());
    if (attrs->size() >= kStructuredMaxAttributes)
      break;
  }

  return attrs;
}

std::unique_ptr<protocol::Array<protocol::ChromiumRL::NodeState>>
InspectorChromiumRLAgent::CollectNodeStates(Element* element) {
  auto states =
      std::make_unique<protocol::Array<protocol::ChromiumRL::NodeState>>();
  if (!element)
    return states;

  auto add_state = [&states](const String& name, const String& value) {
    states->push_back(protocol::ChromiumRL::NodeState::create()
                          .setName(name)
                          .setValue(value)
                          .build());
  };

  const AtomicString& expanded =
      element->FastGetAttribute(html_names::kAriaExpandedAttr);
  if (!expanded.IsNull())
    add_state("expanded", expanded == "true" ? "true" : "false");
  const AtomicString& selected =
      element->FastGetAttribute(html_names::kAriaSelectedAttr);
  if (!selected.IsNull())
    add_state("selected", selected == "true" ? "true" : "false");

  if (element->FastHasAttribute(html_names::kDisabledAttr))
    add_state("disabled", "true");
  if (element->FastHasAttribute(html_names::kRequiredAttr))
    add_state("required", "true");
  if (element->FastHasAttribute(html_names::kReadonlyAttr))
    add_state("readonly", "true");

  if (auto* html_el = DynamicTo<HTMLElement>(element)) {
    if (html_el->contentEditableNormalized() ==
        ContentEditableType::kContentEditable) {
      add_state("editable", "true");
    }
  }

  if (auto* input = DynamicTo<HTMLInputElement>(element)) {
    const AtomicString& input_type =
        input->FastGetAttribute(html_names::kTypeAttr);
    if (input_type == "checkbox" || input_type == "radio")
      add_state("checked", input->Checked() ? "true" : "false");
  }

  return states;
}

std::unique_ptr<protocol::Array<String>>
InspectorChromiumRLAgent::CollectActionTypes(Element* element,
                                             bool scrollable) {
  auto actions = std::make_unique<protocol::Array<String>>();
  if (!element)
    return actions;

  String tag = element->tagName().ToAsciiLower();
  String role = GetAriaRoleName(element).ToAsciiLower();
  Vector<String> added_actions;
  auto add_action = [&actions, &added_actions](const String& action) {
    for (const String& existing : added_actions) {
      if (existing == action)
        return;
    }
    added_actions.push_back(action);
    actions->push_back(action);
  };

  if (tag == "html" || tag == "body")
    return actions;

  bool is_plain_image = tag == "img" && !IsInteractiveElement(element) &&
                        role != "button" && role != "link";
  bool is_application_wrapper =
      role == "application" &&
      (tag == "div" || tag == "main" || tag == "section");
  if (is_plain_image || is_application_wrapper) {
    return actions;
  }

  if (IsInteractiveElement(element) || tag == "button" || tag == "a" ||
      role == "button" || role == "link" || role == "menuitem" ||
      role == "option") {
    add_action("click");
  }

  if (tag == "input" || tag == "textarea" || role == "textbox" ||
      role == "searchbox") {
    add_action("type");
    add_action("focus");
  }
  if (tag == "select" || role == "combobox" || role == "listbox")
    add_action("select");
  if (role == "checkbox" || role == "radio" || role == "switch")
    add_action("toggle");

  if (auto* html_el = DynamicTo<HTMLElement>(element)) {
    if (html_el->contentEditableNormalized() ==
        ContentEditableType::kContentEditable) {
      add_action("type");
      add_action("focus");
    }
  }

  if (auto* input = DynamicTo<HTMLInputElement>(element)) {
    const AtomicString& input_type =
        input->FastGetAttribute(html_names::kTypeAttr);
    if (input_type == "file")
      add_action("upload");
  }

  if (scrollable)
    add_action("scroll");

  return actions;
}

bool InspectorChromiumRLAgent::IsStructuredSnapshotCandidate(
    Node* node,
    bool is_visible,
    bool is_in_viewport,
    bool include_offscreen) {
  if (!node)
    return false;

  if (node->IsDocumentNode())
    return true;

  if (node->IsTextNode()) {
    String text = NormalizeTextContent(node->nodeValue());
    return is_visible && text.length() > 1 && (include_offscreen || is_in_viewport);
  }

  auto* element = DynamicTo<Element>(node);
  if (!element || IsDecorativeOrUnsupported(element))
    return false;

  const AtomicString& aria_hidden =
      element->FastGetAttribute(html_names::kAriaHiddenAttr);
  if (aria_hidden == "true")
    return false;

  String tag = element->tagName().ToAsciiLower();
  String role = GetAriaRoleName(element).ToAsciiLower();
  bool meaningful_container =
      tag == "html" || tag == "body" || tag == "main" || tag == "article" ||
      tag == "section" || tag == "nav" || tag == "header" ||
      tag == "footer" || tag == "form" || tag == "ul" || tag == "ol" ||
      tag == "li" || tag == "table" || tag == "tr" || tag == "td" ||
      tag == "th" || tag == "time" || tag == "label" || tag == "img" ||
      role == "main" || role == "article" || role == "list" ||
      role == "listitem" || role == "row" || role == "cell" ||
      role == "gridcell";

  bool has_text = !NormalizeTextContent(element->textContent()).empty() ||
                  !GetAccessibleName(element).empty();
  bool candidate = meaningful_container || IsInteractiveElement(element) ||
                   IsScrollableElement(element) || has_text;

  return candidate && is_visible && (include_offscreen || is_in_viewport);
}

std::unique_ptr<protocol::ChromiumRL::PageNode>
InspectorChromiumRLAgent::BuildPageNode(
    Node* node,
    const String& ref,
    int index,
    const String& parent_ref,
    std::unique_ptr<protocol::Array<String>> child_refs,
    int source_order,
    const gfx::RectF& css_box,
    bool visible,
    bool in_viewport,
    bool hit_testable,
    bool scrollable,
    const String& repeated_group_id,
    std::optional<int> repeated_item_index,
    int max_text_chars) {
  bool text_truncated = false;
  String direct_text = ComputeDirectText(
      node, std::min<unsigned>(kStructuredMaxDirectTextChars, max_text_chars),
      &text_truncated);
  String subtree_text = ComputeSubtreeTextCapped(
      node, std::min<unsigned>(kStructuredMaxSubtreeTextChars, max_text_chars),
      &text_truncated);

  auto empty_attrs =
      std::make_unique<protocol::Array<protocol::ChromiumRL::NodeAttribute>>();
  auto empty_states =
      std::make_unique<protocol::Array<protocol::ChromiumRL::NodeState>>();
  auto empty_actions = std::make_unique<protocol::Array<String>>();

  String tag = node->nodeName().ToAsciiLower();
  String role;
  String accessible_name;
  String description;
  String semantic_boundary;
  double confidence = 0.5;

  auto bounds = protocol::DOM::Rect::create()
                    .setX(css_box.x())
                    .setY(css_box.y())
                    .setWidth(css_box.width())
                    .setHeight(css_box.height())
                    .build();

  if (auto* element = DynamicTo<Element>(node)) {
    tag = element->tagName().ToAsciiLower();
    role = GetAriaRoleName(element);
    accessible_name = GetAccessibleName(element);
    const AtomicString& aria_description =
        element->FastGetAttribute(html_names::kAriaDescriptionAttr);
    if (!aria_description.IsNull())
      description = aria_description;
    semantic_boundary = DetectSemanticBoundary(element);
    empty_attrs = CollectSelectedAttributes(element);
    empty_states = CollectNodeStates(element);
    empty_actions = CollectActionTypes(element, scrollable);
    confidence = IsInteractiveElement(element) ? 0.92 : 0.72;
    if (!semantic_boundary.empty())
      confidence = std::max(confidence, 0.82);
  }

  if (tag == "html" || tag == "body") {
    direct_text = String();
    subtree_text = String();
    text_truncated = false;
  }

  auto page_node = protocol::ChromiumRL::PageNode::create()
                       .setRef(ref)
                       .setIndex(index)
                       .setNodeId(IdentifiersFactory::IntIdForNode(node))
                       .setBackendNodeId(IdentifiersFactory::IntIdForNode(node))
                       .setChildRefs(std::move(child_refs))
                       .setSourceOrder(source_order)
                       .setTag(tag)
                       .setSelectedAttributes(std::move(empty_attrs))
                       .setStates(std::move(empty_states))
                       .setActionTypes(std::move(empty_actions))
                       .setBounds(std::move(bounds))
                       .setClippedBounds(protocol::DOM::Rect::create()
                                             .setX(css_box.x())
                                             .setY(css_box.y())
                                             .setWidth(css_box.width())
                                             .setHeight(css_box.height())
                                             .build())
                       .setVisible(visible)
                       .setInViewport(in_viewport)
                       .setOccluded(false)
                       .setHitTestable(hit_testable)
                       .setScrollable(scrollable)
                       .setConfidence(confidence)
                       .setTruncated(text_truncated)
                       .build();

  if (!parent_ref.empty())
    page_node->setParentRef(parent_ref);
  if (!role.empty())
    page_node->setRole(role);
  if (!accessible_name.empty())
    page_node->setAccessibleName(accessible_name);
  if (!description.empty())
    page_node->setDescription(description);
  if (!direct_text.empty())
    page_node->setDirectText(direct_text);
  if (!subtree_text.empty())
    page_node->setSubtreeText(subtree_text);
  if (!semantic_boundary.empty())
    page_node->setSemanticBoundary(semantic_boundary);
  if (!repeated_group_id.empty())
    page_node->setRepeatedGroupId(repeated_group_id);
  if (repeated_item_index.has_value())
    page_node->setRepeatedItemIndex(*repeated_item_index);

  return page_node;
}

protocol::Response InspectorChromiumRLAgent::captureStructuredSnapshot(
    std::optional<bool> in_viewport_only,
    std::optional<String> root_selector,
    std::optional<int> max_nodes,
    std::optional<int> max_text_chars,
    std::optional<bool> include_offscreen,
    std::optional<bool> include_diff,
    std::optional<bool> update_baseline,
    std::unique_ptr<protocol::ChromiumRL::StructuredPageSnapshot>* out) {
  Document* document = inspected_frames_->Root()->GetDocument();
  if (!document)
    return protocol::Response::ServerError("No document");
  document->UpdateStyleAndLayout(DocumentUpdateReason::kInspector);

  double scroll_top = 0;
  double viewport_height = 0;
  double viewport_width = 0;
  double device_pixel_ratio = 1.0;
  if (LocalFrame* frame = document->GetFrame()) {
    device_pixel_ratio = frame->DevicePixelRatio();
    if (device_pixel_ratio <= 0)
      device_pixel_ratio = 1.0;
  }
  if (auto* view = document->View()) {
    if (auto* viewport = view->LayoutViewport()) {
      scroll_top = viewport->GetScrollOffset().y();
      viewport_height = viewport->VisibleContentRect(kExcludeScrollbars).height();
      viewport_width = viewport->VisibleContentRect(kExcludeScrollbars).width();
    }
  }

  Node* root = document;
  if (root_selector.has_value() && !root_selector->empty()) {
    if (Element* el =
            document->querySelector(AtomicString(*root_selector),
                                    ASSERT_NO_EXCEPTION)) {
      root = el;
    }
  }

  struct StructuredCandidate {
    wtf_size_t node_index = 0;
    gfx::RectF css_box;
    bool visible = false;
    bool in_viewport = false;
    bool hit_testable = false;
    bool scrollable = false;
    int source_order = 0;
  };

  const bool viewport_only = in_viewport_only.value_or(false);
  const bool allow_offscreen =
      include_offscreen.value_or(false) || !viewport_only;
  const int node_limit =
      std::max(1, max_nodes.value_or(kStructuredDefaultMaxNodes));
  const int text_limit =
      std::max(1, max_text_chars.value_or(kStructuredDefaultMaxTextChars));

  HeapVector<Member<Node>> selected_nodes;
  std::vector<StructuredCandidate> candidates;
  HeapHashMap<Member<Node>, String> ref_by_node;
  HashMap<String, int> repeated_signature_counts;

  int raw_nodes = 0;
  int dropped_hidden = 0;
  int dropped_offscreen = 0;
  int source_order = 0;
  bool snapshot_truncated = false;

  for (Node& node : NodeTraversal::StartsAt(*root)) {
    raw_nodes++;
    source_order++;

    if (node.IsDocumentNode())
      continue;

    Element* element = DynamicTo<Element>(&node);
    Element* visibility_element = element;
    if (!visibility_element && node.parentNode())
      visibility_element = DynamicTo<Element>(node.parentNode());

    if (element && IsDecorativeOrUnsupported(element)) {
      dropped_hidden++;
      continue;
    }

    if (visibility_element) {
      const ComputedStyle* style = visibility_element->GetComputedStyle();
      if (!style || style->Display() == EDisplay::kNone ||
          style->Visibility() != EVisibility::kVisible ||
          style->Opacity() <= 0.01f) {
        dropped_hidden++;
        continue;
      }
      const AtomicString& aria_hidden =
          visibility_element->FastGetAttribute(html_names::kAriaHiddenAttr);
      if (aria_hidden == "true") {
        dropped_hidden++;
        continue;
      }
    }

    LayoutObject* layout_object = node.GetLayoutObject();
    if (!layout_object && visibility_element)
      layout_object = visibility_element->GetLayoutObject();
    if (!layout_object) {
      dropped_hidden++;
      continue;
    }

    gfx::RectF box = layout_object->AbsoluteBoundingBoxRectF();
    bool has_box = box.width() > 0 && box.height() > 0;
    bool has_text = node.IsTextNode()
                        ? !NormalizeTextContent(node.nodeValue()).empty()
                        : element &&
                              (!NormalizeTextContent(element->textContent())
                                    .empty() ||
                               !GetAccessibleName(element).empty());
    if (!has_box && !has_text) {
      dropped_hidden++;
      continue;
    }

    bool is_in_viewport =
        IsElementInViewport(box, scroll_top, viewport_width, viewport_height);
    if (!allow_offscreen && !is_in_viewport) {
      dropped_offscreen++;
      continue;
    }

    bool visible = has_box || has_text;
    if (!IsStructuredSnapshotCandidate(&node, visible, is_in_viewport,
                                       allow_offscreen)) {
      continue;
    }

    if (static_cast<int>(candidates.size()) >= node_limit) {
      snapshot_truncated = true;
      break;
    }

    bool scrollable = element ? IsScrollableElement(element) : false;
    bool hit_testable =
        element && visible && IsElementHitTestable(element, box);
    gfx::RectF css_box(box.x() / device_pixel_ratio,
                       box.y() / device_pixel_ratio,
                       box.width() / device_pixel_ratio,
                       box.height() / device_pixel_ratio);
    selected_nodes.push_back(&node);
    candidates.push_back(StructuredCandidate{selected_nodes.size() - 1, css_box, visible,
                                             is_in_viewport, hit_testable,
                                             scrollable, source_order});

    String prefix = "n";
    if (element) {
      String tag = element->tagName().ToAsciiLower();
      if (tag == "time") {
        prefix = "t";
      } else if (IsInteractiveElement(element)) {
        prefix = "e";
      } else if (!DetectSemanticBoundary(element).empty() || scrollable) {
        prefix = "g";
      } else {
        prefix = "n";
      }

      Node* parent = node.parentNode();
      String signature;
      if (parent) {
        String role = GetAriaRoleName(element).ToAsciiLower();
        signature = String::Number(IdentifiersFactory::IntIdForNode(parent)) +
                    "|" + tag + "|" + role + "|" +
                    DetectSemanticBoundary(element);
      }
      if (!signature.empty()) {
        auto it = repeated_signature_counts.find(signature);
        repeated_signature_counts.Set(
            signature,
            it == repeated_signature_counts.end() ? 1 : it->value + 1);
      }
    }
    ref_by_node.Set(&node, prefix + String::Number(candidates.size()));
  }

  HashMap<String, String> repeated_group_by_signature;
  HashMap<String, int> repeated_index_by_signature;
  HeapHashMap<Member<Node>, String> repeated_group_by_node;
  HeapHashMap<Member<Node>, int> repeated_index_by_node;
  int group_count = 0;
  for (Node* node : selected_nodes) {
    Element* element = DynamicTo<Element>(node);
    if (!element || !node->parentNode())
      continue;
    String signature =
        String::Number(IdentifiersFactory::IntIdForNode(node->parentNode())) +
        "|" + element->tagName().ToAsciiLower() + "|" +
        GetAriaRoleName(element).ToAsciiLower() + "|" +
        DetectSemanticBoundary(element);
    auto count_it = repeated_signature_counts.find(signature);
    if (count_it == repeated_signature_counts.end() || count_it->value < 3)
      continue;
    String group_id;
    auto group_it = repeated_group_by_signature.find(signature);
    if (group_it == repeated_group_by_signature.end()) {
      group_count++;
      group_id = "rg" + String::Number(group_count);
      repeated_group_by_signature.Set(signature, group_id);
      repeated_index_by_signature.Set(signature, 0);
    } else {
      group_id = group_it->value;
    }
    int index = repeated_index_by_signature.at(signature);
    repeated_group_by_node.Set(node, group_id);
    repeated_index_by_node.Set(node, index);
    repeated_index_by_signature.Set(signature, index + 1);
  }

  HeapHashMap<Member<Node>, String> parent_ref_by_node;
  HeapHashMap<Member<Node>, Vector<String>> child_refs_by_node;
  auto roots = std::make_unique<protocol::Array<String>>();

  for (Node* node : selected_nodes) {
    String parent_ref;
    for (Node* parent = node->parentNode(); parent;
         parent = parent->parentNode()) {
      auto parent_it = ref_by_node.find(parent);
      if (parent_it != ref_by_node.end()) {
        parent_ref = parent_it->value;
        Vector<String>& children = child_refs_by_node.insert(parent, Vector<String>()).stored_value->value;
        children.push_back(ref_by_node.at(node));
        break;
      }
    }
    if (parent_ref.empty())
      roots->push_back(ref_by_node.at(node));
    parent_ref_by_node.Set(node, parent_ref);
  }

  auto nodes =
      std::make_unique<protocol::Array<protocol::ChromiumRL::PageNode>>();
  int total_text_chars = 0;
  for (const StructuredCandidate& candidate : candidates) {
    Node* node = selected_nodes[candidate.node_index];
    int index = static_cast<int>(candidate.node_index) + 1;
    auto child_refs = std::make_unique<protocol::Array<String>>();
    auto children_it = child_refs_by_node.find(node);
    if (children_it != child_refs_by_node.end()) {
      wtf_size_t child_count = 0;
      for (const String& child_ref : children_it->value) {
        if (child_count >= kStructuredMaxChildRefs) {
          snapshot_truncated = true;
          break;
        }
        child_refs->push_back(child_ref);
        child_count++;
      }
    }

    String repeated_group_id;
    std::optional<int> repeated_item_index;
    auto group_it = repeated_group_by_node.find(node);
    if (group_it != repeated_group_by_node.end())
      repeated_group_id = group_it->value;
    auto repeated_index_it = repeated_index_by_node.find(node);
    if (repeated_index_it != repeated_index_by_node.end())
      repeated_item_index = repeated_index_it->value;

    auto page_node = BuildPageNode(
        node, ref_by_node.at(node), index,
        parent_ref_by_node.at(node), std::move(child_refs),
        candidate.source_order, candidate.css_box, candidate.visible,
        candidate.in_viewport, candidate.hit_testable, candidate.scrollable,
        repeated_group_id, repeated_item_index, text_limit);
    if (page_node->hasDirectText())
      total_text_chars += page_node->getDirectText("").length();
    if (page_node->hasSubtreeText())
      total_text_chars += page_node->getSubtreeText("").length();
    if (total_text_chars > text_limit)
      snapshot_truncated = true;
    nodes->push_back(std::move(page_node));
  }

  *out = protocol::ChromiumRL::StructuredPageSnapshot::create()
             .setSnapshotId(CreateCanonicalUuidString())
             .setDocumentRevision(source_order)
             .setUrl(document->Url().GetString())
             .setTitle(document->title())
             .setRoots(std::move(roots))
             .setNodes(std::move(nodes))
             .setStats(protocol::ChromiumRL::StructuredSnapshotStats::create()
                           .setRawNodes(raw_nodes)
                           .setReturnedNodes(static_cast<int>(candidates.size()))
                           .setTextChars(total_text_chars)
                           .setGroups(group_count)
                           .setDroppedHidden(dropped_hidden)
                           .setDroppedOffscreen(dropped_offscreen)
                           .setDroppedDuplicate(0)
                           .setTruncated(snapshot_truncated)
                           .build())
             .build();

  if (include_diff.value_or(false) && agent_observation_baseline_) {
    std::unique_ptr<protocol::ChromiumRL::DOMDiffResult> diff;
    protocol::Response diff_response =
        compareDOMState(std::move(agent_observation_baseline_), nullptr, &diff);
    if (diff_response.IsSuccess() && diff)
      (*out)->setDiff(std::move(diff));
  }

  if (update_baseline.value_or(false)) {
    std::unique_ptr<protocol::ChromiumRL::RichDOMState> next_state;
    protocol::Response baseline_response = saveDOMState(&next_state);
    if (baseline_response.IsSuccess() && next_state)
      agent_observation_baseline_ = std::move(next_state);
  }

  return protocol::Response::Success();
}

protocol::Response InspectorChromiumRLAgent::getAgentObservation(
    std::optional<bool> in_viewport_only,
    std::optional<String> root_selector,
    std::optional<int> max_elements,
    std::optional<bool> include_content,
    std::optional<bool> include_diff,
    std::optional<bool> update_baseline,
    std::optional<int> max_interactive_elements,
    std::optional<int> max_content_blocks,
    std::optional<int> max_diff_items,
    std::unique_ptr<protocol::ChromiumRL::AgentObservation>* out) {

  Document* document = inspected_frames_->Root()->GetDocument();
  if (!document) return protocol::Response::ServerError("No document");
  document->UpdateStyleAndLayout(DocumentUpdateReason::kInspector);
  (void)max_diff_items;

  // Scroll info
  double scroll_top = 0, page_height = 0, viewport_height = 0,
         viewport_width = 0;
  double device_pixel_ratio = 1.0;
  if (LocalFrame* frame = document->GetFrame()) {
    device_pixel_ratio = frame->DevicePixelRatio();
    if (device_pixel_ratio <= 0)
      device_pixel_ratio = 1.0;
  }
  if (auto* view = document->View()) {
    if (auto* viewport = view->LayoutViewport()) {
      scroll_top = viewport->GetScrollOffset().y();
      viewport_height = viewport->VisibleContentRect(kExcludeScrollbars).height();
      viewport_width = viewport->VisibleContentRect(kExcludeScrollbars).width();
    }
    if (auto* layout_view = view->GetLayoutView()) {
      page_height = layout_view->DocumentRect().Height();
    }
  }

  auto scroll = protocol::ChromiumRL::ScrollInfo::create()
    .setScrollTop(scroll_top / device_pixel_ratio)
    .setPageHeight(page_height / device_pixel_ratio)
    .setViewportWidth(viewport_width / device_pixel_ratio)
    .setViewportHeight(viewport_height / device_pixel_ratio)
    .setDevicePixelRatio(device_pixel_ratio)
    .setCanScrollDown(scroll_top + viewport_height < page_height - 10)
    .setCanScrollUp(scroll_top > 10)
    .build();

  // Determine root
  Node* root = document;
  if (root_selector.has_value() && !root_selector->empty()) {
    if (Element* el = document->querySelector(AtomicString(*root_selector), ASSERT_NO_EXCEPTION)) {
      root = el;
    }
  }

  int legacy_limit = max_elements.value_or(100);
  int interactive_limit = max_interactive_elements.value_or(legacy_limit);
  int content_limit = max_content_blocks.value_or(include_content.value_or(false) ? 40 : 0);
  if (interactive_limit <= 0) interactive_limit = legacy_limit;
  if (content_limit < 0) content_limit = 0;
  bool viewport_only = in_viewport_only.value_or(false);
  bool want_content = include_content.value_or(false);
  const int bounded_content_limit =
      std::min(content_limit, kAgentObservationMaxContentScanElements);
  const int content_scan_limit =
      want_content && content_limit > 0
          ? std::min(kAgentObservationMaxContentScanElements,
                     bounded_content_limit *
                             kAgentObservationContentScanMultiplier +
                         kAgentObservationContentScanSlack)
          : 0;
  String last_context;

  auto elements = std::make_unique<protocol::Array<protocol::ChromiumRL::ObservedElement>>();
  auto content_blocks = std::make_unique<protocol::Array<protocol::ChromiumRL::ContentBlock>>();

  struct InteractiveCandidate {
    Element* element;
    gfx::RectF css_box;
    bool is_in_viewport;
    bool is_hit_testable;
    double score;
    int dom_order;
    String dedup_key;
  };

  struct ContentCandidate {
    Element* element;
    double score;
    int dom_order;
    String dedup_key;
    String text;
  };

  std::vector<InteractiveCandidate> interactive_candidates;
  std::vector<ContentCandidate> content_candidates;
  HashSet<String> seen_interactive_keys;
  HashSet<String> seen_content_keys;

  int raw_elements = 0;
  int visible_elements = 0;
  int dropped_hidden = 0;
  int dropped_offscreen = 0;
  int dropped_covered = 0;
  int dropped_empty = 0;
  int dropped_duplicate = 0;
  int dom_order = 0;
  int content_elements_scanned = 0;

  for (Element& el : ElementTraversal::DescendantsOf(*root)) {
    raw_elements++;
    dom_order++;

    // Skip invisible and non-rendered nodes early.
    const ComputedStyle* style = el.GetComputedStyle();
    if (!style || style->Display() == EDisplay::kNone ||
        style->Visibility() != EVisibility::kVisible ||
        style->Opacity() <= 0.01f) {
      dropped_hidden++;
      continue;
    }

    if (!el.GetLayoutObject()) {
      dropped_hidden++;
      continue;
    }
    gfx::RectF box = el.GetLayoutObject()->AbsoluteBoundingBoxRectF();
    if (box.width() <= 1 || box.height() <= 1) {
      dropped_hidden++;
      continue;
    }
    visible_elements++;

    // Skip aria-hidden and presentational elements
    const AtomicString& aria_hidden = el.FastGetAttribute(html_names::kAriaHiddenAttr);
    if (aria_hidden == "true") {
      dropped_hidden++;
      continue;
    }
    const AtomicString& role_attr = el.FastGetAttribute(html_names::kRoleAttr);
    if (role_attr == "presentation" || role_attr == "none") {
      dropped_hidden++;
      continue;
    }

    bool interactive = IsInteractiveElement(&el);
    bool content = want_content && content_elements_scanned < content_scan_limit &&
                   IsContentElement(&el);

    if (!interactive && !content) continue;

    bool is_in_viewport =
        IsElementInViewport(box, scroll_top, viewport_width, viewport_height);
    if (viewport_only && !is_in_viewport) {
      dropped_offscreen++;
      continue;
    }

    if (interactive && style->UsedPointerEvents() == EPointerEvents::kNone) {
      dropped_covered++;
      continue;
    }

    if (interactive && el.FastHasAttribute(html_names::kDisabledAttr)) {
      dropped_hidden++;
      continue;
    }
    if (interactive && el.FastHasAttribute(html_names::kReadonlyAttr) &&
        (el.HasTagName(html_names::kInputTag) ||
         el.HasTagName(html_names::kTextareaTag))) {
      dropped_hidden++;
      continue;
    }

    // Skip empty links (no text, no aria-label, no img alt)
    if (interactive && !HasHumanReadableLabel(&el)) {
      dropped_empty++;
      continue;
    }

    if (interactive) {
      bool is_hit_testable = IsElementHitTestable(&el, box);
      if (viewport_only && !is_hit_testable) {
        dropped_covered++;
        continue;
      }
      String dedup_key = BuildObservationDedupKey(&el);
      if (!dedup_key.empty() && seen_interactive_keys.Contains(dedup_key)) {
        dropped_duplicate++;
        continue;
      }
      if (!dedup_key.empty())
        seen_interactive_keys.insert(dedup_key);

      gfx::RectF css_box(box.x() / device_pixel_ratio,
                         box.y() / device_pixel_ratio,
                         box.width() / device_pixel_ratio,
                         box.height() / device_pixel_ratio);
      interactive_candidates.push_back(InteractiveCandidate{
          &el, css_box, is_in_viewport, is_hit_testable,
          ScoreInteractiveElement(&el, is_in_viewport, is_hit_testable),
          dom_order, dedup_key});
    }

    if (content) {
      content_elements_scanned++;
      bool uses_structured_content_builder =
          el.HasTagName(html_names::kTableTag) ||
          el.HasTagName(html_names::kUlTag) ||
          el.HasTagName(html_names::kOlTag);
      String text;
      if (!uses_structured_content_builder) {
        text = ComputeSubtreeTextCapped(
            &el, kAgentObservationMaxContentTextChars, nullptr);
      }
      if (!uses_structured_content_builder && IsJunkContentText(text)) {
        dropped_empty++;
        continue;
      }
      String dedup_key = uses_structured_content_builder
                             ? BuildSelectorPath(&el)
                             : text.substr(0, 180).ToAsciiLower();
      if (!dedup_key.empty() && seen_content_keys.Contains(dedup_key)) {
        dropped_duplicate++;
        continue;
      }
      if (!dedup_key.empty())
        seen_content_keys.insert(dedup_key);
      content_candidates.push_back(ContentCandidate{
          &el, ScoreContentElement(&el, is_in_viewport, text), dom_order,
          dedup_key, text});
    }
  }

  std::sort(interactive_candidates.begin(), interactive_candidates.end(),
            [](const InteractiveCandidate& a, const InteractiveCandidate& b) {
              if (a.score != b.score) return a.score > b.score;
              return a.dom_order < b.dom_order;
            });
  std::sort(content_candidates.begin(), content_candidates.end(),
            [](const ContentCandidate& a, const ContentCandidate& b) {
              if (a.score != b.score) return a.score > b.score;
              return a.dom_order < b.dom_order;
            });

  int idx = 0;
  for (const auto& candidate : interactive_candidates) {
    if (idx >= interactive_limit) break;
    auto obs_el = BuildObservedElement(candidate.element, idx, candidate.css_box,
                                      candidate.is_in_viewport,
                                      candidate.is_hit_testable);
    if (obs_el->hasContext()) {
      String ctx = obs_el->getContext("");
      if (!ctx.empty() && ctx == last_context) {
        obs_el->setContext("");
      } else if (!ctx.empty()) {
        last_context = ctx;
      }
    }
    elements->push_back(std::move(obs_el));
    idx++;
  }

  int content_count = 0;
  for (const auto& candidate : content_candidates) {
    if (content_count >= content_limit) break;
    auto block = BuildContentBlock(candidate.element, candidate.text);
    if (block) {
      content_blocks->push_back(std::move(block));
      content_count++;
    }
  }

  int returned_interactives = static_cast<int>(elements->size());
  int returned_content_blocks =
      want_content ? static_cast<int>(content_blocks->size()) : 0;

  *out = protocol::ChromiumRL::AgentObservation::create()
    .setUrl(document->Url().GetString())
    .setTitle(document->title())
    .setScroll(std::move(scroll))
    .setElements(std::move(elements))
    .build();

  if (want_content) {
    (*out)->setContent(std::move(content_blocks));
  }

  (*out)->setStats(protocol::ChromiumRL::ObservationStats::create()
      .setRawElements(raw_elements)
      .setVisibleElements(visible_elements)
      .setCandidateInteractives(static_cast<int>(interactive_candidates.size()))
      .setReturnedInteractives(returned_interactives)
      .setCandidateContentBlocks(static_cast<int>(content_candidates.size()))
      .setReturnedContentBlocks(returned_content_blocks)
      .setDroppedHidden(dropped_hidden)
      .setDroppedOffscreen(dropped_offscreen)
      .setDroppedCovered(dropped_covered)
      .setDroppedEmpty(dropped_empty)
      .setDroppedDuplicate(dropped_duplicate)
      .build());

  if (include_diff.value_or(false) && agent_observation_baseline_) {
    std::unique_ptr<protocol::ChromiumRL::DOMDiffResult> diff;
    protocol::Response diff_response =
        compareDOMState(std::move(agent_observation_baseline_), nullptr, &diff);
    if (diff_response.IsSuccess() && diff) {
      (*out)->setDiff(std::move(diff));
    }
  }

  if (update_baseline.value_or(false)) {
    std::unique_ptr<protocol::ChromiumRL::RichDOMState> next_state;
    protocol::Response baseline_response = saveDOMState(&next_state);
    if (baseline_response.IsSuccess() && next_state) {
      agent_observation_baseline_ = std::move(next_state);
    }
  }

  return protocol::Response::Success();
}


}  // namespace blink
