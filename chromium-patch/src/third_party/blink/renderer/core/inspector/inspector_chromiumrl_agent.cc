// Copyright 2025 The Chromium Authors
// File: third_party/blink/renderer/core/inspector/inspector_chromiumrl_agent.cc

#include "third_party/blink/renderer/core/inspector/inspector_chromiumrl_agent.h"
#include "third_party/blink/renderer/core/css/css_property_name.h"

#include <vector>
#include <cmath>
#include <algorithm>
#include "base/base64.h"
#include "base/no_destructor.h"
#include "crypto/sha2.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_mutation_observer_init.h"
#include "third_party/blink/renderer/core/dom/mutation_record.h"
#include "third_party/blink/renderer/core/inspector/inspector_dom_agent.h"
#include "third_party/blink/renderer/core/inspector/protocol/chromium_rl.h"
#include "third_party/blink/renderer/core/inspector/protocol/protocol.h"
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
#include "third_party/blink/renderer/core/html/parser/html_entity_parser.h"
#include "third_party/blink/renderer/core/accessibility/ax_object_cache.h"
#include "third_party/blink/renderer/core/accessibility/ax_utilities_generated.h"
#include "third_party/blink/renderer/core/input/event_handler.h"
#include "third_party/blink/renderer/core/layout/hit_test_location.h"
#include "third_party/blink/renderer/core/layout/hit_test_request.h"
#include "third_party/blink/renderer/core/layout/hit_test_result.h"
#include "third_party/blink/renderer/platform/text/segmented_string.h"
#include "third_party/blink/renderer/platform/wtf/text/case_map.h"
#include "third_party/icu/source/common/unicode/uchar.h"
#include "third_party/re2/src/re2/re2.h"
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


protocol::Response InspectorChromiumRLAgent::BuildStructuredSnapshotDiff(
    protocol::ChromiumRL::StructuredPageSnapshot* before_snapshot,
    protocol::ChromiumRL::StructuredPageSnapshot* after_snapshot,
    const std::optional<String>& action_type,
    std::unique_ptr<protocol::ChromiumRL::StructuredSnapshotDiff>* out) {
  using PageNode = protocol::ChromiumRL::PageNode;

  struct IdentityStatsData {
    int nodes = 0;
    int anchored_nodes = 0;
    int positional_nodes = 0;
    int duplicate_anchor_bases = 0;
    int duplicate_anchor_nodes = 0;
    int path_collisions = 0;
  };
  struct IdentityProblemData {
    String kind;
    String ref;
    String parent_ref;
    String child_ref;
    Vector<String> child_refs;
    String path;
    String existing_path;
    Vector<PageNode*> nodes;
  };
  struct PathIndexData {
    HashMap<String, PageNode*> by_ref;
    HashMap<String, PageNode*> by_path;
    HashMap<String, String> path_by_ref;
    Vector<String> refs_in_order;
    Vector<String> paths;
    Vector<IdentityProblemData> problems;
    IdentityStatsData stats;
  };
  struct CanonicalValue {
    enum class Kind { kNull, kString, kAttributes, kStates, kActions };
    Kind kind = Kind::kNull;
    String text;
    Vector<std::pair<String, String>> pairs;
    Vector<String> strings;
  };
  struct FieldChangeData {
    int field = 0;
    CanonicalValue before;
    CanonicalValue after;
  };
  struct DiffEntryData {
    String path;
    bool semantic = false;
    PageNode* node = nullptr;
    Vector<FieldChangeData> fields;
    String repeated_group_id;
    std::optional<int> repeated_item_index;
  };
  struct SubtreeIndexData {
    String operation;
    String root_path;
    Vector<String> member_paths;
    int descendant_count = 0;
    int subtree_node_count = 0;
    double document_percent = 0;
    String visible_text;
    Vector<String> interactive_paths;
  };
  struct RepeatedGroupIndexData {
    String operation;
    String repeated_group_id;
    String tag;
    String role;
    Vector<String> changed_fields;
    int count = 0;
    Vector<int> item_indices;
    Vector<String> member_paths;
  };

  if (!before_snapshot || !after_snapshot)
    return protocol::Response::InvalidParams("Structured snapshots are required");

  auto is_python_whitespace = [](UChar c) -> bool {
    return (c >= 0x0009 && c <= 0x000d) ||
           (c >= 0x001c && c <= 0x0020) || c == 0x0085 || c == 0x00a0 ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
           c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
           c == 0x3000;
  };
  auto clean_text = [&is_python_whitespace](const String& value) -> String {
    StringBuilder result;
    bool pending_space = false;
    for (wtf_size_t i = 0; i < value.length(); ++i) {
      UChar c = value[i];
      if (is_python_whitespace(c)) {
        if (result.length())
          pending_space = true;
        continue;
      }
      if (pending_space) {
        result.Append(' ');
        pending_space = false;
      }
      result.Append(c);
    }
    return result.ToString();
  };
  auto fold_case = [](const String& value) -> String {
    return value.FoldCase();
  };
  auto python_code_point_less = [](const String& left,
                                   const String& right) -> bool {
    auto next_code_point = [](const String& value, wtf_size_t* offset) -> UChar32 {
      UChar32 result = value[(*offset)++];
      if (U16_IS_LEAD(result) && *offset < value.length() &&
          U16_IS_TRAIL(value[*offset])) {
        result = U16_GET_SUPPLEMENTARY(result, value[(*offset)++]);
      }
      return result;
    };
    wtf_size_t left_offset = 0;
    wtf_size_t right_offset = 0;
    while (left_offset < left.length() && right_offset < right.length()) {
      UChar32 left_code_point = next_code_point(left, &left_offset);
      UChar32 right_code_point = next_code_point(right, &right_offset);
      if (left_code_point != right_code_point)
        return left_code_point < right_code_point;
    }
    return left.length() < right.length();
  };
  auto json_quote = [](const String& value) -> String {
    static const char kHex[] = "0123456789abcdef";
    StringBuilder result;
    result.Append('"');
    for (wtf_size_t i = 0; i < value.length(); ++i) {
      UChar c = value[i];
      switch (c) {
        case '"': result.Append("\\\""); break;
        case '\\': result.Append("\\\\"); break;
        case '\b': result.Append("\\b"); break;
        case '\f': result.Append("\\f"); break;
        case '\n': result.Append("\\n"); break;
        case '\r': result.Append("\\r"); break;
        case '\t': result.Append("\\t"); break;
        default:
          if (c < 0x20) {
            result.Append("\\u00");
            result.Append(kHex[(c >> 4) & 0xf]);
            result.Append(kHex[c & 0xf]);
          } else {
            result.Append(c);
          }
      }
    }
    result.Append('"');
    return result.ToString();
  };
  auto rstrip_python_whitespace = [&is_python_whitespace](const String& value) {
    wtf_size_t end = value.length();
    while (end && is_python_whitespace(value[end - 1]))
      --end;
    return value.substr(0, end);
  };
  auto contains_string = [](const Vector<String>& values,
                            const String& value) -> bool {
    for (const String& existing : values) {
      if (existing == value)
        return true;
    }
    return false;
  };
  auto node_tag = [&clean_text](PageNode* node) -> String {
    String raw_value = node ? node->getTag() : String();
    if (raw_value.empty())
      raw_value = "?";
    String value = clean_text(raw_value).ToAsciiLower();
    StringBuilder result;
    for (wtf_size_t i = 0; i < value.length(); ++i) {
      if (value[i] == '/')
        result.Append('_');
      else
        result.Append(value[i]);
    }
    return result.ToString();
  };
  auto stable_anchor = [&clean_text, &fold_case](PageNode* node) -> String {
    if (!node)
      return String();
    String value = clean_text(node->getDirectText(String()));
    if (value.empty()) {
      String role = clean_text(node->getRole(String())).ToAsciiLower();
      String semantic =
          clean_text(node->getSemanticBoundary(String())).ToAsciiLower();
      bool has_non_scroll_action = false;
      for (const String& action : *node->getActionTypes()) {
        if (clean_text(action).ToAsciiLower() != "scroll") {
          has_non_scroll_action = true;
          break;
        }
      }
      bool meaningful_role = role != "" && role != "application" &&
                             role != "document" && role != "generic" &&
                             role != "main" && role != "none";
      if (node->getChildRefs()->empty() || !semantic.empty() ||
          has_non_scroll_action || meaningful_role) {
        value = clean_text(node->getAccessibleName(String()));
      }
    }
    value = fold_case(value);
    StringBuilder anchor;
    bool separator_pending = false;
    for (wtf_size_t i = 0; i < value.length(); ++i) {
      UChar c = value[i];
      bool ascii_word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
      if (ascii_word) {
        if (separator_pending && anchor.length() > 0)
          anchor.Append('-');
        anchor.Append(c);
        separator_pending = false;
      } else if (anchor.length() > 0) {
        separator_pending = true;
      }
    }
    String result = anchor.ToString();
    while (result.ends_with("-"))
      result = result.substr(0, result.length() - 1);
    return result.length() <= 20 ? result : result.substr(0, 20);
  };

  auto build_path_index = [&](protocol::ChromiumRL::StructuredPageSnapshot* snapshot)
      -> PathIndexData {
    PathIndexData result;
    HashMap<String, int> node_position;
    int position = 0;
    for (const auto& owned_node : *snapshot->getNodes()) {
      PageNode* node = owned_node.get();
      String ref = node->getRef();
      if (ref.empty()) {
        IdentityProblemData problem;
        problem.kind = "missing_ref";
        problem.nodes.push_back(node);
        result.problems.push_back(std::move(problem));
        continue;
      }
      auto existing = result.by_ref.find(ref);
      if (existing != result.by_ref.end()) {
        IdentityProblemData problem;
        problem.kind = "duplicate_ref";
        problem.ref = ref;
        problem.nodes.push_back(existing->value);
        problem.nodes.push_back(node);
        result.problems.push_back(std::move(problem));
        continue;
      }
      result.by_ref.Set(ref, node);
      node_position.Set(ref, position++);
      result.refs_in_order.push_back(ref);
    }
    result.stats.nodes = static_cast<int>(result.refs_in_order.size());

    HashMap<String, Vector<String>> children_from_parent;
    for (const String& ref : result.refs_in_order)
      children_from_parent.Set(ref, Vector<String>());
    for (const String& ref : result.refs_in_order) {
      PageNode* node = result.by_ref.at(ref);
      if (!node->hasParentRef())
        continue;
      String parent_ref = node->getParentRef(String());
      auto parent = children_from_parent.find(parent_ref);
      if (parent != children_from_parent.end())
        parent->value.push_back(ref);
    }

    HashMap<String, Vector<String>> ordered_children;
    for (const String& ref : result.refs_in_order) {
      PageNode* node = result.by_ref.at(ref);
      HashMap<String, int> child_counts;
      Vector<String> listed_children;
      Vector<String> duplicate_children;
      for (const String& child_ref : *node->getChildRefs()) {
        int count = 1;
        auto count_it = child_counts.find(child_ref);
        if (count_it != child_counts.end())
          count = count_it->value + 1;
        child_counts.Set(child_ref, count);
        if (count == 2)
          duplicate_children.push_back(child_ref);
      }
      std::sort(duplicate_children.begin(), duplicate_children.end(),
                python_code_point_less);
      if (!duplicate_children.empty()) {
        IdentityProblemData problem;
        problem.kind = "duplicate_child_ref";
        problem.parent_ref = ref;
        problem.child_refs = duplicate_children;
        problem.nodes.push_back(node);
        result.problems.push_back(std::move(problem));
      }
      for (const String& child_ref : *node->getChildRefs()) {
        auto child_it = result.by_ref.find(child_ref);
        if (child_it == result.by_ref.end())
          continue;
        PageNode* child = child_it->value;
        if (child->getParentRef(String()) != ref) {
          IdentityProblemData problem;
          problem.kind = "parent_child_disagreement";
          problem.parent_ref = ref;
          problem.child_ref = child_ref;
          problem.nodes.push_back(node);
          problem.nodes.push_back(child);
          result.problems.push_back(std::move(problem));
          continue;
        }
        if (!contains_string(listed_children, child_ref))
          listed_children.push_back(child_ref);
      }
      Vector<String> all_children = children_from_parent.at(ref);
      bool same_children = listed_children.size() == all_children.size();
      if (same_children) {
        for (const String& child_ref : listed_children) {
          if (!contains_string(all_children, child_ref)) {
            same_children = false;
            break;
          }
        }
      }
      if (!same_children) {
        std::sort(all_children.begin(), all_children.end(),
                  [&](const String& left, const String& right) {
                    PageNode* left_node = result.by_ref.at(left);
                    PageNode* right_node = result.by_ref.at(right);
                    int left_position = node_position.at(left);
                    int right_position = node_position.at(right);
                    int left_order = left_node->getSourceOrder();
                    int right_order = right_node->getSourceOrder();
                    if (!left_order)
                      left_order = left_position;
                    if (!right_order)
                      right_order = right_position;
                    if (left_order != right_order)
                      return left_order < right_order;
                    return left_position < right_position;
                  });
        ordered_children.Set(ref, std::move(all_children));
      } else {
        ordered_children.Set(ref, std::move(listed_children));
      }
    }

    Vector<String> root_refs;
    for (const String& ref : *snapshot->getRoots()) {
      if (result.by_ref.find(ref) != result.by_ref.end() &&
          !contains_string(root_refs, ref)) {
        root_refs.push_back(ref);
      }
    }
    for (const String& ref : result.refs_in_order) {
      PageNode* node = result.by_ref.at(ref);
      String parent_ref = node->getParentRef(String());
      if ((!node->hasParentRef() || result.by_ref.find(parent_ref) == result.by_ref.end()) &&
          !contains_string(root_refs, ref)) {
        root_refs.push_back(ref);
      }
    }

    auto sibling_segments = [&](const Vector<String>& refs) {
      HashMap<String, int> totals;
      Vector<String> tags;
      Vector<String> anchors;
      for (const String& ref : refs) {
        String tag = node_tag(result.by_ref.at(ref));
        String anchor = stable_anchor(result.by_ref.at(ref));
        tags.push_back(tag);
        anchors.push_back(anchor);
        if (!anchor.empty()) {
          String base = tag + String("\x1f") + anchor;
          auto it = totals.find(base);
          totals.Set(base, it == totals.end() ? 1 : it->value + 1);
        }
      }
      for (const auto& entry : totals) {
        if (entry.value > 1) {
          result.stats.duplicate_anchor_bases++;
          result.stats.duplicate_anchor_nodes += entry.value;
        }
      }
      HashMap<String, int> anchored_seen;
      HashMap<String, int> positional_seen;
      HashMap<String, String> segments;
      for (wtf_size_t i = 0; i < refs.size(); ++i) {
        const String& ref = refs[i];
        const String& tag = tags[i];
        const String& anchor = anchors[i];
        if (!anchor.empty()) {
          String base = tag + String("\x1f") + anchor;
          auto seen = anchored_seen.find(base);
          int ordinal = seen == anchored_seen.end() ? 1 : seen->value + 1;
          anchored_seen.Set(base, ordinal);
          segments.Set(ref, tag + "{" + anchor + "}[" +
                                String::Number(ordinal) + "]");
          result.stats.anchored_nodes++;
        } else {
          auto seen = positional_seen.find(tag);
          int ordinal = seen == positional_seen.end() ? 1 : seen->value + 1;
          positional_seen.Set(tag, ordinal);
          segments.Set(ref, tag + "[" + String::Number(ordinal) + "]");
          result.stats.positional_nodes++;
        }
      }
      return segments;
    };

    struct WalkTask {
      String ref;
      String path;
      bool exit = false;
    };
    HashSet<String> visiting;
    Vector<WalkTask> stack;
    HashMap<String, String> root_segments = sibling_segments(root_refs);
    for (wtf_size_t i = root_refs.size(); i > 0; --i)
      stack.push_back(WalkTask{root_refs[i - 1], root_segments.at(root_refs[i - 1]), false});
    while (!stack.empty()) {
      WalkTask task = stack.back();
      stack.pop_back();
      if (task.exit) {
        visiting.erase(task.ref);
        continue;
      }
      if (visiting.Contains(task.ref)) {
        IdentityProblemData problem;
        problem.kind = "cycle";
        problem.path = task.path;
        problem.nodes.push_back(result.by_ref.at(task.ref));
        result.problems.push_back(std::move(problem));
        continue;
      }
      auto existing_path = result.path_by_ref.find(task.ref);
      if (existing_path != result.path_by_ref.end()) {
        IdentityProblemData problem;
        problem.kind = "node_reached_more_than_once";
        problem.path = task.path;
        problem.existing_path = existing_path->value;
        problem.nodes.push_back(result.by_ref.at(task.ref));
        result.problems.push_back(std::move(problem));
        continue;
      }
      visiting.insert(task.ref);
      result.path_by_ref.Set(task.ref, task.path);
      stack.push_back(WalkTask{task.ref, task.path, true});
      Vector<String> children = ordered_children.at(task.ref);
      HashMap<String, String> segments = sibling_segments(children);
      for (wtf_size_t i = children.size(); i > 0; --i) {
        const String& child_ref = children[i - 1];
        stack.push_back(WalkTask{child_ref,
                                 task.path + "/" + segments.at(child_ref),
                                 false});
      }
    }

    Vector<PageNode*> unreachable;
    for (const String& ref : result.refs_in_order) {
      if (result.path_by_ref.find(ref) == result.path_by_ref.end())
        unreachable.push_back(result.by_ref.at(ref));
    }
    if (!unreachable.empty()) {
      IdentityProblemData problem;
      problem.kind = "unreachable_nodes";
      problem.nodes = std::move(unreachable);
      result.problems.push_back(std::move(problem));
    }
    HashMap<String, Vector<String>> refs_by_path;
    for (const String& ref : result.refs_in_order) {
      auto path_it = result.path_by_ref.find(ref);
      if (path_it == result.path_by_ref.end())
        continue;
      auto inserted = refs_by_path.insert(path_it->value, Vector<String>());
      inserted.stored_value->value.push_back(ref);
    }
    for (const auto& entry : refs_by_path) {
      if (entry.value.size() <= 1)
        continue;
      result.stats.path_collisions++;
      IdentityProblemData problem;
      problem.kind = "path_collision";
      problem.path = entry.key;
      for (const String& ref : entry.value)
        problem.nodes.push_back(result.by_ref.at(ref));
      result.problems.push_back(std::move(problem));
    }
    for (const String& ref : result.refs_in_order) {
      auto path_it = result.path_by_ref.find(ref);
      if (path_it == result.path_by_ref.end())
        continue;
      result.by_path.Set(path_it->value, result.by_ref.at(ref));
      result.paths.push_back(path_it->value);
    }
    return result;
  };

  PathIndexData before_index = build_path_index(before_snapshot);
  PathIndexData after_index = build_path_index(after_snapshot);

  auto string_value = [](const String& value) -> std::unique_ptr<protocol::Value> {
    return protocol::StringValue::create(value);
  };
  auto null_value = []() -> std::unique_ptr<protocol::Value> {
    return protocol::Value::null();
  };
  auto string_list_value = [](const Vector<String>& values)
      -> std::unique_ptr<protocol::Value> {
    auto list = protocol::ListValue::create();
    for (const String& value : values)
      list->pushValue(protocol::StringValue::create(value));
    return list;
  };

  auto identity_stats_value = [](const IdentityStatsData& stats) {
    return protocol::ChromiumRL::StructuredSnapshotIdentityStats::create()
        .setNodes(stats.nodes)
        .setAnchored_nodes(stats.anchored_nodes)
        .setPositional_nodes(stats.positional_nodes)
        .setDuplicate_anchor_bases(stats.duplicate_anchor_bases)
        .setDuplicate_anchor_nodes(stats.duplicate_anchor_nodes)
        .setPath_collisions(stats.path_collisions)
        .build();
  };
  auto endpoint_value = [&](protocol::ChromiumRL::StructuredPageSnapshot* snapshot) {
    return protocol::ChromiumRL::StructuredSnapshotDiffEndpoint::create()
        .setSnapshot_id(snapshot->getSnapshotId())
        .setDocument_revision(protocol::FundamentalValue::create(
            snapshot->getDocumentRevision()))
        .setUrl(snapshot->getUrl())
        .build();
  };

  auto canonical_pair_sort_key = [&json_quote](
      const std::pair<String, String>& item) -> String {
    return String("{\"name\":") + json_quote(item.first) +
           ",\"value\":" + json_quote(item.second) + "}";
  };
  auto sort_canonical_pairs = [&](Vector<std::pair<String, String>>* pairs) {
    std::sort(pairs->begin(), pairs->end(),
              [&](const auto& left, const auto& right) {
                return python_code_point_less(canonical_pair_sort_key(left),
                                              canonical_pair_sort_key(right));
              });
  };
  auto sort_canonical_strings = [&](Vector<String>* values) {
    std::sort(values->begin(), values->end(),
              [&](const String& left, const String& right) {
                return python_code_point_less(json_quote(left), json_quote(right));
              });
  };
  auto canonical_value = [&](PageNode* node, int field) -> CanonicalValue {
    CanonicalValue value;
    if (field == 0) {
      value.kind = CanonicalValue::Kind::kString;
      value.text = node->getTag();
    } else if (field == 1 && node->hasRole()) {
      value.kind = CanonicalValue::Kind::kString;
      value.text = node->getRole(String());
    } else if (field == 2 && node->hasAccessibleName()) {
      value.kind = CanonicalValue::Kind::kString;
      value.text = node->getAccessibleName(String());
    } else if (field == 3 && node->hasDirectText()) {
      value.kind = CanonicalValue::Kind::kString;
      value.text = node->getDirectText(String());
    } else if (field == 4) {
      value.kind = CanonicalValue::Kind::kAttributes;
      for (const auto& item : *node->getSelectedAttributes())
        value.pairs.push_back(
            std::make_pair(item->getName(), item->getValue()));
      sort_canonical_pairs(&value.pairs);
    } else if (field == 5) {
      value.kind = CanonicalValue::Kind::kStates;
      for (const auto& item : *node->getStates())
        value.pairs.push_back(
            std::make_pair(item->getName(), item->getValue()));
      sort_canonical_pairs(&value.pairs);
    } else if (field == 6) {
      value.kind = CanonicalValue::Kind::kActions;
      for (const String& item : *node->getActionTypes())
        value.strings.push_back(item);
      sort_canonical_strings(&value.strings);
    } else if (field == 7 && node->hasSemanticBoundary()) {
      value.kind = CanonicalValue::Kind::kString;
      value.text = node->getSemanticBoundary(String());
    }
    return value;
  };
  auto canonical_equal = [](const CanonicalValue& left,
                            const CanonicalValue& right) -> bool {
    return left.kind == right.kind && left.text == right.text &&
           left.pairs == right.pairs && left.strings == right.strings;
  };
  auto canonical_empty = [](const CanonicalValue& value) -> bool {
    return value.kind == CanonicalValue::Kind::kNull ||
           (value.kind == CanonicalValue::Kind::kString && value.text.empty()) ||
           ((value.kind == CanonicalValue::Kind::kAttributes ||
             value.kind == CanonicalValue::Kind::kStates) &&
            value.pairs.empty()) ||
           (value.kind == CanonicalValue::Kind::kActions && value.strings.empty());
  };
  auto canonical_protocol_value = [&](const CanonicalValue& value)
      -> std::unique_ptr<protocol::Value> {
    if (value.kind == CanonicalValue::Kind::kNull)
      return null_value();
    if (value.kind == CanonicalValue::Kind::kString)
      return string_value(value.text);
    auto list = protocol::ListValue::create();
    if (value.kind == CanonicalValue::Kind::kActions) {
      for (const String& item : value.strings)
        list->pushValue(protocol::StringValue::create(item));
    } else {
      for (const auto& item : value.pairs) {
        auto object = protocol::DictionaryValue::create();
        object->setString("name", item.first);
        object->setString("value", item.second);
        list->pushValue(std::move(object));
      }
    }
    return list;
  };

  auto meaningful_node = [&clean_text](PageNode* node) -> bool {
    return !clean_text(node->getDirectText(String())).empty() ||
           !clean_text(node->getAccessibleName(String())).empty() ||
           !node->getSelectedAttributes()->empty() || !node->getStates()->empty() ||
           !node->getActionTypes()->empty();
  };

  auto compared_fingerprint = [&](PageNode* node) -> String {
    StringBuilder builder;
    for (int field = 0; field < 8; ++field) {
      CanonicalValue value = canonical_value(node, field);
      if (canonical_empty(value))
        continue;
      builder.Append(String::Number(field));
      builder.Append(':');
      builder.Append(String::Number(static_cast<int>(value.kind)));
      builder.Append(':');
      builder.Append(String::Number(value.text.length()));
      builder.Append(':');
      builder.Append(value.text);
      for (const auto& item : value.pairs) {
        builder.Append('|');
        builder.Append(String::Number(item.first.length()));
        builder.Append(':');
        builder.Append(item.first);
        builder.Append('=');
        builder.Append(String::Number(item.second.length()));
        builder.Append(':');
        builder.Append(item.second);
      }
      for (const String& item : value.strings) {
        builder.Append('|');
        builder.Append(String::Number(item.length()));
        builder.Append(':');
        builder.Append(item);
      }
      builder.Append(';');
    }
    return builder.ToString();
  };

  auto append_compared_node = [&](PageNode* node)
      -> std::unique_ptr<protocol::ChromiumRL::StructuredComparedNode> {
    auto result = protocol::ChromiumRL::StructuredComparedNode::create().build();
    CanonicalValue tag = canonical_value(node, 0);
    CanonicalValue role = canonical_value(node, 1);
    CanonicalValue name = canonical_value(node, 2);
    CanonicalValue direct = canonical_value(node, 3);
    CanonicalValue attrs = canonical_value(node, 4);
    CanonicalValue states = canonical_value(node, 5);
    CanonicalValue actions = canonical_value(node, 6);
    CanonicalValue boundary = canonical_value(node, 7);
    if (!canonical_empty(tag))
      result->setTag(tag.text);
    if (!canonical_empty(role))
      result->setRole(role.text);
    if (!canonical_empty(name))
      result->setAccessibleName(name.text);
    if (!canonical_empty(direct))
      result->setDirectText(direct.text);
    if (!canonical_empty(attrs)) {
      auto values = std::make_unique<protocol::Array<protocol::ChromiumRL::NodeAttribute>>();
      for (const auto& item : attrs.pairs)
        values->push_back(protocol::ChromiumRL::NodeAttribute::create()
                              .setName(item.first).setValue(item.second).build());
      result->setSelectedAttributes(std::move(values));
    }
    if (!canonical_empty(states)) {
      auto values = std::make_unique<protocol::Array<protocol::ChromiumRL::NodeState>>();
      for (const auto& item : states.pairs)
        values->push_back(protocol::ChromiumRL::NodeState::create()
                              .setName(item.first).setValue(item.second).build());
      result->setStates(std::move(values));
    }
    if (!canonical_empty(actions)) {
      auto values = std::make_unique<protocol::Array<String>>();
      for (const String& item : actions.strings)
        values->push_back(item);
      result->setActionTypes(std::move(values));
    }
    if (!canonical_empty(boundary))
      result->setSemanticBoundary(boundary.text);
    return result;
  };

  auto collision_node_value = [&](PageNode* node) {
    Vector<String> child_refs;
    for (const String& ref : *node->getChildRefs())
      child_refs.push_back(ref);
    auto result = protocol::ChromiumRL::StructuredCollisionNode::create()
        .setRef(string_value(node->getRef()))
        .setParentRef(node->hasParentRef()
                          ? string_value(node->getParentRef(String()))
                          : null_value())
        .setChildRefs(string_list_value(child_refs))
        .build();
    auto compared = append_compared_node(node);
    if (compared->hasTag()) result->setTag(compared->getTag(String()));
    if (compared->hasRole()) result->setRole(compared->getRole(String()));
    if (compared->hasAccessibleName())
      result->setAccessibleName(compared->getAccessibleName(String()));
    if (compared->hasDirectText())
      result->setDirectText(compared->getDirectText(String()));
    if (compared->hasSemanticBoundary())
      result->setSemanticBoundary(compared->getSemanticBoundary(String()));
    if (!node->getSelectedAttributes()->empty()) {
      CanonicalValue attrs = canonical_value(node, 4);
      auto values = std::make_unique<protocol::Array<protocol::ChromiumRL::NodeAttribute>>();
      for (const auto& item : attrs.pairs)
        values->push_back(protocol::ChromiumRL::NodeAttribute::create()
                              .setName(item.first).setValue(item.second).build());
      result->setSelectedAttributes(std::move(values));
    }
    if (!node->getStates()->empty()) {
      CanonicalValue states = canonical_value(node, 5);
      auto values = std::make_unique<protocol::Array<protocol::ChromiumRL::NodeState>>();
      for (const auto& item : states.pairs)
        values->push_back(protocol::ChromiumRL::NodeState::create()
                              .setName(item.first).setValue(item.second).build());
      result->setStates(std::move(values));
    }
    if (!node->getActionTypes()->empty()) {
      CanonicalValue actions = canonical_value(node, 6);
      auto values = std::make_unique<protocol::Array<String>>();
      for (const String& item : actions.strings)
        values->push_back(item);
      result->setActionTypes(std::move(values));
    }
    return result;
  };

  if (!before_index.problems.empty() || !after_index.problems.empty()) {
    auto errors = std::make_unique<
        protocol::Array<protocol::ChromiumRL::StructuredIdentityError>>();
    auto append_error = [&](const String& side, const PathIndexData& index) {
      if (index.problems.empty())
        return;
      auto problems = std::make_unique<
          protocol::Array<protocol::ChromiumRL::StructuredIdentityProblem>>();
      for (const IdentityProblemData& data : index.problems) {
        auto nodes = std::make_unique<
            protocol::Array<protocol::ChromiumRL::StructuredCollisionNode>>();
        for (PageNode* node : data.nodes)
          nodes->push_back(collision_node_value(node));
        auto problem = protocol::ChromiumRL::StructuredIdentityProblem::create()
                           .setKind(data.kind)
                           .setNodes(std::move(nodes))
                           .build();
        if (!data.ref.empty()) problem->setRef(data.ref);
        if (!data.parent_ref.empty()) problem->setParent_ref(data.parent_ref);
        if (!data.child_ref.empty()) problem->setChild_ref(data.child_ref);
        if (!data.child_refs.empty()) {
          auto values = std::make_unique<protocol::Array<String>>();
          for (const String& value : data.child_refs) values->push_back(value);
          problem->setChild_refs(std::move(values));
        }
        if (!data.path.empty()) problem->setPath(data.path);
        if (!data.existing_path.empty()) problem->setExisting_path(data.existing_path);
        problems->push_back(std::move(problem));
      }
      errors->push_back(protocol::ChromiumRL::StructuredIdentityError::create()
                            .setSide(side)
                            .setMessage(side +
                                " snapshot cannot be assigned safe unique node paths")
                            .setProblems(std::move(problems))
                            .build());
    };
    append_error("before", before_index);
    append_error("after", after_index);
    auto totals = protocol::ChromiumRL::StructuredDiffCounts::create().build();
    totals->setAdded(0); totals->setRemoved(0); totals->setChanged(0);
    totals->setChanges(0);
    auto emitted = protocol::ChromiumRL::StructuredDiffCounts::create().build();
    emitted->setAdded(0); emitted->setRemoved(0); emitted->setChanged(0);
    emitted->setSemantic_entry_count(0);
    emitted->setStructural_entry_count(0);
    emitted->setChanges(0);
    auto payload = protocol::ChromiumRL::StructuredDiffPayload::create().build();
    payload->setErrors(std::move(errors));
    *out = protocol::ChromiumRL::StructuredSnapshotDiff::create()
        .setSource("runner_snapshot_diff")
        .setInterval("before_snapshot_to_after_snapshot")
        .setStatus("unsafe_node_identity")
        .setAction_type(action_type.has_value() ? string_value(*action_type)
                                                : null_value())
        .setGeometry_excluded(true)
        .setCovers_live_control_state(false)
        .setBefore(endpoint_value(before_snapshot))
        .setAfter(endpoint_value(after_snapshot))
        .setChange_count(0)
        .setTotals(std::move(totals))
        .setEmitted_counts(std::move(emitted))
        .setDiff(std::move(payload))
        .build();
    return protocol::Response::Success();
  }

  auto identity = protocol::ChromiumRL::StructuredSnapshotIdentity::create()
      .setStrategy("own_content_or_semantic_anchor_with_positional_fallback")
      .setBefore(identity_stats_value(before_index.stats))
      .setAfter(identity_stats_value(after_index.stats))
      .build();

  auto url_without_fragment = [](const String& url) -> String {
    unsigned fragment = url.find('#');
    return fragment == kNotFound ? url : url.substr(0, fragment);
  };
  auto canonical_document_url = [&](const String& url) -> String {
    String base = url_without_fragment(url);
    unsigned colon = base.find(':');
    if (colon == kNotFound)
      return base;
    StringBuilder result;
    result.Append(base.substr(0, colon).ToAsciiLower());
    result.Append(':');
    unsigned authority_start = colon + 1;
    if (authority_start + 1 >= base.length() ||
        base[authority_start] != '/' || base[authority_start + 1] != '/') {
      result.Append(base.substr(authority_start));
      return result.ToString();
    }
    result.Append("//");
    authority_start += 2;
    unsigned authority_end = authority_start;
    while (authority_end < base.length() && base[authority_end] != '/' &&
           base[authority_end] != '?') {
      authority_end++;
    }
    result.Append(base.substr(authority_start, authority_end - authority_start)
                      .ToAsciiLower());
    result.Append(base.substr(authority_end));
    return result.ToString();
  };
  auto visible_document_text = [&](protocol::ChromiumRL::StructuredPageSnapshot* snapshot) {
    struct TextRow {
      int order = 0;
      int position = 0;
      String text;
    };
    Vector<TextRow> rows;
    int position = 0;
    for (const auto& owned_node : *snapshot->getNodes()) {
      PageNode* node = owned_node.get();
      if (!node->getVisible()) {
        position++;
        continue;
      }
      String direct = clean_text(node->getDirectText(String()));
      String subtree = clean_text(node->getSubtreeText(String()));
      String text = direct;
      if (node->getTruncated() && !direct.empty() &&
          subtree.length() > direct.length() &&
          subtree.length() <= std::max<wtf_size_t>(1200, direct.length() * 4)) {
        text = subtree;
      }
      if (text.empty()) {
        String role = clean_text(node->getRole(String())).ToAsciiLower();
        String boundary =
            clean_text(node->getSemanticBoundary(String())).ToAsciiLower();
        bool fact_node = node->getChildRefs()->empty() || !role.empty() ||
                         !boundary.empty() || !node->getActionTypes()->empty();
        if (fact_node) {
          String raw_text = node->getAccessibleName(String());
          if (raw_text.empty())
            raw_text = node->getSubtreeText(String());
          text = clean_text(raw_text);
        }
      }
      if (!text.empty()) {
        int order = node->getSourceOrder();
        if (!order)
          order = position;
        rows.push_back(TextRow{order, position, text});
      }
      position++;
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const TextRow& left, const TextRow& right) {
                       return left.order < right.order;
                     });
    HashSet<String> seen;
    Vector<String> result;
    for (const TextRow& row : rows) {
      if (!seen.Contains(row.text)) {
        seen.insert(row.text);
        result.push_back(row.text);
      }
    }
    return result;
  };

  String before_url = before_snapshot->getUrl();
  String after_url = after_snapshot->getUrl();
  if (before_url != after_url &&
      canonical_document_url(before_url) != canonical_document_url(after_url)) {
    Vector<String> before_text = visible_document_text(before_snapshot);
    Vector<String> after_text = visible_document_text(after_snapshot);
    HashSet<String> before_set;
    HashSet<String> after_set;
    for (const String& value : before_text) before_set.insert(value);
    for (const String& value : after_text) after_set.insert(value);
    Vector<String> removed_text;
    Vector<String> added_text;
    for (const String& value : before_text)
      if (!after_set.Contains(value)) removed_text.push_back(value);
    for (const String& value : after_text)
      if (!before_set.Contains(value)) added_text.push_back(value);

    auto removed_values = std::make_unique<protocol::Array<String>>();
    for (const String& value : removed_text) removed_values->push_back(value);
    auto added_values = std::make_unique<protocol::Array<String>>();
    for (const String& value : added_text) added_values->push_back(value);
    auto text_delta = protocol::ChromiumRL::StructuredNavigationTextDelta::create()
        .setRemoved(std::move(removed_values))
        .setAdded(std::move(added_values))
        .setRemoved_total(static_cast<int>(removed_text.size()))
        .setAdded_total(static_cast<int>(added_text.size()))
        .setTruncated(protocol::ChromiumRL::StructuredTruncatedCounts::create()
                          .setRemoved(0).setAdded(0).build())
        .build();
    int before_nodes = static_cast<int>(before_snapshot->getNodes()->size());
    int after_nodes = static_cast<int>(after_snapshot->getNodes()->size());
    auto totals = protocol::ChromiumRL::StructuredDiffCounts::create().build();
    totals->setRemoved_nodes(before_nodes);
    totals->setAdded_nodes(after_nodes);
    totals->setRemoved_text(static_cast<int>(removed_text.size()));
    totals->setAdded_text(static_cast<int>(added_text.size()));
    auto emitted = protocol::ChromiumRL::StructuredDiffCounts::create().build();
    emitted->setRemoved_nodes(0);
    emitted->setAdded_nodes(0);
    emitted->setRemoved_text(static_cast<int>(removed_text.size()));
    emitted->setAdded_text(static_cast<int>(added_text.size()));
    emitted->setSemantic_entry_count(0);
    emitted->setStructural_entry_count(0);
    auto payload = protocol::ChromiumRL::StructuredDiffPayload::create().build();
    payload->setNavigation(protocol::ChromiumRL::StructuredNavigation::create()
                               .setFrom(before_url).setTo(after_url).build());
    payload->setRemoved_node_count(before_nodes);
    payload->setAdded_node_count(after_nodes);
    payload->setText_delta(std::move(text_delta));
    *out = protocol::ChromiumRL::StructuredSnapshotDiff::create()
        .setSource("runner_snapshot_diff")
        .setInterval("before_snapshot_to_after_snapshot")
        .setAction_type(action_type.has_value() ? string_value(*action_type)
                                                : null_value())
        .setGeometry_excluded(true)
        .setCovers_live_control_state(false)
        .setIdentity(std::move(identity))
        .setStatus("document_replaced")
        .setBefore(endpoint_value(before_snapshot))
        .setAfter(endpoint_value(after_snapshot))
        .setChange_count(before_nodes + after_nodes)
        .setTotals(std::move(totals))
        .setEmitted_counts(std::move(emitted))
        .setDiff(std::move(payload))
        .build();
    return protocol::Response::Success();
  }

  Vector<String> before_paths = before_index.paths;
  Vector<String> after_paths = after_index.paths;
  std::sort(before_paths.begin(), before_paths.end(), python_code_point_less);
  std::sort(after_paths.begin(), after_paths.end(), python_code_point_less);
  HashSet<String> before_set;
  HashSet<String> after_set;
  for (const String& path : before_paths) before_set.insert(path);
  for (const String& path : after_paths) after_set.insert(path);
  HashSet<String> added_set;
  HashSet<String> removed_set;
  for (const String& path : after_paths)
    if (!before_set.Contains(path)) added_set.insert(path);
  for (const String& path : before_paths)
    if (!after_set.Contains(path)) removed_set.insert(path);
  int raw_added_before_relocation_match = static_cast<int>(added_set.size());
  int raw_removed_before_relocation_match = static_cast<int>(removed_set.size());

  HashMap<String, Vector<String>> removed_by_fingerprint;
  HashMap<String, Vector<String>> added_by_fingerprint;
  Vector<String> fingerprint_order;
  HashSet<String> fingerprints_seen;
  for (const String& path : before_paths) {
    if (!removed_set.Contains(path) || !meaningful_node(before_index.by_path.at(path)))
      continue;
    String fingerprint = compared_fingerprint(before_index.by_path.at(path));
    auto inserted = removed_by_fingerprint.insert(fingerprint, Vector<String>());
    inserted.stored_value->value.push_back(path);
    if (!fingerprints_seen.Contains(fingerprint)) {
      fingerprints_seen.insert(fingerprint);
      fingerprint_order.push_back(fingerprint);
    }
  }
  for (const String& path : after_paths) {
    if (!added_set.Contains(path) || !meaningful_node(after_index.by_path.at(path)))
      continue;
    String fingerprint = compared_fingerprint(after_index.by_path.at(path));
    added_by_fingerprint.insert(fingerprint, Vector<String>())
        .stored_value->value.push_back(path);
  }
  auto path_parts = [](const String& path) {
    Vector<String> parts;
    unsigned start = 0;
    while (start <= path.length()) {
      unsigned slash = path.find('/', start);
      unsigned end = slash == kNotFound ? path.length() : slash;
      parts.push_back(path.substr(start, end - start));
      if (slash == kNotFound)
        break;
      start = slash + 1;
    }
    return parts;
  };
  struct RelocationCandidate {
    int distance = 0;
    int depth_difference = 0;
    String old_path;
    String new_path;
  };
  int relocated_nodes_suppressed = 0;
  for (const String& fingerprint : fingerprint_order) {
    auto added_it = added_by_fingerprint.find(fingerprint);
    if (added_it == added_by_fingerprint.end())
      continue;
    const Vector<String>& old_paths = removed_by_fingerprint.at(fingerprint);
    const Vector<String>& new_paths = added_it->value;
    Vector<RelocationCandidate> candidates;
    for (const String& old_path : old_paths) {
      Vector<String> old_parts = path_parts(old_path);
      for (const String& new_path : new_paths) {
        Vector<String> new_parts = path_parts(new_path);
        int common = 0;
        while (common < static_cast<int>(old_parts.size()) &&
               common < static_cast<int>(new_parts.size()) &&
               old_parts[common] == new_parts[common]) {
          common++;
        }
        candidates.push_back(RelocationCandidate{
            static_cast<int>(old_parts.size() + new_parts.size()) - 2 * common,
            std::abs(static_cast<int>(old_parts.size()) -
                     static_cast<int>(new_parts.size())),
            old_path, new_path});
      }
    }
    std::sort(candidates.begin(), candidates.end(),
              [&](const RelocationCandidate& left,
                  const RelocationCandidate& right) {
                if (left.distance != right.distance)
                  return left.distance < right.distance;
                if (left.depth_difference != right.depth_difference)
                  return left.depth_difference < right.depth_difference;
                if (left.old_path != right.old_path)
                  return python_code_point_less(left.old_path, right.old_path);
                return python_code_point_less(left.new_path, right.new_path);
              });
    HashSet<String> paired_old;
    HashSet<String> paired_new;
    int pair_limit = static_cast<int>(std::min(old_paths.size(), new_paths.size()));
    for (const RelocationCandidate& candidate : candidates) {
      if (paired_old.Contains(candidate.old_path) ||
          paired_new.Contains(candidate.new_path)) {
        continue;
      }
      paired_old.insert(candidate.old_path);
      paired_new.insert(candidate.new_path);
      removed_set.erase(candidate.old_path);
      added_set.erase(candidate.new_path);
      relocated_nodes_suppressed++;
      if (static_cast<int>(paired_old.size()) >= pair_limit)
        break;
    }
  }

  auto field_name = [](int field) -> String {
    switch (field) {
      case 0: return "tag";
      case 1: return "role";
      case 2: return "accessibleName";
      case 3: return "directText";
      case 4: return "selectedAttributes";
      case 5: return "states";
      case 6: return "actionTypes";
      case 7: return "semanticBoundary";
      default: return String();
    }
  };
  Vector<DiffEntryData> changed_entries;
  int subtree_text_changes_ignored = 0;
  for (const String& path : before_paths) {
    if (!after_set.Contains(path))
      continue;
    PageNode* before_node = before_index.by_path.at(path);
    PageNode* after_node = after_index.by_path.at(path);
    DiffEntryData entry;
    entry.path = path;
    entry.semantic = meaningful_node(before_node) || meaningful_node(after_node);
    for (int field = 0; field < 8; ++field) {
      CanonicalValue before_value = canonical_value(before_node, field);
      CanonicalValue after_value = canonical_value(after_node, field);
      if (!canonical_equal(before_value, after_value)) {
        entry.fields.push_back(
            FieldChangeData{field, std::move(before_value), std::move(after_value)});
      }
    }
    if (!entry.fields.empty()) {
      if (after_node->hasRepeatedGroupId()) {
        entry.repeated_group_id = after_node->getRepeatedGroupId(String());
        if (after_node->hasRepeatedItemIndex())
          entry.repeated_item_index = after_node->getRepeatedItemIndex(0);
      }
      changed_entries.push_back(std::move(entry));
    } else if (clean_text(before_node->getSubtreeText(String())) !=
               clean_text(after_node->getSubtreeText(String()))) {
      subtree_text_changes_ignored++;
    }
  }

  auto make_node_entries = [&](const Vector<String>& sorted_paths,
                               const HashSet<String>& selected,
                               const PathIndexData& index) {
    Vector<DiffEntryData> entries;
    for (const String& path : sorted_paths) {
      if (!selected.Contains(path))
        continue;
      PageNode* node = index.by_path.at(path);
      DiffEntryData entry;
      entry.path = path;
      entry.semantic = meaningful_node(node);
      entry.node = node;
      if (node->hasRepeatedGroupId()) {
        entry.repeated_group_id = node->getRepeatedGroupId(String());
        if (node->hasRepeatedItemIndex())
          entry.repeated_item_index = node->getRepeatedItemIndex(0);
      }
      entries.push_back(std::move(entry));
    }
    return entries;
  };
  Vector<DiffEntryData> added_entries =
      make_node_entries(after_paths, added_set, after_index);
  Vector<DiffEntryData> removed_entries =
      make_node_entries(before_paths, removed_set, before_index);

  auto last_slash = [](const String& path) -> unsigned {
    unsigned result = kNotFound;
    unsigned start = 0;
    while (start < path.length()) {
      unsigned found = path.find('/', start);
      if (found == kNotFound)
        break;
      result = found;
      start = found + 1;
    }
    return result;
  };
  auto build_subtree_indexes = [&](const String& operation,
                                   const Vector<String>& sorted_paths,
                                   const HashSet<String>& selected,
                                   const PathIndexData& index,
                                   int* indexed_descendants) {
    HashMap<String, Vector<String>> children;
    for (const String& path : sorted_paths)
      children.Set(path, Vector<String>());
    for (const String& path : sorted_paths) {
      unsigned slash = last_slash(path);
      if (slash == kNotFound)
        continue;
      String parent = path.substr(0, slash);
      auto parent_it = children.find(parent);
      if (parent_it != children.end())
        parent_it->value.push_back(path);
    }
    for (auto& item : children) {
      std::sort(item.value.begin(), item.value.end(), python_code_point_less);
    }
    Vector<String> roots;
    for (const String& path : sorted_paths) {
      if (!selected.Contains(path))
        continue;
      unsigned slash = last_slash(path);
      String parent = slash == kNotFound ? String() : path.substr(0, slash);
      if (slash == kNotFound || !selected.Contains(parent))
        roots.push_back(path);
    }
    Vector<SubtreeIndexData> indexes;
    Vector<String> pending;
    for (wtf_size_t i = roots.size(); i > 0; --i)
      pending.push_back(roots[i - 1]);
    int document_nodes = std::max(1, static_cast<int>(index.by_path.size()));
    while (!pending.empty()) {
      String root = pending.back();
      pending.pop_back();
      Vector<String> members;
      Vector<String> member_stack;
      member_stack.push_back(root);
      while (!member_stack.empty()) {
        String path = member_stack.back();
        member_stack.pop_back();
        if (!selected.Contains(path))
          continue;
        members.push_back(path);
        const Vector<String>& path_children = children.at(path);
        for (wtf_size_t i = path_children.size(); i > 0; --i)
          member_stack.push_back(path_children[i - 1]);
      }
      double share = static_cast<double>(members.size()) / document_nodes;
      Vector<String> selected_children;
      for (const String& child : children.at(root))
        if (selected.Contains(child)) selected_children.push_back(child);
      if (share > 0.60 && !selected_children.empty()) {
        for (wtf_size_t i = selected_children.size(); i > 0; --i)
          pending.push_back(selected_children[i - 1]);
        continue;
      }
      if (members.size() == 1)
        continue;
      SubtreeIndexData subtree;
      subtree.operation = operation;
      subtree.root_path = root;
      subtree.member_paths = members;
      subtree.descendant_count = static_cast<int>(members.size()) - 1;
      subtree.subtree_node_count = static_cast<int>(members.size());
      subtree.document_percent =
          std::nearbyint(share * 10000.0) / 100.0;
      PageNode* root_node = index.by_path.at(root);
      String raw_visible_text = root_node->getSubtreeText(String());
      if (raw_visible_text.empty())
        raw_visible_text = root_node->getDirectText(String());
      if (raw_visible_text.empty())
        raw_visible_text = root_node->getAccessibleName(String());
      subtree.visible_text = clean_text(raw_visible_text);
      if (subtree.visible_text.empty()) {
        Vector<String> fragments;
        for (const String& path : members) {
          PageNode* node = index.by_path.at(path);
          String raw_fragment = node->getDirectText(String());
          if (raw_fragment.empty())
            raw_fragment = node->getAccessibleName(String());
          String fragment = clean_text(raw_fragment);
          if (!fragment.empty() && !contains_string(fragments, fragment))
            fragments.push_back(fragment);
        }
        StringBuilder joined;
        for (wtf_size_t i = 0; i < fragments.size(); ++i) {
          if (i) joined.Append(" | ");
          joined.Append(fragments[i]);
        }
        subtree.visible_text = joined.ToString();
      }
      for (const String& path : members) {
        if (!index.by_path.at(path)->getActionTypes()->empty())
          subtree.interactive_paths.push_back(path);
      }
      *indexed_descendants += subtree.descendant_count;
      indexes.push_back(std::move(subtree));
    }
    return indexes;
  };
  int added_indexed_descendants = 0;
  int removed_indexed_descendants = 0;
  Vector<SubtreeIndexData> subtree_indexes = build_subtree_indexes(
      "added", after_paths, added_set, after_index, &added_indexed_descendants);
  Vector<SubtreeIndexData> removed_subtree_indexes = build_subtree_indexes(
      "removed", before_paths, removed_set, before_index,
      &removed_indexed_descendants);
  for (SubtreeIndexData& item : removed_subtree_indexes)
    subtree_indexes.push_back(std::move(item));

  auto repeated_signature_key = [&](const DiffEntryData& entry) -> String {
    StringBuilder builder;
    builder.Append("node|");
    if (entry.node) {
      CanonicalValue tag = canonical_value(entry.node, 0);
      CanonicalValue role = canonical_value(entry.node, 1);
      if (!canonical_empty(tag)) builder.Append(tag.text);
      builder.Append('|');
      if (!canonical_empty(role)) builder.Append(role.text);
    } else {
      builder.Append('|');
      Vector<String> names;
      for (const FieldChangeData& field : entry.fields) {
        String name = field_name(field.field);
        if (!name.empty()) names.push_back(name);
      }
      std::sort(names.begin(), names.end(), python_code_point_less);
      for (const String& name : names) {
        builder.Append('|');
        builder.Append(name);
      }
    }
    return builder.ToString();
  };
  auto build_repeated_indexes = [&](const String& operation,
                                    const Vector<DiffEntryData>& entries) {
    HashMap<String, Vector<int>> buckets;
    for (wtf_size_t i = 0; i < entries.size(); ++i) {
      const DiffEntryData& entry = entries[i];
      if (entry.repeated_group_id.empty())
        continue;
      String key = entry.repeated_group_id + String("\x1f") +
                   repeated_signature_key(entry);
      buckets.insert(key, Vector<int>()).stored_value->value.push_back(i);
    }
    Vector<bool> consumed;
    for (wtf_size_t i = 0; i < entries.size(); ++i)
      consumed.push_back(false);
    Vector<RepeatedGroupIndexData> indexes;
    for (wtf_size_t i = 0; i < entries.size(); ++i) {
      if (consumed[i])
        continue;
      const DiffEntryData& entry = entries[i];
      if (entry.repeated_group_id.empty())
        continue;
      String key = entry.repeated_group_id + String("\x1f") +
                   repeated_signature_key(entry);
      const Vector<int>& bucket = buckets.at(key);
      if (bucket.size() < 2)
        continue;
      RepeatedGroupIndexData index_data;
      index_data.operation = operation;
      index_data.repeated_group_id = entry.repeated_group_id;
      index_data.count = static_cast<int>(bucket.size());
      if (entry.node) {
        CanonicalValue tag = canonical_value(entry.node, 0);
        CanonicalValue role = canonical_value(entry.node, 1);
        if (!canonical_empty(tag)) index_data.tag = tag.text;
        if (!canonical_empty(role)) index_data.role = role.text;
      } else {
        for (const FieldChangeData& field : entry.fields) {
          String name = field_name(field.field);
          if (!name.empty()) index_data.changed_fields.push_back(name);
        }
        std::sort(index_data.changed_fields.begin(),
                  index_data.changed_fields.end(), python_code_point_less);
      }
      for (int member : bucket) {
        consumed[static_cast<wtf_size_t>(member)] = true;
        const DiffEntryData& member_entry = entries[member];
        if (member_entry.repeated_item_index.has_value())
          index_data.item_indices.push_back(*member_entry.repeated_item_index);
        index_data.member_paths.push_back(member_entry.path);
      }
      indexes.push_back(std::move(index_data));
    }
    return indexes;
  };
  Vector<RepeatedGroupIndexData> repeated_indexes =
      build_repeated_indexes("added", added_entries);
  Vector<RepeatedGroupIndexData> removed_repeated =
      build_repeated_indexes("removed", removed_entries);
  Vector<RepeatedGroupIndexData> changed_repeated =
      build_repeated_indexes("changed", changed_entries);
  for (RepeatedGroupIndexData& item : removed_repeated)
    repeated_indexes.push_back(std::move(item));
  for (RepeatedGroupIndexData& item : changed_repeated)
    repeated_indexes.push_back(std::move(item));

  struct ViewportTextRow {
    int order = 0;
    String text;
  };
  struct MovementCluster {
    double dx = 0;
    double dy = 0;
    int count = 0;
  };
  int entered_nodes = 0;
  int exited_nodes = 0;
  int in_viewport_changed = 0;
  int hit_testable_changed = 0;
  int geometry_compared = 0;
  int shifted_nodes = 0;
  int dimension_changed_nodes = 0;
  Vector<ViewportTextRow> entered_rows;
  Vector<ViewportTextRow> exited_rows;
  Vector<MovementCluster> movement_clusters;
  auto viewport_membership = [](PageNode* node) -> bool {
    if (!node->getVisible())
      return false;
    return node->getInViewport() || node->getHitTestable();
  };
  auto viewport_node_text = [&clean_text](PageNode* node) -> String {
    String text = clean_text(node->getDirectText(String()));
    if (!text.empty())
      return text;
    String role = clean_text(node->getRole(String())).ToAsciiLower();
    String boundary =
        clean_text(node->getSemanticBoundary(String())).ToAsciiLower();
    bool semantic = !role.empty() || !boundary.empty() ||
                    !node->getActionTypes()->empty() ||
                    node->getChildRefs()->empty();
    return semantic ? clean_text(node->getAccessibleName(String())) : String();
  };
  auto empty_bounds = protocol::DOM::Rect::create()
                          .setX(0).setY(0).setWidth(0).setHeight(0).build();
  int fallback_order = 0;
  for (const String& path : before_paths) {
    if (!after_set.Contains(path))
      continue;
    PageNode* before_node = before_index.by_path.at(path);
    PageNode* after_node = after_index.by_path.at(path);
    bool before_member = viewport_membership(before_node);
    bool after_member = viewport_membership(after_node);
    if (before_node->getInViewport() != after_node->getInViewport())
      in_viewport_changed++;
    if (before_node->getHitTestable() != after_node->getHitTestable())
      hit_testable_changed++;
    if (before_member != after_member) {
      PageNode* selected = after_member ? after_node : before_node;
      String text = viewport_node_text(selected);
      int order = selected->getSourceOrder();
      if (!order)
        order = fallback_order;
      if (after_member) {
        entered_nodes++;
        if (!text.empty()) entered_rows.push_back(ViewportTextRow{order, text});
      } else {
        exited_nodes++;
        if (!text.empty()) exited_rows.push_back(ViewportTextRow{order, text});
      }
    }
    if (before_node->hasBounds() && after_node->hasBounds()) {
      protocol::DOM::Rect* before_bounds = before_node->getBounds(empty_bounds.get());
      protocol::DOM::Rect* after_bounds = after_node->getBounds(empty_bounds.get());
      geometry_compared++;
      double dx = std::nearbyint((after_bounds->getX() - before_bounds->getX()) * 10.0) / 10.0;
      double dy = std::nearbyint((after_bounds->getY() - before_bounds->getY()) * 10.0) / 10.0;
      if (std::abs(dx) >= 0.5 || std::abs(dy) >= 0.5) {
        shifted_nodes++;
        bool found = false;
        for (MovementCluster& cluster : movement_clusters) {
          if (cluster.dx == dx && cluster.dy == dy) {
            cluster.count++;
            found = true;
            break;
          }
        }
        if (!found)
          movement_clusters.push_back(MovementCluster{dx, dy, 1});
      }
      if (std::abs(after_bounds->getWidth() - before_bounds->getWidth()) >= 0.5 ||
          std::abs(after_bounds->getHeight() - before_bounds->getHeight()) >= 0.5) {
        dimension_changed_nodes++;
      }
    }
    fallback_order++;
  }
  auto unique_viewport_text = [&](Vector<ViewportTextRow> rows) {
    std::sort(rows.begin(), rows.end(),
              [&](const ViewportTextRow& left, const ViewportTextRow& right) {
                if (left.order != right.order)
                  return left.order < right.order;
                return python_code_point_less(left.text, right.text);
              });
    HashSet<String> seen;
    Vector<String> values;
    for (const ViewportTextRow& row : rows) {
      String key = fold_case(clean_text(row.text));
      if (!key.empty() && !seen.Contains(key)) {
        seen.insert(key);
        values.push_back(row.text);
      }
    }
    return values;
  };
  Vector<String> entered_text = unique_viewport_text(std::move(entered_rows));
  Vector<String> exited_text = unique_viewport_text(std::move(exited_rows));
  bool has_viewport_delta = entered_nodes || exited_nodes ||
      in_viewport_changed || hit_testable_changed || shifted_nodes ||
      dimension_changed_nodes;
  if (!movement_clusters.empty()) {
    std::sort(movement_clusters.begin(), movement_clusters.end(),
              [](const MovementCluster& left, const MovementCluster& right) {
                if (left.count != right.count)
                  return left.count > right.count;
                double left_magnitude = std::abs(left.dx) + std::abs(left.dy);
                double right_magnitude = std::abs(right.dx) + std::abs(right.dy);
                if (left_magnitude != right_magnitude)
                  return left_magnitude > right_magnitude;
                if (left.dx != right.dx)
                  return left.dx < right.dx;
                return left.dy < right.dy;
              });
  }

  auto canonical_fragment = [&](const CanonicalValue& value) -> String {
    if (value.kind == CanonicalValue::Kind::kNull)
      return String();
    if (value.kind == CanonicalValue::Kind::kString)
      return value.text.empty() ? String() : clean_text(json_quote(value.text));
    if (value.kind == CanonicalValue::Kind::kActions && value.strings.empty())
      return String();
    if ((value.kind == CanonicalValue::Kind::kAttributes ||
         value.kind == CanonicalValue::Kind::kStates) &&
        value.pairs.empty()) {
      return String();
    }
    StringBuilder builder;
    builder.Append('[');
    if (value.kind == CanonicalValue::Kind::kActions) {
      for (wtf_size_t i = 0; i < value.strings.size(); ++i) {
        if (i) builder.Append(", ");
        builder.Append(json_quote(value.strings[i]));
      }
    } else {
      for (wtf_size_t i = 0; i < value.pairs.size(); ++i) {
        if (i) builder.Append(", ");
        builder.Append("{\"name\": ");
        builder.Append(json_quote(value.pairs[i].first));
        builder.Append(", \"value\": ");
        builder.Append(json_quote(value.pairs[i].second));
        builder.Append('}');
      }
    }
    builder.Append(']');
    return clean_text(builder.ToString());
  };
  auto entry_fragment = [&](const DiffEntryData& entry) -> String {
    Vector<String> candidates;
    if (entry.node) {
      candidates.push_back(clean_text(entry.node->getDirectText(String())));
      candidates.push_back(clean_text(entry.node->getAccessibleName(String())));
    } else {
      const int priority_fields[] = {3, 2, 4, 5, 6};
      for (int wanted : priority_fields) {
        for (const FieldChangeData& change : entry.fields) {
          if (change.field != wanted)
            continue;
          String after = canonical_fragment(change.after);
          String before = canonical_fragment(change.before);
          if (!after.empty()) candidates.push_back(after);
          if (!before.empty()) candidates.push_back(before);
        }
      }
    }
    String fragment;
    for (const String& candidate : candidates) {
      if (!candidate.empty()) {
        fragment = candidate;
        break;
      }
    }
    if (fragment.length() > 180) {
      StringBuilder truncated_fragment;
      truncated_fragment.Append(
          rstrip_python_whitespace(fragment.substr(0, 179)));
      truncated_fragment.Append(static_cast<UChar>(0x2026));
      fragment = truncated_fragment.ToString();
    }
    return fragment;
  };
  auto code_point_at = [](const String& text, wtf_size_t offset,
                          wtf_size_t* next) -> UChar32 {
    UChar32 result = text[offset++];
    if (U16_IS_LEAD(result) && offset < text.length() &&
        U16_IS_TRAIL(text[offset])) {
      result = U16_GET_SUPPLEMENTARY(result, text[offset++]);
    }
    *next = offset;
    return result;
  };
  auto code_point_before = [](const String& text, wtf_size_t offset) -> UChar32 {
    UChar32 trail = text[--offset];
    if (U16_IS_TRAIL(trail) && offset && U16_IS_LEAD(text[offset - 1]))
      return U16_GET_SUPPLEMENTARY(text[offset - 1], trail);
    return trail;
  };
  auto is_python_word = [](UChar32 c) -> bool {
    int8_t category = u_charType(c);
    return c == '_' || u_isalpha(c) ||
           category == U_DECIMAL_DIGIT_NUMBER ||
           category == U_LETTER_NUMBER || category == U_OTHER_NUMBER;
  };
  auto is_python_decimal = [](UChar32 c) -> bool {
    return u_charType(c) == U_DECIMAL_DIGIT_NUMBER;
  };
  auto contains_numeric = [&](const String& text) -> bool {
    for (wtf_size_t start = 0; start < text.length();) {
      wtf_size_t first_next = start;
      UChar32 first = code_point_at(text, start, &first_next);
      wtf_size_t digit_start = start;
      if ((first == '+' || first == '-') && first_next < text.length()) {
        digit_start = first_next;
        wtf_size_t after_sign = digit_start;
        first = code_point_at(text, digit_start, &after_sign);
        if (!is_python_decimal(first)) {
          start = first_next;
          continue;
        }
      } else if (!is_python_decimal(first)) {
        start = first_next;
        continue;
      }
      if (start && is_python_word(code_point_before(text, start))) {
        start = first_next;
        continue;
      }
      wtf_size_t cursor = digit_start;
      while (cursor < text.length()) {
        wtf_size_t next = cursor;
        UChar32 digit = code_point_at(text, cursor, &next);
        if (!is_python_decimal(digit))
          break;
        cursor = next;
      }
      if (cursor == text.length())
        return true;
      wtf_size_t after_integer = cursor;
      UChar32 following = code_point_at(text, cursor, &after_integer);
      if (!is_python_word(following))
        return true;
      start = first_next;
    }
    return false;
  };
  auto entry_has_field = [](const DiffEntryData& entry, int field) -> bool {
    for (const FieldChangeData& change : entry.fields)
      if (change.field == field) return true;
    return false;
  };
  struct EntrySortKey {
    wtf_size_t index = 0;
    int score = 0;
    bool has_text = false;
    bool has_numeric = false;
    bool has_state_or_action = false;
    String path;
  };
  auto sort_entries = [&](Vector<DiffEntryData>* entries) {
    Vector<EntrySortKey> keys;
    for (wtf_size_t i = 0; i < entries->size(); ++i) {
      const DiffEntryData& entry = (*entries)[i];
      String fragment = entry_fragment(entry);
      bool has_action = (entry.node && !entry.node->getActionTypes()->empty()) ||
                        entry_has_field(entry, 6);
      bool has_state = (entry.node && !entry.node->getStates()->empty()) ||
                       entry_has_field(entry, 5);
      bool has_numeric = contains_numeric(fragment);
      bool has_text = !fragment.empty();
      int score = (has_action ? 8 : 0) + (has_state ? 6 : 0) +
                  (has_numeric ? 4 : 0) + (has_text ? 2 : 0);
      keys.push_back(EntrySortKey{i, score, has_text, has_numeric,
                                  has_state || has_action, entry.path});
    }
    std::stable_sort(keys.begin(), keys.end(),
                     [&](const EntrySortKey& left, const EntrySortKey& right) {
                       if (left.score != right.score)
                         return left.score > right.score;
                       if (left.has_text != right.has_text)
                         return left.has_text;
                       if (left.has_numeric != right.has_numeric)
                         return left.has_numeric;
                       if (left.has_state_or_action != right.has_state_or_action)
                         return left.has_state_or_action;
                       return python_code_point_less(left.path, right.path);
                     });
    Vector<DiffEntryData> ordered;
    for (const EntrySortKey& key : keys)
      ordered.push_back(std::move((*entries)[key.index]));
    *entries = std::move(ordered);
  };
  sort_entries(&added_entries);
  sort_entries(&removed_entries);
  sort_entries(&changed_entries);

  auto diff_entry_value = [&](const DiffEntryData& data) {
    auto entry = protocol::ChromiumRL::StructuredDiffEntry::create()
                     .setKind("node")
                     .setPath(data.path)
                     .setSemantic(data.semantic)
                     .build();
    if (data.node)
      entry->setNode(append_compared_node(data.node));
    if (!data.fields.empty()) {
      auto fields = protocol::ChromiumRL::StructuredFieldChanges::create().build();
      for (const FieldChangeData& data_field : data.fields) {
        auto change = protocol::ChromiumRL::StructuredValueChange::create()
                          .setBefore(canonical_protocol_value(data_field.before))
                          .setAfter(canonical_protocol_value(data_field.after))
                          .build();
        switch (data_field.field) {
          case 0: fields->setTag(std::move(change)); break;
          case 1: fields->setRole(std::move(change)); break;
          case 2: fields->setAccessibleName(std::move(change)); break;
          case 3: fields->setDirectText(std::move(change)); break;
          case 4: fields->setSelectedAttributes(std::move(change)); break;
          case 5: fields->setStates(std::move(change)); break;
          case 6: fields->setActionTypes(std::move(change)); break;
          case 7: fields->setSemanticBoundary(std::move(change)); break;
        }
      }
      entry->setFields(std::move(fields));
    }
    if (!data.repeated_group_id.empty())
      entry->setRepeated_group_id(data.repeated_group_id);
    if (data.repeated_item_index.has_value())
      entry->setRepeated_item_index(*data.repeated_item_index);
    return entry;
  };
  auto added_values = std::make_unique<
      protocol::Array<protocol::ChromiumRL::StructuredDiffEntry>>();
  auto removed_values = std::make_unique<
      protocol::Array<protocol::ChromiumRL::StructuredDiffEntry>>();
  auto changed_values = std::make_unique<
      protocol::Array<protocol::ChromiumRL::StructuredDiffEntry>>();
  for (const DiffEntryData& entry : added_entries)
    added_values->push_back(diff_entry_value(entry));
  for (const DiffEntryData& entry : removed_entries)
    removed_values->push_back(diff_entry_value(entry));
  for (const DiffEntryData& entry : changed_entries)
    changed_values->push_back(diff_entry_value(entry));

  auto subtree_values = std::make_unique<
      protocol::Array<protocol::ChromiumRL::StructuredSubtreeIndex>>();
  double max_collapse_percent = 0;
  Vector<String> collapse_root_summaries;
  auto python_float_string = [](double value) -> String {
    String result = String::Number(value);
    if (std::floor(value) == value)
      result = result + ".0";
    return result;
  };
  for (const SubtreeIndexData& data : subtree_indexes) {
    auto members = std::make_unique<protocol::Array<String>>();
    for (const String& path : data.member_paths) members->push_back(path);
    auto interactive = std::make_unique<protocol::Array<String>>();
    for (const String& path : data.interactive_paths) interactive->push_back(path);
    subtree_values->push_back(protocol::ChromiumRL::StructuredSubtreeIndex::create()
        .setOperation(data.operation)
        .setRoot_path(data.root_path)
        .setMember_paths(std::move(members))
        .setDescendant_count(data.descendant_count)
        .setSubtree_node_count(data.subtree_node_count)
        .setDocument_percent(data.document_percent)
        .setVisible_text(data.visible_text)
        .setInteractive_paths(std::move(interactive))
        .build());
    max_collapse_percent = std::max(max_collapse_percent, data.document_percent);
    collapse_root_summaries.push_back(
        "operation=" + data.operation + " path=" + data.root_path +
        " descendant_count=" + String::Number(data.descendant_count) +
        " document_percent=" + python_float_string(data.document_percent));
  }
  auto repeated_values = std::make_unique<
      protocol::Array<protocol::ChromiumRL::StructuredRepeatedGroupIndex>>();
  int repeated_group_members_indexed = 0;
  for (const RepeatedGroupIndexData& data : repeated_indexes) {
    auto changed_fields = std::make_unique<protocol::Array<String>>();
    for (const String& field : data.changed_fields) changed_fields->push_back(field);
    auto signature = protocol::ChromiumRL::StructuredRepeatedSignature::create()
        .setChanged_fields(std::move(changed_fields))
        .setKind("node")
        .setRole(data.role.empty() ? null_value() : string_value(data.role))
        .setTag(data.tag.empty() ? null_value() : string_value(data.tag))
        .build();
    auto indices = std::make_unique<protocol::Array<int>>();
    for (int value : data.item_indices) indices->push_back(value);
    auto paths = std::make_unique<protocol::Array<String>>();
    for (const String& path : data.member_paths) paths->push_back(path);
    repeated_values->push_back(
        protocol::ChromiumRL::StructuredRepeatedGroupIndex::create()
            .setOperation(data.operation)
            .setRepeated_group_id(data.repeated_group_id)
            .setSignature(std::move(signature))
            .setCount(data.count)
            .setItem_indices(std::move(indices))
            .setMember_paths(std::move(paths))
            .build());
    repeated_group_members_indexed += data.count;
  }
  auto indexes_value = protocol::ChromiumRL::StructuredDiffIndexes::create()
      .setSubtrees(std::move(subtree_values))
      .setRepeated_groups(std::move(repeated_values))
      .build();
  auto payload = protocol::ChromiumRL::StructuredDiffPayload::create().build();
  payload->setAdded(std::move(added_values));
  payload->setRemoved(std::move(removed_values));
  payload->setChanged(std::move(changed_values));
  payload->setIndexes(std::move(indexes_value));

  int viewport_change_count =
      static_cast<int>(entered_text.size() + exited_text.size());
  if (has_viewport_delta) {
    auto entered_values = std::make_unique<protocol::Array<String>>();
    for (const String& value : entered_text) entered_values->push_back(value);
    auto exited_values = std::make_unique<protocol::Array<String>>();
    for (const String& value : exited_text) exited_values->push_back(value);
    std::unique_ptr<protocol::Value> dominant = null_value();
    if (!movement_clusters.empty()) {
      const MovementCluster& cluster = movement_clusters.front();
      auto dominant_value = protocol::DictionaryValue::create();
      dominant_value->setDouble("delta_x", cluster.dx);
      dominant_value->setDouble("delta_y", cluster.dy);
      dominant_value->setInteger("matched_nodes", cluster.count);
      dominant_value->setDouble(
          "share_of_shifted_nodes_percent",
          std::nearbyint((static_cast<double>(cluster.count) / shifted_nodes) *
                         10000.0) / 100.0);
      dominant = std::move(dominant_value);
    }
    auto viewport = protocol::ChromiumRL::StructuredViewportDelta::create()
        .setAggregation("structured_snapshot_viewport_flags_and_dominant_geometry_shift")
        .setViewport_state(protocol::ChromiumRL::StructuredViewportState::create()
            .setEntered_nodes(entered_nodes)
            .setExited_nodes(exited_nodes)
            .setIn_viewport_changed_nodes(in_viewport_changed)
            .setHit_testable_changed_nodes(hit_testable_changed)
            .setEntered_text_total(static_cast<int>(entered_text.size()))
            .setExited_text_total(static_cast<int>(exited_text.size()))
            .build())
        .setGeometry(protocol::ChromiumRL::StructuredViewportGeometry::create()
            .setCompared_nodes(geometry_compared)
            .setShifted_nodes(shifted_nodes)
            .setStationary_nodes(std::max(0, geometry_compared - shifted_nodes))
            .setDimension_changed_nodes(dimension_changed_nodes)
            .setMovement_clusters_total(static_cast<int>(movement_clusters.size()))
            .setDominant_shift(std::move(dominant))
            .setPer_node_geometry_emitted(false)
            .build())
        .setVisible_text_entered(std::move(entered_values))
        .setVisible_text_exited(std::move(exited_values))
        .build();
    payload->setViewport_delta(std::move(viewport));
  }

  int semantic_changes = static_cast<int>(added_set.size() + removed_set.size() +
                                           changed_entries.size());
  int total_changes = semantic_changes + viewport_change_count;
  int semantic_entry_count = 0;
  int structural_entry_count = 0;
  auto count_semantics = [&](const Vector<DiffEntryData>& entries) {
    for (const DiffEntryData& entry : entries) {
      if (entry.semantic) semantic_entry_count++;
      else structural_entry_count++;
    }
  };
  count_semantics(added_entries);
  count_semantics(removed_entries);
  count_semantics(changed_entries);
  String status = semantic_changes ? "changes_present" : "no_dom_change";
  if (!semantic_changes && viewport_change_count)
    status = "viewport_content_changed";
  else if (!total_changes && action_type.has_value() &&
           action_type->ToAsciiLower() == "scroll")
    status = "no_semantic_change_scroll";

  auto totals = protocol::ChromiumRL::StructuredDiffCounts::create().build();
  totals->setAdded(static_cast<int>(added_set.size()));
  totals->setRemoved(static_cast<int>(removed_set.size()));
  totals->setChanged(static_cast<int>(changed_entries.size()));
  totals->setSemantic_changes(semantic_changes);
  totals->setViewport_text_changed(viewport_change_count);
  totals->setChanges(total_changes);
  auto emitted = protocol::ChromiumRL::StructuredDiffCounts::create().build();
  emitted->setAdded(static_cast<int>(added_entries.size()));
  emitted->setRemoved(static_cast<int>(removed_entries.size()));
  emitted->setChanged(static_cast<int>(changed_entries.size()));
  emitted->setSemantic_entry_count(semantic_entry_count);
  emitted->setStructural_entry_count(structural_entry_count);
  emitted->setViewport_text_changed(viewport_change_count);
  emitted->setChanges(static_cast<int>(added_entries.size() +
      removed_entries.size() + changed_entries.size()) + viewport_change_count);
  auto emitted_before = protocol::ChromiumRL::StructuredOperationCounts::create()
      .setAdded(static_cast<int>(added_entries.size()))
      .setRemoved(static_cast<int>(removed_entries.size()))
      .setChanged(static_cast<int>(changed_entries.size()))
      .build();
  auto entries_truncated = protocol::ChromiumRL::StructuredOperationCounts::create()
      .setAdded(0).setRemoved(0).setChanged(0).build();
  auto dropped_entries = std::make_unique<
      protocol::Array<protocol::ChromiumRL::StructuredDroppedEntry>>();
  auto collapse_roots = std::make_unique<protocol::Array<String>>();
  for (const String& value : collapse_root_summaries)
    collapse_roots->push_back(value);
  auto compression = protocol::ChromiumRL::StructuredDiffCompression::create()
      .setSubtree_text_changes_ignored(subtree_text_changes_ignored)
      .setRaw_added_before_relocation_match(raw_added_before_relocation_match)
      .setRaw_removed_before_relocation_match(raw_removed_before_relocation_match)
      .setRelocated_nodes_suppressed(relocated_nodes_suppressed)
      .setRelocation_matching("structural_nearest_within_semantic_fingerprint")
      .setNoise_nodes_skipped(0)
      .setCollapsed_descendants(0)
      .setSubtree_indexed_descendants(added_indexed_descendants +
                                      removed_indexed_descendants)
      .setRepeated_group_members_condensed(0)
      .setRepeated_group_members_indexed(repeated_group_members_indexed)
      .setEmitted_before_truncation(std::move(emitted_before))
      .setEntries_truncated(std::move(entries_truncated))
      .setEntry_limit(null_value())
      .setTruncation_selection("none_all_compared_entries_emitted")
      .setDropped_entries(std::move(dropped_entries))
      .setMax_collapse_document_percent(max_collapse_percent)
      .setCollapse_roots(std::move(collapse_roots))
      .build();

  auto result = protocol::ChromiumRL::StructuredSnapshotDiff::create()
      .setSource("runner_snapshot_diff")
      .setInterval("before_snapshot_to_after_snapshot")
      .setAction_type(action_type.has_value() ? string_value(*action_type)
                                              : null_value())
      .setGeometry_excluded(true)
      .setCovers_live_control_state(false)
      .setIdentity(std::move(identity))
      .setStatus(status)
      .setBefore(endpoint_value(before_snapshot))
      .setAfter(endpoint_value(after_snapshot))
      .setChange_count(total_changes)
      .setSemantic_change_count(semantic_changes)
      .setViewport_change_count(viewport_change_count)
      .setTotals(std::move(totals))
      .setEmitted_counts(std::move(emitted))
      .setCompression(std::move(compression))
      .setDiff(std::move(payload))
      .build();
  *out = std::move(result);
  return protocol::Response::Success();
}

protocol::Response InspectorChromiumRLAgent::compareStructuredSnapshots(
    std::unique_ptr<protocol::ChromiumRL::StructuredPageSnapshot> before_snapshot,
    std::unique_ptr<protocol::ChromiumRL::StructuredPageSnapshot> after_snapshot,
    std::optional<String> action_type,
    std::unique_ptr<protocol::ChromiumRL::StructuredSnapshotDiff>* diff) {
  return BuildStructuredSnapshotDiff(before_snapshot.get(), after_snapshot.get(),
                                     action_type, diff);
}

protocol::Response InspectorChromiumRLAgent::captureSnapshotDiff(
    std::unique_ptr<protocol::ChromiumRL::StructuredPageSnapshot> before_snapshot,
    std::optional<String> action_type,
    std::optional<bool> in_viewport_only,
    std::optional<String> root_selector,
    std::optional<int> max_nodes,
    std::optional<int> max_text_chars,
    std::optional<bool> include_offscreen,
    std::unique_ptr<protocol::ChromiumRL::StructuredPageSnapshot>* after_snapshot,
    std::unique_ptr<protocol::ChromiumRL::StructuredSnapshotDiff>* diff) {
  std::unique_ptr<protocol::ChromiumRL::StructuredPageSnapshot> captured_after;
  protocol::Response capture_response = captureStructuredSnapshot(
      in_viewport_only, root_selector, max_nodes, max_text_chars,
      include_offscreen, false, false, &captured_after);
  if (!capture_response.IsSuccess())
    return capture_response;
  protocol::Response diff_response = BuildStructuredSnapshotDiff(
      before_snapshot.get(), captured_after.get(), action_type, diff);
  if (!diff_response.IsSuccess())
    return diff_response;
  *after_snapshot = std::move(captured_after);
  return protocol::Response::Success();
}


namespace {

// Browser-side port of scripts/render_chromiumrl_snapshot_model.py. It
// consumes only the supplied protocol object and never reads or mutates the
// live document.
class StructuredModelDOMRenderer {
 public:
  using Node = protocol::ChromiumRL::PageNode;

  explicit StructuredModelDOMRenderer(
      protocol::ChromiumRL::StructuredPageSnapshot* snapshot)
      : snapshot_(snapshot) {
    if (!snapshot_)
      return;
    for (const auto& owned_node : *snapshot_->getNodes()) {
      Node* node = owned_node.get();
      nodes_.push_back(node);
      by_ref_.Set(node->getRef(), node);
      if (node->hasParentRef()) {
        children_.insert(node->getParentRef(String()), Vector<Node*>())
            .stored_value->value.push_back(node);
      }
    }
    for (auto& entry : children_) {
      std::stable_sort(entry.value.begin(), entry.value.end(),
                       [](Node* left, Node* right) {
                         return left->getSourceOrder() <
                                right->getSourceOrder();
                       });

      HashMap<String, Vector<Node*>> by_signature;
      for (Node* child : entry.value) {
        String semantic = child->hasSemanticBoundary()
                              ? PythonLower(Clean(
                                    child->getSemanticBoundary(String()), false))
                              : String();
        String signature = NodeTag(child) + "\n" + NodeRole(child) + "\n" +
                           semantic;
        by_signature.insert(signature, Vector<Node*>())
            .stored_value->value.push_back(child);
      }
      for (const auto& repeated : by_signature) {
        if (repeated.value.size() < 2)
          continue;
        for (Node* child : repeated.value)
          repeated_item_refs_.insert(child->getRef());
      }
    }
    BuildActionNodes();
    for (const ActionRow& row : action_nodes_) {
      if (row.actions.size() == 1 && row.actions[0] == "scroll")
        continue;
      if (IsBroadAction(row.node, row.actions))
        continue;
      String key = NormKey(Text(row.node));
      if (!key.empty())
        action_labels_.insert(key);
    }
    GroupNestedActions();
  }

  std::unique_ptr<protocol::ChromiumRL::ModelDOM> Render();

 private:
  static constexpr int kMaxActions = 80;
  static constexpr int kMaxSecondaryActions = 160;
  static constexpr int kMaxNestedActions = 8;
  static constexpr int kMaxTableRows = 60;
  static constexpr int kMaxMedia = 60;
  static constexpr int kMaxScrollRegions = 16;
  static constexpr int kMaxCellChars = 180;
  static constexpr int kMaxTextChars = 0;
  static constexpr int kChromeLabelMaxChars = 40;
  static constexpr bool kIncludeSecondary = true;
  static constexpr bool kIncludeOffscreenContent = true;

  struct ActionRow {
    Node* node = nullptr;
    Vector<String> actions;
  };

  struct ParsedMessage {
    String author;
    String time;
    String text;
    String thread;
    String reactions;
  };

  struct ScrollRegionRow {
    double area = 0;
    Node* node = nullptr;
  };

  static bool IsPythonWhitespace(UChar c) {
    return (c >= 0x0009 && c <= 0x000d) ||
           (c >= 0x001c && c <= 0x0020) || c == 0x0085 || c == 0x00a0 ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200a) ||
           c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
           c == 0x3000;
  }

  static UChar32 CodePointAt(const String& text,
                             wtf_size_t offset,
                             wtf_size_t* next) {
    UChar32 result = text[offset++];
    if (U16_IS_LEAD(result) && offset < text.length() &&
        U16_IS_TRAIL(text[offset])) {
      result = U16_GET_SUPPLEMENTARY(result, text[offset++]);
    }
    *next = offset;
    return result;
  }

  static UChar32 CodePointBefore(const String& text, wtf_size_t offset) {
    UChar32 result = text[--offset];
    if (U16_IS_TRAIL(result) && offset > 0 && U16_IS_LEAD(text[offset - 1]))
      result = U16_GET_SUPPLEMENTARY(text[offset - 1], result);
    return result;
  }

  static bool IsPythonWord(UChar32 value) {
    return value == '_' || u_isalnum(value);
  }

  static bool ContainsPythonDigit(const String& text) {
    for (wtf_size_t offset = 0; offset < text.length();) {
      wtf_size_t next = offset;
      UChar32 value = CodePointAt(text, offset, &next);
      offset = next;
      int numeric_type =
          u_getIntPropertyValue(value, UCHAR_NUMERIC_TYPE);
      if (numeric_type == U_NT_DECIMAL || numeric_type == U_NT_DIGIT)
        return true;
    }
    return false;
  }

  static bool IsAttachmentBoundaryCharacter(UChar32 value) {
    return IsPythonWord(value) || value == '.' || value == '-';
  }

  static void AppendCodePoint(StringBuilder& builder, UChar32 value) {
    if (U_IS_BMP(value)) {
      builder.Append(static_cast<UChar>(value));
      return;
    }
    builder.Append(U16_LEAD(value));
    builder.Append(U16_TRAIL(value));
  }

  static String CodePoints(std::initializer_list<UChar32> values) {
    StringBuilder builder;
    for (UChar32 value : values)
      AppendCodePoint(builder, value);
    return builder.ToString();
  }

  static wtf_size_t CodePointLength(const String& value) {
    wtf_size_t count = 0;
    for (wtf_size_t offset = 0; offset < value.length(); ++count) {
      wtf_size_t next = offset;
      CodePointAt(value, offset, &next);
      offset = next;
    }
    return count;
  }

  static String PrefixCodePoints(const String& value, wtf_size_t count) {
    wtf_size_t offset = 0;
    wtf_size_t seen = 0;
    while (offset < value.length() && seen < count) {
      wtf_size_t next = offset;
      CodePointAt(value, offset, &next);
      offset = next;
      ++seen;
    }
    return value.substr(0, offset);
  }

  static String StripPythonWhitespace(const String& value) {
    wtf_size_t start = 0;
    wtf_size_t end = value.length();
    while (start < end && IsPythonWhitespace(value[start]))
      ++start;
    while (end > start && IsPythonWhitespace(value[end - 1]))
      --end;
    return value.substr(start, end - start);
  }

  static String CollapsePythonWhitespace(const String& value) {
    StringBuilder result;
    bool pending_space = false;
    for (wtf_size_t i = 0; i < value.length(); ++i) {
      UChar c = value[i];
      if (IsPythonWhitespace(c)) {
        if (result.length())
          pending_space = true;
        continue;
      }
      if (pending_space) {
        result.Append(' ');
        pending_space = false;
      }
      result.Append(c);
    }
    return result.ToString();
  }

  static String DecodeHtmlEntities(const String& value) {
    if (!value.contains('&'))
      return value;
    SegmentedString source(value);
    StringBuilder result;
    while (!source.IsEmpty()) {
      UChar current = source.CurrentChar();
      source.Advance();
      if (current != '&') {
        result.Append(current);
        continue;
      }
      bool not_enough_characters = false;
      DecodedHTMLEntity decoded;
      bool success =
          ConsumeHTMLEntity(source, decoded, not_enough_characters);
      if (!success || not_enough_characters) {
        result.Append('&');
        continue;
      }
      for (unsigned i = 0; i < decoded.length; ++i)
        result.Append(decoded.data[i]);
    }
    return result.ToString();
  }

  static String Clean(const String& raw, bool fix_mojibake = true) {
    String text = raw;
    if (fix_mojibake) {
      struct Replacement {
        String bad;
        String good;
      };
      const Replacement replacements[] = {
          {CodePoints({0x00e2, 0x20ac, 0x00a2}), CodePoints({0x2022})},
          {CodePoints({0x00e2, 0x20ac, 0x201c}), CodePoints({0x2013})},
          {CodePoints({0x00e2, 0x20ac, 0x201d}), CodePoints({0x2014})},
          {CodePoints({0x00e2, 0x20ac, 0x02dc}), CodePoints({0x2018})},
          {CodePoints({0x00e2, 0x20ac, 0x2122}), CodePoints({0x2019})},
          {CodePoints({0x00e2, 0x20ac, 0x0153}), CodePoints({0x201c})},
          {CodePoints({0x00e2, 0x20ac, 0x009d}), CodePoints({0x201d})},
          {CodePoints({0x00e2, 0x20ac}), CodePoints({0x201d})},
          {CodePoints({0x00c2, 0x0020}), CodePoints({0x0020})},
          {CodePoints({0x00c2}), String()},
      };
      for (const Replacement& replacement : replacements)
        text.Replace(replacement.bad, replacement.good);
    }
    text = DecodeHtmlEntities(text);
    if (text.contains('<') && text.contains('>')) {
      std::string utf8 = text.Utf8().c_str();
      re2::StringPiece labels_input(utf8);
      std::string label;
      Vector<String> labels;
      static const base::NoDestructor<re2::RE2> kLabelPattern(
          "(?i)\\b(?:aria-label|alt|title)=[\"']([^\"']+)[\"']");
      while (re2::RE2::FindAndConsume(&labels_input, *kLabelPattern, &label))
        labels.push_back(String::FromUtf8(label));
      static const base::NoDestructor<re2::RE2> kTagPattern("<[^>]+>");
      re2::RE2::GlobalReplace(&utf8, *kTagPattern, " ");
      String stripped =
          CollapsePythonWhitespace(String::FromUtf8(utf8));
      if (!stripped.empty()) {
        text = stripped;
      } else if (!labels.empty()) {
        text = Join(labels, " ");
      }
    }
    return CollapsePythonWhitespace(text);
  }

  static String Clip(const String& text, int limit) {
    if (limit <= 0 || CodePointLength(text) <= static_cast<wtf_size_t>(limit))
      return text;
    String clipped = PrefixCodePoints(text, std::max(0, limit - 1));
    clipped = StripPythonWhitespace(clipped);
    return clipped + CodePoints({0x2026});
  }

  static String PythonLower(const String& text) {
    static const CaseMap kRootCaseMap{CaseMap::Locale()};
    return kRootCaseMap.ToLower(text);
  }

  static String NormKey(const String& text) {
    String lowered = PythonLower(text);
    StringBuilder result;
    for (wtf_size_t offset = 0; offset < lowered.length();) {
      wtf_size_t next = offset;
      UChar32 value = CodePointAt(lowered, offset, &next);
      offset = next;
      if (IsPythonWord(value))
        AppendCodePoint(result, value);
    }
    return result.ToString();
  }

  static Vector<String> Dedupe(const Vector<String>& values) {
    Vector<String> result;
    HashSet<String> seen;
    for (const String& item : values) {
      String value = Clean(item, false);
      if (value.empty() || seen.Contains(value))
        continue;
      seen.insert(value);
      result.push_back(value);
    }
    return result;
  }

  static String Join(const Vector<String>& values, const String& separator) {
    StringBuilder result;
    for (wtf_size_t i = 0; i < values.size(); ++i) {
      if (i)
        result.Append(separator);
      result.Append(values[i]);
    }
    return result.ToString();
  }

  static bool Contains(const Vector<String>& values, const String& value) {
    for (const String& item : values) {
      if (item == value)
        return true;
    }
    return false;
  }

  static bool IsOneOf(const String& value,
                      std::initializer_list<const char*> choices) {
    for (const char* choice : choices) {
      if (value == choice)
        return true;
    }
    return false;
  }

  static bool ContainsAny(const Vector<String>& values,
                          std::initializer_list<const char*> choices) {
    for (const char* choice : choices) {
      if (Contains(values, choice))
        return true;
    }
    return false;
  }

  static String JsonQuote(const String& value) {
    static const char kHex[] = "0123456789abcdef";
    StringBuilder result;
    result.Append('"');
    for (wtf_size_t i = 0; i < value.length(); ++i) {
      UChar c = value[i];
      switch (c) {
        case '"': result.Append("\\\""); break;
        case '\\': result.Append("\\\\"); break;
        case '\b': result.Append("\\b"); break;
        case '\f': result.Append("\\f"); break;
        case '\n': result.Append("\\n"); break;
        case '\r': result.Append("\\r"); break;
        case '\t': result.Append("\\t"); break;
        default:
          if (c < 0x20) {
            result.Append("\\u00");
            result.Append(kHex[(c >> 4) & 0xf]);
            result.Append(kHex[c & 0xf]);
          } else {
            result.Append(c);
          }
      }
    }
    result.Append('"');
    return result.ToString();
  }

  static String PythonNumber(double value) {
    if (std::isfinite(value) && value == std::trunc(value))
      return String::Number(static_cast<int64_t>(value));
    return String::FromUtf8(base::NumberToString(value));
  }

  static String PythonBool(bool value) {
    return value ? "True" : "False";
  }

  static bool RegexSearch(const String& text, const re2::RE2& pattern) {
    return re2::RE2::PartialMatch(text.Utf8().c_str(), pattern);
  }

  static bool RegexFullMatch(const String& text, const re2::RE2& pattern) {
    return re2::RE2::FullMatch(text.Utf8().c_str(), pattern);
  }

  static HashMap<String, String> AttrMap(Node* node) {
    HashMap<String, String> attributes;
    if (!node)
      return attributes;
    for (const auto& owned_attribute : *node->getSelectedAttributes()) {
      auto* attribute = owned_attribute.get();
      String name = Clean(attribute->getName(), false);
      String value = Clean(attribute->getValue());
      if (!name.empty())
        attributes.Set(name, value);
    }
    return attributes;
  }

  static String BoundsText(Node* node) {
    if (!node || !node->hasBounds())
      return String();
    protocol::DOM::Rect* bounds = node->getBounds(nullptr);
    return "x=" + PythonNumber(bounds->getX()) +
           " y=" + PythonNumber(bounds->getY()) +
           " w=" + PythonNumber(bounds->getWidth()) +
           " h=" + PythonNumber(bounds->getHeight());
  }
  static bool HasNonEmptyAttribute(const HashMap<String, String>& attrs,
                                   const String& name) {
    auto it = attrs.find(name);
    return it != attrs.end() && !it->value.empty();
  }

  Node* FindNode(const String& ref) const {
    auto it = by_ref_.find(ref);
    return it == by_ref_.end() ? nullptr : it->value;
  }

  Vector<String> ActionTypes(Node* node) const {
    auto cached = action_types_cache_.find(node);
    if (cached != action_types_cache_.end())
      return cached->value;
    Vector<String> values;
    for (const String& action : *node->getActionTypes())
      values.push_back(action);
    // The protocol has no snapshot-level actions array, so Python's action_map
    // is empty for every real capture.
    values = Dedupe(values);
    action_types_cache_.Set(node, values);
    return values;
  }

  String ActionId(Node* node) const {
    return node->hasBackendNodeId()
               ? String::Number(node->getBackendNodeId(0))
               : String::Number(node->getIndex());
  }

  Vector<Node*> Ancestors(Node* node) const {
    Vector<Node*> result;
    HashSet<String> seen;
    while (node && node->hasParentRef()) {
      String ref = node->getParentRef(String());
      if (seen.Contains(ref))
        break;
      seen.insert(ref);
      node = FindNode(ref);
      if (!node)
        break;
      result.push_back(node);
    }
    return result;
  }

  HashSet<String> Descendants(const String& ref) const {
    HashSet<String> found;
    Vector<Node*> stack;
    auto it = children_.find(ref);
    if (it != children_.end())
      stack = it->value;
    while (!stack.empty()) {
      Node* node = stack.back();
      stack.pop_back();
      String node_ref = node->getRef();
      if (found.Contains(node_ref))
        continue;
      found.insert(node_ref);
      auto child_it = children_.find(node_ref);
      if (child_it != children_.end())
        for (Node* child : child_it->value)
          stack.push_back(child);
    }
    return found;
  }

  String ComputeText(Node* node) const {
    String direct = node->hasDirectText()
                        ? Clean(node->getDirectText(String()))
                        : String();
    String accessible = node->hasAccessibleName()
                            ? Clean(node->getAccessibleName(String()))
                            : String();
    String subtree = node->hasSubtreeText()
                         ? Clean(node->getSubtreeText(String()))
                         : String();
    if (!direct.empty() &&
        CodePointLength(subtree) > CodePointLength(direct)) {
      String direct_key = NormKey(direct);
      if (node->getTruncated() ||
          (!direct_key.empty() && NormKey(subtree).contains(direct_key)))
        return subtree;
    }
    if (!direct.empty())
      return direct;
    if (!subtree.empty() &&
        CodePointLength(subtree) > CodePointLength(accessible))
      return subtree;
    if (!accessible.empty())
      return accessible;
    if (!subtree.empty())
      return subtree;
    if (node->hasDescription()) {
      String description = Clean(node->getDescription(String()));
      if (!description.empty())
        return description;
    }
    HashMap<String, String> attrs = AttrMap(node);
    for (const char* key : {"aria-label", "title", "placeholder", "alt",
                            "value", "name"}) {
      auto it = attrs.find(key);
      if (it != attrs.end() && !it->value.empty())
        return it->value;
    }
    return String();
  }

  String Text(Node* node) const {
    auto cached = text_cache_.find(node);
    if (cached != text_cache_.end())
      return cached->value;
    String value = ComputeText(node);
    text_cache_.Set(node, value);
    return value;
  }

  String NodeTag(Node* node) const {
    auto cached = tag_cache_.find(node);
    if (cached != tag_cache_.end())
      return cached->value;
    String value = PythonLower(Clean(node->getTag(), false));
    tag_cache_.Set(node, value);
    return value;
  }

  String NodeRole(Node* node) const {
    auto cached = role_cache_.find(node);
    if (cached != role_cache_.end())
      return cached->value;
    String value = node->hasRole()
                       ? PythonLower(Clean(node->getRole(String()), false))
                       : String();
    role_cache_.Set(node, value);
    return value;
  }

  String ActionKind(Node* node, const Vector<String>& actions) const {
    String tag = NodeTag(node);
    String role = NodeRole(node);
    HashMap<String, String> attrs = AttrMap(node);
    String label = PythonLower(Text(node));
    if (Contains(actions, "type") ||
        IsOneOf(role, {"textbox", "searchbox"}) ||
        IsOneOf(tag, {"input", "textarea"})) {
      return role != "searchbox" && !label.contains("search")
                 ? "text-input"
                 : "search-input";
    }
    if (IsOneOf(role, {"checkbox", "radio", "switch"}) ||
        Contains(actions, "toggle"))
      return "toggle";
    if (IsOneOf(role, {"combobox", "listbox"}) || tag == "select" ||
        Contains(actions, "select"))
      return "select";
    if (Contains(actions, "upload"))
      return "file-upload";
    if (tag == "a" || role == "link" || HasNonEmptyAttribute(attrs, "href"))
      return "link";
    if (tag == "button" || role == "button")
      return "button";
    if (actions.size() == 1 && actions[0] == "scroll")
      return "scroll-region";
    if (Contains(actions, "click"))
      return "clickable";
    return "action";
  }

  String CompactNodeLabel(Node* node, int limit = 80) const {
    String label = node->hasAccessibleName()
                       ? Clean(node->getAccessibleName(String()))
                       : String();
    if (label.empty() && node->hasDirectText())
      label = Clean(node->getDirectText(String()));
    if (label.empty() && node->hasDescription())
      label = Clean(node->getDescription(String()));
    if (label.empty()) {
      HashMap<String, String> attrs = AttrMap(node);
      for (const char* key : {"aria-label", "title", "placeholder", "alt",
                              "value", "name"}) {
        auto it = attrs.find(key);
        if (it != attrs.end() && !it->value.empty()) {
          label = it->value;
          break;
        }
      }
    }
    return Clip(label, limit);
  }

  String RegionContext(Node* node, int limit = 160) const {
    Vector<String> parts;
    HashSet<String> seen;
    for (Node* ancestor : Ancestors(node)) {
      String tag = NodeTag(ancestor);
      String role = NodeRole(ancestor);
      String semantic = ancestor->hasSemanticBoundary()
                            ? PythonLower(Clean(
                                  ancestor->getSemanticBoundary(String()), false))
                            : String();
      if (IsOneOf(tag, {"html", "body"}))
        continue;
      if (IsOneOf(role, {"generic", "none", ""}) &&
          IsOneOf(tag, {"div", "span"}) && semantic.empty())
        continue;
      String label = CompactNodeLabel(ancestor, 70);
      if (label.empty())
        label = !role.empty() ? role : tag;
      String key = NormKey(role + ":" + tag + ":" + label);
      if (key.empty() || seen.Contains(key))
        continue;
      seen.insert(key);
      String prefix = !role.empty() ? role : (!semantic.empty() ? semantic : tag);
      parts.push_back(label.empty() ? prefix : prefix + ": " + label);
      if (parts.size() >= 3)
        break;
    }
    std::reverse(parts.begin(), parts.end());
    return Clip(Join(parts, " / "), limit);
  }

  bool IsBroadAction(Node* node, const Vector<String>& actions) const {
    String tag = NodeTag(node);
    String role = NodeRole(node);
    String text = Text(node);
    String direct = node->hasDirectText()
                        ? Clean(node->getDirectText(String()))
                        : String();
    if (IsOneOf(tag, {"html", "body"}))
      return true;
    if (IsOneOf(role, {"dialog", "alertdialog"}) &&
        IsOneOf(tag, {"html", "body", "main", "section", "article", "div"}))
      return true;
    if (IsOneOf(tag, {"table", "thead", "tbody", "tfoot", "tr", "td", "th"}) &&
        actions.size() == 1 && actions[0] == "scroll")
      return true;
    if (IsOneOf(tag, {"html", "body", "main", "section", "article", "div"}) &&
        IsOneOf(role, {"", "generic", "none", "application"})) {
      if (direct.empty() && CodePointLength(text) > 220)
        return true;
      if (actions.size() == 1 && actions[0] == "click" &&
          CodePointLength(text) > 180)
        return true;
    }
    return false;
  }

  String DisplayContext(Node* node, int limit = 80) const {
    for (Node* ancestor : Ancestors(node)) {
      String tag = NodeTag(ancestor);
      String role = NodeRole(ancestor);
      String semantic = ancestor->hasSemanticBoundary()
                            ? PythonLower(Clean(
                                  ancestor->getSemanticBoundary(String()), false))
                            : String();
      String label = CompactNodeLabel(ancestor, limit);
      if (IsOneOf(StripPythonWhitespace(PythonLower(label)),
                  {"", "document", "generic", "group", "home",
                   "jump to date", "list", "listitem", "main", "none",
                   "tabpanel", "toolbar"}))
        label = String();
      if (!label.empty() &&
          IsOneOf(role, {"alertdialog", "dialog", "group", "list", "main",
                         "navigation", "search", "tabpanel", "toolbar", "tree"}))
        return Clip(label, limit);
      if (!label.empty() && IsOneOf(semantic, {"list", "section", "region"}))
        return Clip(label, limit);
      if (!label.empty() &&
          IsOneOf(tag, {"article", "aside", "footer", "header", "main", "nav", "section"}))
        return Clip(label, limit);
    }
    return String();
  }

  bool HasAncestorRoleOrTag(Node* node,
                            std::initializer_list<const char*> values) const {
    for (Node* ancestor : Ancestors(node)) {
      if (IsOneOf(NodeTag(ancestor), values) ||
          IsOneOf(NodeRole(ancestor), values))
        return true;
    }
    return false;
  }

  bool IsReadableItem(Node* node) const {
    String semantic = node->hasSemanticBoundary()
                          ? PythonLower(Clean(
                                node->getSemanticBoundary(String()), false))
                          : String();
    return NodeTag(node) == "li" || NodeRole(node) == "listitem" ||
           semantic == "listitem";
  }

  Node* NearestReadableItem(Node* node) const {
    for (Node* ancestor : Ancestors(node)) {
      if (IsReadableItem(ancestor))
        return ancestor;
    }
    return nullptr;
  }

  String SemanticOwnerRef(Node* node) const {
    if (IsReadableItem(node))
      return node->getRef();
    Node* owner = NearestReadableItem(node);
    if (!owner) {
      for (Node* ancestor : Ancestors(node)) {
        if (repeated_item_refs_.Contains(ancestor->getRef())) {
          owner = ancestor;
          break;
        }
      }
    }
    return owner ? owner->getRef() : String();
  }

  Vector<String> AdditionalContentEvidence(
      Node* node,
      const Vector<String>& emitted_lines) const {
    String ref = node->getRef();
    if (ref.empty())
      return {};
    String emitted_key = NormKey(Join(emitted_lines, " "));
    String owner_ref = SemanticOwnerRef(node);
    Vector<Node*> descendants;
    for (const String& child_ref : Descendants(ref)) {
      Node* child = FindNode(child_ref);
      if (child)
        descendants.push_back(child);
    }
    std::stable_sort(descendants.begin(), descendants.end(),
                     [](Node* left, Node* right) {
                       return left->getSourceOrder() < right->getSourceOrder();
                     });
    Vector<String> evidence;
    HashSet<String> seen;
    for (Node* child : descendants) {
      if (ActionTypes(child).empty())
        continue;
      String child_owner_ref = SemanticOwnerRef(child);
      if (!owner_ref.empty() && !child_owner_ref.empty() &&
          child_owner_ref != owner_ref) {
        continue;
      }
      HashMap<String, String> attrs = AttrMap(child);
      auto attribute = [&attrs](const String& name) {
        auto it = attrs.find(name);
        return it == attrs.end() ? String() : it->value;
      };
      Vector<String> candidates = {
          child->hasAccessibleName()
              ? Clean(child->getAccessibleName(String()))
              : String(),
          attribute("aria-label"), attribute("value"), attribute("title"),
          Text(child)};
      bool has_state = false;
      for (const auto& state : *child->getStates()) {
        if (!Clean(state->getValue()).empty()) {
          has_state = true;
          break;
        }
      }
      for (const String& candidate : candidates) {
        String value = Clean(candidate);
        if (!has_state && !ContainsPythonDigit(value))
          continue;
        String key = NormKey(value);
        if (key.empty() || seen.Contains(key) || emitted_key.contains(key))
          continue;
        seen.insert(key);
        evidence.push_back(value);
        break;
      }
    }
    return evidence;
  }

  static bool ControlWord(const String& text) {
    static const base::NoDestructor<re2::RE2> pattern(
        "(?i)\\b(?:open|view|show|expand|details|more|download|submit|send|"
        "search|load|toggle|next|previous|change|dismiss|close|sign in|reply|"
        "replies|thread|comment|comments|file|attachment)\\b");
    return RegexSearch(text, *pattern);
  }

  static bool EssentialActionWord(const String& text) {
    static const base::NoDestructor<re2::RE2> pattern(
        "(?i)\\b(?:search|load|submit|send|save|dismiss|close|change|continue|"
        "next|previous|open|download|upload)\\b");
    return RegexSearch(text, *pattern);
  }

  bool LowValueNestedControl(Node* node,
                             const Vector<String>& actions,
                             const String& parent_text) const {
    String label = Text(node);
    if (ContainsAny(actions, {"type", "focus", "select", "toggle", "upload"}))
      return false;
    if (label.empty() || StripPythonWhitespace(PythonLower(label)) == "toggle file")
      return true;
    static const base::NoDestructor<re2::RE2> media(
        "(?i)\\.(?:pdf|docx?|xlsx?|pptx?|csv|txt|png|jpe?g|gif|webp|svg)\\b");
    if (RegexSearch(label, *media))
      return true;
    if (ControlWord(label))
      return false;
    static const base::NoDestructor<re2::RE2> punctuation("^[^A-Za-z0-9]+$");
    static const base::NoDestructor<re2::RE2> handle("^@\\S+$");
    static const base::NoDestructor<re2::RE2> time(
        "(?i)^\\d{1,2}:\\d{2}(?::\\d{2})?\\s*(?:AM|PM)?$");
    static const base::NoDestructor<re2::RE2> date(
        "(?i)^\\d{1,2}\\s+[A-Za-z]{3,9}\\s+\\d{4}(?:\\s+at\\b.*)?$");
    if (RegexSearch(label, *punctuation) || RegexSearch(label, *handle) ||
        RegexSearch(label, *time) || RegexSearch(label, *date))
      return true;
    if (PythonLower(RegionContext(node, 160)).contains("profile") &&
        !ControlWord(label))
      return true;
    if (NodeTag(node) == "button" || NodeRole(node) == "button")
      return false;
    String label_key = NormKey(label);
    return !label_key.empty() && NormKey(parent_text).contains(label_key) &&
           CodePointLength(label) <= kChromeLabelMaxChars;
  }

  bool IsPrimaryAction(Node* node, const Vector<String>& actions) const {
    if (suppressed_refs_.Contains(node->getRef()) || IsBroadAction(node, actions))
      return false;
    String tag = NodeTag(node);
    String label = Text(node);
    if (actions.size() == 1 && actions[0] == "scroll")
      return false;
    if (!node->getVisible() || !node->getInViewport() || !node->getHitTestable())
      return false;
    if (IsOneOf(tag, {"input", "textarea", "select"}) ||
        ContainsAny(actions, {"type", "focus", "select", "toggle", "upload"}))
      return true;
    String label_key = NormKey(label);
    if (!label.empty() &&
        !IsOneOf(label_key, {"", "toggle", "more", "menu"}))
      return true;
    HashMap<String, String> attrs = AttrMap(node);
    return !label.empty() && (HasNonEmptyAttribute(attrs, "href") ||
                              HasNonEmptyAttribute(attrs, "src"));
  }

  bool IsUsefulNestedAction(Node* node,
                            const Vector<String>& actions,
                            const String& parent_text) const {
    String label = Text(node);
    String tag = NodeTag(node);
    if (ContainsAny(actions, {"type", "focus", "select", "toggle", "upload"}) ||
        IsOneOf(tag, {"input", "textarea", "select"}))
      return true;
    if (label.empty() || LowValueNestedControl(node, actions, parent_text))
      return false;
    if (ControlWord(label) || tag == "button" ||
        (tag == "a" && CodePointLength(label) > 2))
      return true;
    String key = NormKey(label);
    return key.empty() || !NormKey(parent_text).contains(key);
  }

  std::pair<int, int> NestedActionRank(Node* node) const {
    Vector<String> actions = ActionTypes(node);
    int rank = 3;
    if (ContainsAny(actions, {"type", "focus", "select", "toggle", "upload"}))
      rank = 0;
    else if (ControlWord(Text(node)))
      rank = 1;
    else if (NodeTag(node) == "button" || NodeRole(node) == "button")
      rank = 2;
    else if (NodeTag(node) == "a" || NodeRole(node) == "link")
      rank = 4;
    return {rank, node->getSourceOrder()};
  }

  void BuildActionNodes() {
    for (Node* node : nodes_) {
      Vector<String> actions = ActionTypes(node);
      if (!actions.empty())
        action_nodes_.push_back(ActionRow{node, actions});
    }
    std::stable_sort(action_nodes_.begin(), action_nodes_.end(),
                     [](const ActionRow& left, const ActionRow& right) {
      if (left.node->getIndex() != right.node->getIndex())
        return left.node->getIndex() < right.node->getIndex();
      return left.node->getSourceOrder() < right.node->getSourceOrder();
    });
  }

  void GroupNestedActions() {
    for (const ActionRow& row : action_nodes_) {
      if (!IsPrimaryAction(row.node, row.actions))
        continue;
      Node* parent = NearestReadableItem(row.node);
      if (!parent ||
          !IsUsefulNestedAction(row.node, row.actions, Text(parent)) ||
          parent->getRef().empty() || parent->getRef() == row.node->getRef())
        continue;
      nested_actions_.insert(parent->getRef(), Vector<ActionRow>())
          .stored_value->value.push_back(row);
    }
    for (auto& entry : nested_actions_) {
      std::stable_sort(entry.value.begin(), entry.value.end(),
                       [this](const ActionRow& left, const ActionRow& right) {
        return NestedActionRank(left.node) < NestedActionRank(right.node);
      });
    }
  }

  String VisibilityNotes(Node* node) const {
    Vector<String> notes;
    if (!node->getVisible()) notes.push_back("hidden");
    if (!node->getInViewport()) notes.push_back("offscreen");
    if (node->getOccluded()) notes.push_back("occluded");
    if (!node->getHitTestable()) notes.push_back("not-hit-testable");
    return Join(notes, ",");
  }

  String ActionLine(Node* node,
                    const Vector<String>& actions,
                    bool debug = false) const {
    String label = Clip(Text(node), debug ? 220 : 140);
    static const base::NoDestructor<re2::RE2> thread(
        "(?i)\\b\\d+\\s+repl(?:y|ies)(?:\\s+(?:Last reply\\s+)?[^|]{1,50}?)?\\s+View thread\\b");
    static const base::NoDestructor<re2::RE2> replies(
        "(?i)^\\d+\\s+repl(?:y|ies)$");
    if (!debug && (RegexSearch(label, *thread) || RegexFullMatch(label, *replies)))
      return "[" + ActionId(node) + "] open thread/replies";
    String kind = ActionKind(node, actions);
    Vector<String> bits = {"[" + ActionId(node) + "]", kind};
    if (!label.empty()) bits.push_back(JsonQuote(label));
    if (!(actions.size() == 1 && actions[0] == "click") ||
        IsOneOf(kind, {"search-input", "text-input", "select", "toggle", "file-upload"}))
      bits.push_back("actions=" + Join(actions, ","));
    HashMap<String, String> attrs = AttrMap(node);
    if (HasNonEmptyAttribute(attrs, "src") && label.empty()) bits.push_back("src=(image)");
    if (debug) {
      bits.push_back("<" + (node->getTag().empty() ? String("?") : node->getTag()) + ">");
      String role = node->hasRole() ? Clean(node->getRole(String()), false) : String();
      if (!role.empty()) bits.push_back("role=" + role);
      String context = RegionContext(node, 140);
      if (!context.empty()) bits.push_back("context=" + JsonQuote(context));
      String visibility = VisibilityNotes(node);
      if (!visibility.empty()) bits.push_back("state=" + visibility);
      bits.push_back("hit=" + PythonBool(node->getHitTestable()));
      bits.push_back("viewport=" + PythonBool(node->getInViewport()));
      bits.push_back("sourceOrder=" + String::Number(node->getSourceOrder()));
      String bounds = BoundsText(node);
      if (!bounds.empty()) bits.push_back("bounds=(" + bounds + ")");
    }
    return Join(bits, " ");
  }

  HashSet<String> AncestorRegionTokens(Node* node) const {
    HashSet<String> result;
    for (Node* ancestor : Ancestors(node)) {
      String tag = NodeTag(ancestor), role = NodeRole(ancestor);
      if (!tag.empty()) result.insert(tag);
      if (!role.empty()) result.insert(role);
      if (ancestor->hasSemanticBoundary()) {
        String semantic = PythonLower(
            Clean(ancestor->getSemanticBoundary(String()), false));
        if (!semantic.empty()) result.insert(semantic);
      }
    }
    return result;
  }

  std::optional<int> AttrInt(Node* node, const String& name) const {
    HashMap<String, String> attrs = AttrMap(node);
    auto it = attrs.find(name);
    if (it == attrs.end()) return std::nullopt;
    int value = 0;
    if (!base::StringToInt(it->value.Utf8(), &value)) return std::nullopt;
    return value;
  }

  bool HasDeeperTreeItem(Node* node) const {
    std::optional<int> level = AttrInt(node, "aria-level");
    if (!level.has_value()) return false;
    String tree_ref;
    for (Node* ancestor : Ancestors(node)) {
      if (NodeRole(ancestor) == "tree") { tree_ref = ancestor->getRef(); break; }
    }
    if (tree_ref.empty()) return false;
    for (const String& ref : Descendants(tree_ref)) {
      Node* other = FindNode(ref);
      if (!other || other == node || NodeRole(other) != "treeitem") continue;
      std::optional<int> other_level = AttrInt(other, "aria-level");
      if (other_level.has_value() && *other_level > *level) return true;
    }
    return false;
  }

  std::pair<int, int> ActionPriority(Node* node,
                                     const Vector<String>& actions) const {
    String label = Text(node), lower = PythonLower(label);
    String kind = ActionKind(node, actions), role = NodeRole(node), tag = NodeTag(node);
    HashSet<String> tokens = AncestorRegionTokens(node);
    String context = PythonLower(RegionContext(node, 180));
    bool toolbar = tokens.Contains("toolbar");
    bool navigation = tokens.Contains("nav") || tokens.Contains("navigation") ||
                      tokens.Contains("aside") || tokens.Contains("tree") ||
                      tokens.Contains("treeitem") || tokens.Contains("tablist");
    bool search = tokens.Contains("search") || context.contains("search") ||
                  role == "option" || tokens.Contains("listbox");
    bool main = tokens.Contains("main") || tokens.Contains("article") ||
                tokens.Contains("feed") || tokens.Contains("list") ||
                tokens.Contains("listitem") || tokens.Contains("table") ||
                tokens.Contains("row") || tokens.Contains("cell") ||
                NearestReadableItem(node);
    bool result_like = search &&
        !IsOneOf(kind, {"search-input", "text-input", "file-upload"}) &&
        CodePointLength(label) >= 12 && !toolbar &&
        (IsOneOf(role, {"option", "listitem", "treeitem"}) ||
         IsOneOf(kind, {"select", "link", "clickable"}));
    int order = node->getSourceOrder();
    if (!node->getHitTestable() || !node->getInViewport() || !node->getVisible()) return {9, order};
    if (lower.contains("nan")) return {9, order};
    if (IsOneOf(kind, {"search-input", "text-input"})) return {0, order};
    if (lower.contains("search")) return {1, order};
    if (result_like || lower.starts_with("load ")) return {2, order};
    if (role == "tab") return {7, order};
    if (role == "treeitem" && !label.empty() && CodePointLength(label) <= 32) {
      std::optional<int> level = AttrInt(node, "aria-level");
      if (level.has_value() && *level <= 1 && HasDeeperTreeItem(node)) return {7, order};
      return {3, order};
    }
    if (navigation && !EssentialActionWord(label)) return {7, order};
    if (toolbar) return {7, order};
    if ((tag == "button" || role == "button") && !main &&
        !EssentialActionWord(label) && !ControlWord(label)) return {7, order};
    String stripped = StripPythonWhitespace(lower);
    if (IsOneOf(stripped, {"user:", "user"})) return {8, order};
    if (ControlWord(label)) return {2, order};
    if (IsOneOf(role, {"tab", "treeitem"})) return {5, order};
    if (IsOneOf(tag, {"a", "button"}) || IsOneOf(role, {"button", "link"})) return {3, order};
    return {6, order};
  }

  Vector<String> RenderHeader() const {
    Vector<String> lines = {
        "URL: " + Clean(snapshot_->getUrl()),
        "Title: " + Clean(snapshot_->getTitle()),
        "Use only ids from this observation. Scroll with scroll(\"down\", 1600, element_id=<id>)."};
    auto* stats = snapshot_->getStats();
    if (stats) {
      lines.push_back("Snapshot: returnedNodes=" + String::Number(stats->getReturnedNodes()) +
                      " groups=" + String::Number(stats->getGroups()) +
                      " truncated=" + PythonBool(stats->getTruncated()));
    }
    return lines;
  }

  Vector<Vector<String>> TableRows(Node* table) const {
    Vector<Node*> rows;
    for (const String& ref : Descendants(table->getRef())) {
      Node* node = FindNode(ref);
      String semantic = node && node->hasSemanticBoundary()
                            ? PythonLower(Clean(
                                  node->getSemanticBoundary(String()), false))
                            : String();
      if (node && (NodeTag(node) == "tr" || semantic == "row")) rows.push_back(node);
    }
    std::stable_sort(rows.begin(), rows.end(), [](Node* a, Node* b) {
      return a->getSourceOrder() < b->getSourceOrder();
    });
    Vector<Vector<String>> result;
    for (Node* row : rows) {
      Vector<String> cells;
      auto it = children_.find(row->getRef());
      if (it != children_.end()) {
        for (Node* cell : it->value) {
          String semantic = cell->hasSemanticBoundary()
                                ? PythonLower(Clean(
                                      cell->getSemanticBoundary(String()), false))
                                : String();
          if (!IsOneOf(NodeTag(cell), {"td", "th"}) && semantic != "cell") continue;
          String raw = cell->hasDirectText() && !cell->getDirectText(String()).empty()
                           ? cell->getDirectText(String())
                           : cell->getSubtreeText(String());
          cells.push_back(Clip(Clean(raw), kMaxCellChars));
        }
      }
      bool any = false;
      for (const String& cell : cells) if (!cell.empty()) { any = true; break; }
      if (any) result.push_back(cells);
    }
    return result;
  }

  Vector<String> RenderTables(HashSet<String>* covered) const {
    Vector<Node*> tables;
    for (Node* node : nodes_) if (NodeTag(node) == "table") tables.push_back(node);
    std::stable_sort(tables.begin(), tables.end(), [](Node* a, Node* b) {
      return a->getSourceOrder() < b->getSourceOrder();
    });
    Vector<String> lines;
    if (tables.empty()) return lines;
    lines.push_back("=== TABLES ===");
    int number = 0;
    for (Node* table : tables) {
      ++number;
      covered->insert(table->getRef());
      for (const String& ref : Descendants(table->getRef())) covered->insert(ref);
      Vector<Vector<String>> rows = TableRows(table);
      wtf_size_t columns = 0;
      for (const auto& row : rows) columns = std::max(columns, row.size());
      lines.push_back("Table " + String::Number(number) + ": rows=" +
                      String::Number(rows.size()) + " columns=" + String::Number(columns));
      wtf_size_t shown = std::min<wtf_size_t>(rows.size(), kMaxTableRows);
      for (wtf_size_t i = 0; i < shown; ++i) lines.push_back("  " + Join(rows[i], " | "));
      if (rows.size() > shown)
        lines.push_back("  [table rows hidden: " + String::Number(rows.size() - shown) + " more]");
    }
    return lines;
  }

  bool IsLowValueContent(const String& text) const {
    static const base::NoDestructor<re2::RE2> pattern(
        "(?i)^(?:folder|new|loading|loading[ .…]*|loading history[ .…]*|shift \\+ return|last updated\\b.*|press ctrl\\b.*)$");
    return RegexFullMatch(Clean(text), *pattern);
  }

  bool MatchesActionLabel(const String& text) const {
    String key = NormKey(text);
    if (key.empty()) return false;
    if (action_labels_.Contains(key)) return true;
    for (const String& label : action_labels_) if (label.contains(key)) return true;
    return false;
  }

  bool IsContentNode(Node* node, const HashSet<String>& covered) const {
    String ref = node->getRef();
    if (covered.Contains(ref) || suppressed_refs_.Contains(ref)) return false;
    String tag = NodeTag(node), role = NodeRole(node);
    String semantic = node->hasSemanticBoundary()
                          ? PythonLower(Clean(
                                node->getSemanticBoundary(String()), false))
                          : String();
    String text = Text(node);
    if (text.empty() || IsLowValueContent(text)) return false;
    if (!kIncludeOffscreenContent && !node->getInViewport()) return false;
    if (kIncludeOffscreenContent && !node->getInViewport() && ActionTypes(node).empty() &&
        HasAncestorRoleOrTag(node, {"nav", "aside", "navigation", "toolbar", "header", "footer", "menu"}))
      return false;
    if (IsOneOf(tag, {"html", "body", "table", "thead", "tbody", "tfoot", "tr", "td", "th",
                      "img", "picture", "video", "audio", "canvas"})) return false;
    if (!ActionTypes(node).empty() &&
        !IsOneOf(tag, {"article", "blockquote", "caption", "dd", "dt", "figcaption", "h1", "h2", "h3", "h4", "h5", "h6", "li", "p", "pre", "summary"}) &&
        !IsOneOf(role, {"heading", "paragraph", "listitem"})) return false;
    if (IsOneOf(tag, {"article", "blockquote", "caption", "dd", "dt", "figcaption", "h1", "h2", "h3", "h4", "h5", "h6", "li", "p", "pre", "summary"}) ||
        IsOneOf(role, {"heading", "paragraph", "listitem", "alertdialog", "dialog"}) || semantic == "listitem") return true;
    String direct = node->hasDirectText() ? Clean(node->getDirectText(String())) : String();
    if (direct.empty()) return false;
    return !(CodePointLength(direct) <= kChromeLabelMaxChars &&
             MatchesActionLabel(direct) && !ContainsPythonDigit(text));
  }

  bool HasRenderedAncestor(Node* node, const HashSet<String>& rendered) const {
    HashSet<String> walked;
    while (node && node->hasParentRef()) {
      String ref = node->getParentRef(String());
      if (walked.Contains(ref)) break;
      walked.insert(ref);
      if (rendered.Contains(ref)) return true;
      node = FindNode(ref);
    }
    return false;
  }

  std::optional<ParsedMessage> ParseMessage(const String& text) const {
    String raw = Clean(text);
    std::string author_utf8, time_utf8, body_utf8;
    static const base::NoDestructor<re2::RE2> message(
        "(?i)^([A-Z][A-Za-z0-9 ._'-]{1,80}?)\\s+"
        "(\\d{1,2}:\\d{2}\\s*(?:AM|PM))\\s+(.+)$");
    if (!re2::RE2::FullMatch(raw.Utf8().c_str(), *message, &author_utf8,
                             &time_utf8, &body_utf8))
      return std::nullopt;
    ParsedMessage result;
    result.author = Clean(String::FromUtf8(author_utf8));
    result.time = Clean(String::FromUtf8(time_utf8));
    String body = Clean(String::FromUtf8(body_utf8));

    std::string body_bytes = body.Utf8().c_str();
    re2::StringPiece groups[3];
    static const base::NoDestructor<re2::RE2> thread(
        "(?i)\\b(\\d+\\s+repl(?:y|ies))"
        "(?:\\s+((?:Last reply\\s+)?[^|]{1,50}?))?"
        "\\s+View thread\\b");
    if (thread->Match(body_bytes, 0, body_bytes.size(), re2::RE2::UNANCHORED,
                      groups, 3)) {
      String count = Clean(String::FromUtf8(std::string(groups[1])));
      String last = Clean(String::FromUtf8(std::string(groups[2])));
      result.thread = last.empty() ? count : count + ", " + last;
      size_t start = static_cast<size_t>(groups[0].data() - body_bytes.data());
      size_t end = start + groups[0].size();
      body = Clean(String::FromUtf8(body_bytes.substr(0, start) + " " +
                                    body_bytes.substr(end)));
      wtf_size_t space = body.rfind(' ');
      if (space != kNotFound) {
        String token = StripPythonWhitespace(body.substr(space + 1));
        if (IsOneOf(token, {"+1", "👍", "👀", "🙏", "🎉", "🔥", "✅", "❌"})) {
          result.reactions = token;
          body = Clean(body.substr(0, space));
        }
      }
    }
    result.text = body;
    return result;
  }

  static bool MediaFileLabel(const String& label) {
    static const base::NoDestructor<re2::RE2> pattern(
        "(?i)\\.(?:pdf|docx?|xlsx?|pptx?|csv|txt|png|jpe?g|gif|webp|svg)"
        "(?:$|[^\\p{L}\\p{N}_])");
    return RegexSearch(label, *pattern);
  }

  static bool FullFileLabel(const String& label) {
    static const base::NoDestructor<re2::RE2> pattern(
        "(?i)^[\\p{L}\\p{N}_@()+.-]+(?:"
        "[\\x{0009}-\\x{000D}\\x{001C}-\\x{0020}\\x{0085}"
        "\\x{00A0}\\x{1680}\\x{2000}-\\x{200A}\\x{2028}"
        "\\x{2029}\\x{202F}\\x{205F}\\x{3000}]+-"
        "[\\x{0009}-\\x{000D}\\x{001C}-\\x{0020}\\x{0085}"
        "\\x{00A0}\\x{1680}\\x{2000}-\\x{200A}\\x{2028}"
        "\\x{2029}\\x{202F}\\x{205F}\\x{3000}]+"
        "[\\p{L}\\p{N}_@()+.-]+)*"
        "\\.(?:pdf|docx?|xlsx?|pptx?|csv|txt|png|jpe?g|gif|webp|svg)$");
    return RegexFullMatch(label, *pattern);
  }

  static Vector<String> FileLabelsInText(const String& text) {
    Vector<String> labels;
    std::string bytes = Clean(text).Utf8().c_str();
    struct Span { size_t start; size_t end; };
    Vector<Span> occupied;
    static const base::NoDestructor<re2::RE2> spaced_file_pattern(
        "(?i)(?:^|[^\\p{L}\\p{N}_.-])"
        "([\\p{L}\\p{N}_@()+.-]+(?:"
        "[\\x{0009}-\\x{000D}\\x{001C}-\\x{0020}\\x{0085}"
        "\\x{00A0}\\x{1680}\\x{2000}-\\x{200A}\\x{2028}"
        "\\x{2029}\\x{202F}\\x{205F}\\x{3000}]+-"
        "[\\x{0009}-\\x{000D}\\x{001C}-\\x{0020}\\x{0085}"
        "\\x{00A0}\\x{1680}\\x{2000}-\\x{200A}\\x{2028}"
        "\\x{2029}\\x{202F}\\x{205F}\\x{3000}]+"
        "[\\p{L}\\p{N}_@()+.-]+)+"
        "\\.(?:pdf|docx?|xlsx?|pptx?|csv|txt|png|jpe?g|gif|webp|svg))"
        "(?:$|[^\\p{L}\\p{N}_])");
    static const base::NoDestructor<re2::RE2> file_token_pattern(
        "(?i)(?:^|[^\\p{L}\\p{N}_.-])"
        "([\\p{L}\\p{N}_@()+.-]+"
        "\\.(?:pdf|docx?|xlsx?|pptx?|csv|txt|png|jpe?g|gif|webp|svg))"
        "(?:$|[^\\p{L}\\p{N}_])");
    const re2::RE2* patterns[] = {spaced_file_pattern.get(),
                                  file_token_pattern.get()};
    for (const re2::RE2* pattern : patterns) {
      size_t position = 0;
      while (position <= bytes.size()) {
        re2::StringPiece groups[2];
        if (!pattern->Match(bytes, position, bytes.size(),
                            re2::RE2::UNANCHORED, groups, 2))
          break;
        size_t start = static_cast<size_t>(groups[1].data() - bytes.data());
        size_t end = start + groups[1].size();
        bool overlaps = false;
        for (const Span& old : occupied)
          if (!(end <= old.start || start >= old.end)) { overlaps = true; break; }
        if (!overlaps) {
          occupied.push_back(Span{start, end});
          labels.push_back(String::FromUtf8(std::string(groups[1])));
        }
        position = std::max(end, position + 1);
      }
    }
    return labels;
  }

  String NormalizeAttachmentLabel(const String& raw) const {
    String value = Clean(raw);
    if (value.ends_with("Canvas") && value != "Canvas" &&
        value.length() > 6) {
      UChar before = value[value.length() - 7];
      if ((before >= 'A' && before <= 'Z') || (before >= 'a' && before <= 'z') ||
          (before >= '0' && before <= '9'))
        value = value.substr(0, value.length() - 6) + " Canvas";
    }
    return Clean(value);
  }

  Vector<String> AttachmentLabels(const String& text,
                                  const Vector<ActionRow>& controls) const {
    Vector<String> labels = FileLabelsInText(text);
    HashSet<String> seen;
    for (const String& label : labels) seen.insert(NormKey(label));
    static const base::NoDestructor<re2::RE2> canvas("(?i)\\bcanvas\\b");
    for (const ActionRow& control : controls) {
      String label = NormalizeAttachmentLabel(Text(control.node));
      String lower = StripPythonWhitespace(PythonLower(label));
      if (label.empty() || lower == "toggle file") continue;
      bool attachment = MediaFileLabel(label) ||
                        IsOneOf(NodeTag(control.node), {"img", "picture", "video", "audio", "canvas"});
      bool canvas_attachment = RegexSearch(lower, *canvas) && lower.ends_with("canvas");
      bool short_add = (lower.starts_with("add ") || lower.starts_with("create "));
      if (short_add) {
        int words = 1;
        for (wtf_size_t i = 0; i < lower.length(); ++i) if (lower[i] == ' ') ++words;
        short_add = words <= 4;
      }
      attachment = attachment || (canvas_attachment && lower != "canvas" && !short_add);
      if (!attachment) continue;
      String key = NormKey(label);
      if (!key.empty() && seen.Contains(key)) continue;
      seen.insert(key);
      labels.push_back(label);
    }
    return labels;
  }

  String TextWithoutAttachments(const String& text,
                                const Vector<String>& attachments) const {
    String stripped = Clean(text);
    for (const String& label : attachments) {
      if (!FullFileLabel(label))
        continue;
      wtf_size_t search_from = 0;
      while (search_from <= stripped.length()) {
        wtf_size_t position = stripped.find(label, search_from);
        if (position == kNotFound)
          break;
        wtf_size_t end = position + label.length();
        bool valid_before =
            position == 0 ||
            !IsAttachmentBoundaryCharacter(CodePointBefore(stripped, position));
        wtf_size_t next = end;
        bool valid_after = true;
        if (end < stripped.length()) {
          UChar32 after = CodePointAt(stripped, end, &next);
          valid_after = !IsAttachmentBoundaryCharacter(after);
        }
        if (valid_before && valid_after) {
          stripped = stripped.substr(0, position) + " " +
                     stripped.substr(end);
          break;
        }
        search_from = next;
      }
    }
    std::string bytes = stripped.Utf8().c_str();
    static const base::NoDestructor<re2::RE2> count(
        "(?i)\\b\\d+\\s+files?\\s+Download all\\b");
    re2::RE2::GlobalReplace(&bytes, *count, " ");
    stripped = Clean(String::FromUtf8(bytes));
    return stripped.empty() ? Clean(text) : stripped;
  }

  Vector<String> FormatContent(const String& text,
                               const String& prefix,
                               const Vector<ActionRow>& controls) const {
    Vector<String> attachments = AttachmentLabels(text, controls);
    std::optional<ParsedMessage> parsed = ParseMessage(text);
    Vector<String> lines;
    if (!parsed.has_value()) {
      lines.push_back(prefix + " " + Clip(text, std::min(kMaxTextChars, 360)));
      if (!attachments.empty())
        lines.push_back("  attachments: " + Join(attachments, ", "));
      return lines;
    }
    lines.push_back(prefix + " " + parsed->author + " " + parsed->time);
    if (!parsed->text.empty())
      lines.push_back("  text: " + Clip(TextWithoutAttachments(parsed->text, attachments),
                                        std::min(kMaxTextChars, 320)));
    if (!attachments.empty())
      lines.push_back("  attachments: " + Join(attachments, ", "));
    if (!parsed->reactions.empty()) {
      String reaction = parsed->reactions;
      reaction.Replace("+1", "+1 emoji");
      lines.push_back("  reactions: " + reaction);
    }
    if (!parsed->thread.empty())
      lines.push_back("  thread: " + parsed->thread);
    return lines;
  }

  Vector<ScrollRegionRow> ScrollRegionRows() const {
    Vector<ScrollRegionRow> rows;
    for (Node* node : nodes_) {
      if (suppressed_refs_.Contains(node->getRef()) || !node->getScrollable() ||
          !node->getVisible() || !node->getInViewport() || !node->hasBounds())
        continue;
      auto* bounds = node->getBounds(nullptr);
      double area = bounds->getWidth() * bounds->getHeight();
      if (area <= 0 || bounds->getHeight() < 320) continue;
      rows.push_back(ScrollRegionRow{area, node});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
      return a.area > b.area;
    });
    return rows;
  }

  String ScrollRegionLabel(Node* node) const {
    String label = node->hasAccessibleName() ? Clean(node->getAccessibleName(String())) : String();
    if (label.empty() && node->hasDescription()) label = Clean(node->getDescription(String()));
    if (label.empty() && node->hasRole()) label = Clean(node->getRole(String()), false);
    if (label.empty()) label = Clean(node->getTag(), false);
    if (label.empty()) label = "region";
    String key = StripPythonWhitespace(PythonLower(label));
    if (IsOneOf(key, {"", "document", "generic", "group", "home", "jump to date", "list",
                      "listitem", "main", "none", "tabpanel", "toolbar"})) {
      String context = DisplayContext(node, 60);
      label = context.empty() ? String("page content") : context + " content";
    }
    return Clip(label, 80);
  }

  Vector<String> RenderContent(const HashSet<String>& covered,
                               HashSet<String>* rendered_refs) {
    Vector<Node*> candidates;
    for (Node* node : nodes_) if (IsContentNode(node, covered)) candidates.push_back(node);
    std::stable_sort(candidates.begin(), candidates.end(), [](Node* a, Node* b) {
      return a->getSourceOrder() < b->getSourceOrder();
    });
    Vector<String> visible, offscreen;
    HashSet<String> whole, seen;
    String visible_context, offscreen_context;
    for (wtf_size_t index = 0; index < candidates.size(); ++index) {
      Node* node = candidates[index];
      if (HasRenderedAncestor(node, whole)) continue;
      String full = Text(node), clipped = Clip(full, kMaxTextChars), ref = node->getRef();
      auto children_it = children_.find(ref);
      bool is_leaf =
          children_it == children_.end() || children_it->value.empty();
      if (!ref.empty() && is_leaf && !node->getTruncated() &&
          CodePointLength(clipped) >= CodePointLength(full))
        whole.insert(ref);
      String key = NormKey(clipped);
      String dedupe_key = SemanticOwnerRef(node) + "\n" + key;
      if (key.empty() || seen.Contains(dedupe_key)) continue;
      seen.insert(dedupe_key);
      rendered_content_text_keys_.insert(key);
      rendered_refs->insert(ref);
      bool is_visible = node->getVisible() && node->getInViewport();
      Vector<String>& target = is_visible ? visible : offscreen;
      String context = DisplayContext(node);
      String& last_context = is_visible ? visible_context : offscreen_context;
      if (!context.empty() && context != last_context) {
        target.push_back("Context: " + context);
        last_context = context;
      }
      Vector<String> node_actions = ActionTypes(node);
      auto controls_it = nested_actions_.find(ref);
      Vector<ActionRow> controls = controls_it == nested_actions_.end()
                                       ? Vector<ActionRow>() : controls_it->value;
      Vector<ActionRow> useful;
      for (const ActionRow& control : controls) {
        if (!LowValueNestedControl(control.node, control.actions, full)) useful.push_back(control);
      }
      String prefix = "-";
      static const base::NoDestructor<re2::RE2> date("(?i)^\\d{1,2}\\s+[A-Za-z]{3,9}\\s+\\d{4}$");
      if (is_visible && NodeRole(node) != "listitem" && !RegexFullMatch(clipped, *date) &&
          !node_actions.empty() && IsPrimaryAction(node, node_actions) && useful.empty())
        prefix = "- [" + ActionId(node) + "]";
      Vector<String> content_lines = FormatContent(full, prefix, controls);
      for (const String& line : content_lines)
        target.push_back(line);
      for (const String& evidence :
           AdditionalContentEvidence(node, content_lines)) {
        target.push_back("  evidence: " + evidence);
      }
      if (!useful.empty()) {
        Vector<String> control_lines;
        wtf_size_t shown = std::min<wtf_size_t>(useful.size(), kMaxNestedActions);
        for (wtf_size_t i = 0; i < shown; ++i)
          control_lines.push_back(ActionLine(useful[i].node, useful[i].actions));
        target.push_back("  actions: " + Join(control_lines, "; "));
        if (useful.size() > shown)
          target.push_back("  [actions hidden: " + String::Number(useful.size() - shown) + " more]");
      }
    }
    Vector<String> lines;
    if (!visible.empty()) {
      lines.push_back("=== VISIBLE CONTENT ===");
      for (const String& line : visible)
        lines.push_back(line);
    }
    if (!offscreen.empty()) {
      lines.push_back("=== ADDITIONAL CAPTURED CONTENT ===");
      Vector<ScrollRegionRow> scroll = ScrollRegionRows();
      if (!scroll.empty())
        lines.push_back("Not currently clickable. To interact with these rows, scroll [" +
                        ActionId(scroll[0].node) + "] " + ScrollRegionLabel(scroll[0].node) + ".");
      else
        lines.push_back("Not currently clickable. Use page scroll to bring this content into view.");
      for (const String& line : offscreen)
        lines.push_back(line);
    }
    return lines;
  }

  Vector<String> RenderMedia(const HashSet<String>& covered) const {
    Vector<Node*> ordered = nodes_;
    std::stable_sort(ordered.begin(), ordered.end(), [](Node* a, Node* b) {
      return a->getSourceOrder() < b->getSourceOrder();
    });
    Vector<String> lines;
    HashSet<String> seen;
    int count = 0;
    for (Node* node : ordered) {
      if (covered.Contains(node->getRef())) continue;
      String tag = NodeTag(node);
      if (!IsOneOf(tag, {"img", "picture", "video", "audio", "canvas"})) continue;
      HashMap<String, String> attrs = AttrMap(node);
      String label = Text(node);
      for (const char* key : {"alt", "title", "src"}) {
        if (!label.empty()) break;
        auto it = attrs.find(key); if (it != attrs.end()) label = it->value;
      }
      if (label.empty()) continue;
      String label_key = NormKey(label);
      bool represented = false;
      for (const String& key : rendered_content_text_keys_) {
        if (!label_key.empty() && (label_key == key || key.contains(label_key) || label_key.contains(key))) {
          represented = true; break;
        }
      }
      if (represented) continue;
      String key = tag + ":" + label_key;
      if (seen.Contains(key)) continue;
      seen.insert(key);
      if (lines.empty()) lines.push_back("=== MEDIA / IMAGE LABELS ===");
      Vector<String> parts = {"- " + JsonQuote(Clip(label, 120))};
      if (HasNonEmptyAttribute(attrs, "src")) parts.push_back("source=present");
      String context = DisplayContext(node, 60);
      if (!context.empty()) parts.push_back("context=" + JsonQuote(context));
      lines.push_back(Join(parts, " "));
      if (++count >= std::min(kMaxMedia, 20)) {
        lines.push_back("[media hidden: more than " + String::Number(std::min(kMaxMedia, 20)) + " items]");
        break;
      }
    }
    return lines;
  }

  Vector<String> RenderActions(const HashSet<String>& rendered) const {
    HashSet<String> nested, descendants;
    for (const String& item_ref : rendered) {
      auto it = nested_actions_.find(item_ref);
      if (it != nested_actions_.end())
        for (const ActionRow& row : it->value) nested.insert(row.node->getRef());
      for (const String& ref : Descendants(item_ref)) descendants.insert(ref);
    }
    Vector<ActionRow> primary;
    struct Secondary { ActionRow row; String reason; };
    Vector<Secondary> secondary;
    HashSet<String> seen_primary, seen_secondary;
    for (const ActionRow& row : action_nodes_) {
      String ref = row.node->getRef();
      if (nested.Contains(ref) || rendered.Contains(ref) || descendants.Contains(ref)) continue;
      String label_key = NormKey(Text(row.node));
      String key;
      if (!row.node->getHitTestable() && !label_key.empty())
        key = String("not-hit:") + label_key;
      else
        key = ActionId(row.node) + ":" + Join(row.actions, ",") + ":" + label_key;
      if (IsPrimaryAction(row.node, row.actions)) {
        if (seen_primary.Contains(key)) continue;
        seen_primary.insert(key); primary.push_back(row);
      } else {
        if (seen_secondary.Contains(key)) continue;
        seen_secondary.insert(key);
        String reason = "broad/noisy";
        if (Contains(row.actions, "click") && !row.node->getHitTestable()) reason = "not_hit_testable";
        else if (Text(row.node).empty()) reason = "unlabeled";
        secondary.push_back(Secondary{row, reason});
      }
    }
    Vector<String> lines;
    if (!primary.empty()) {
      std::stable_sort(primary.begin(), primary.end(), [this](const auto& a, const auto& b) {
        return ActionPriority(a.node, a.actions) < ActionPriority(b.node, b.actions);
      });
      Vector<ActionRow> compact;
      for (const auto& row : primary)
        if (ActionPriority(row.node, row.actions).first <= 3) compact.push_back(row);
      if (compact.empty()) compact = primary;
      int limit = std::min(kMaxActions, 25);
      lines.push_back("=== USEFUL PAGE ACTIONS ===");
      wtf_size_t shown = std::min<wtf_size_t>(compact.size(), limit);
      HashSet<String> shown_refs;
      for (wtf_size_t i = 0; i < shown; ++i) {
        lines.push_back(ActionLine(compact[i].node, compact[i].actions));
        shown_refs.insert(compact[i].node->getRef());
      }
      wtf_size_t hidden = primary.size() - std::min(primary.size(), shown);
      if (hidden) lines.push_back("[other actions hidden: " + String::Number(hidden) + "]");

      Vector<String> read_only_rows;
      HashSet<String> seen_read_only;
      for (const ActionRow& row : primary) {
        if (shown_refs.Contains(row.node->getRef()))
          continue;
        Vector<String> facts = {Text(row.node)};
        for (const auto& state : *row.node->getStates()) {
          String name = Clean(state->getName(), false);
          String value = Clean(state->getValue());
          if (!name.empty() && !value.empty())
            facts.push_back(name + "=" + value);
        }
        Vector<String> non_empty_facts;
        for (const String& fact : facts) {
          if (!fact.empty())
            non_empty_facts.push_back(fact);
        }
        String fact = Clean(Join(non_empty_facts, "; "));
        String fact_key = NormKey(fact);
        if (fact_key.empty() || seen_read_only.Contains(fact_key))
          continue;
        bool represented = false;
        for (const String& content_key : rendered_content_text_keys_) {
          if (fact_key == content_key || content_key.contains(fact_key)) {
            represented = true;
            break;
          }
        }
        if (represented)
          continue;
        seen_read_only.insert(fact_key);
        read_only_rows.push_back("- " + fact);
      }
      if (!read_only_rows.empty()) {
        lines.push_back("=== READ-ONLY CONTROL EVIDENCE ===");
        for (const String& row : read_only_rows)
          lines.push_back(row);
      }
    }
    if (kIncludeSecondary && !secondary.empty()) {
      lines.push_back("=== SECONDARY / DEBUG ACTIONS ===");
      wtf_size_t shown = std::min<wtf_size_t>(secondary.size(), kMaxSecondaryActions);
      for (wtf_size_t i = 0; i < shown; ++i)
        lines.push_back(ActionLine(secondary[i].row.node, secondary[i].row.actions, true) +
                        " reason=" + secondary[i].reason);
      if (secondary.size() > shown)
        lines.push_back("[secondary actions hidden: " + String::Number(secondary.size() - shown) + " more]");
    }

    Vector<String> complete_control_text;
    HashSet<String> seen_complete;
    for (const ActionRow& row : action_nodes_) {
      String full = Text(row.node);
      if (CodePointLength(full) <= 220)
        continue;
      String key = NormKey(full);
      if (key.empty() || seen_complete.Contains(key))
        continue;
      seen_complete.insert(key);
      complete_control_text.push_back("- " + full);
    }
    if (!complete_control_text.empty()) {
      lines.push_back("=== COMPLETE READ-ONLY CONTROL TEXT ===");
      for (const String& row : complete_control_text)
        lines.push_back(row);
    }
    return lines;
  }

  Vector<String> RenderScrollRegions() const {
    Vector<ScrollRegionRow> regions = ScrollRegionRows();
    Vector<String> lines;
    if (regions.empty()) return lines;
    lines = {"=== SCROLLABLE REGIONS ===",
             "Use: scroll(\"down\", 1600, element_id=<id>) or scroll(\"up\", 1600, element_id=<id>)."};
    wtf_size_t shown = std::min<wtf_size_t>(regions.size(), kMaxScrollRegions);
    for (wtf_size_t i = 0; i < shown; ++i) {
      Node* node = regions[i].node;
      auto* bounds = node->getBounds(nullptr);
      String role = NodeRole(node);
      String role_text = !role.empty() && !IsOneOf(role, {"generic", "none"})
                             ? " (" + role + ")" : String();
      String size = String::Number(static_cast<int>(bounds->getWidth())) + "x" +
                    String::Number(static_cast<int>(bounds->getHeight()));
      lines.push_back("[" + ActionId(node) + "] " + ScrollRegionLabel(node) +
                      role_text + " " + CodePoints({0x2014}) + " " + size);
    }
    if (regions.size() > shown)
      lines.push_back("[scrollable regions hidden: " + String::Number(regions.size() - shown) + " more]");
    return lines;
  }

  protocol::ChromiumRL::StructuredPageSnapshot* snapshot_ = nullptr;
  Vector<Node*> nodes_;
  HashMap<String, Node*> by_ref_;
  HashMap<String, Vector<Node*>> children_;
  HashSet<String> suppressed_refs_;
  Vector<ActionRow> action_nodes_;
  HashSet<String> action_labels_;
  HashMap<String, Vector<ActionRow>> nested_actions_;
  HashSet<String> rendered_content_text_keys_;
  HashSet<String> repeated_item_refs_;
  mutable HashMap<Node*, String> text_cache_;
  mutable HashMap<Node*, String> tag_cache_;
  mutable HashMap<Node*, String> role_cache_;
  mutable HashMap<Node*, Vector<String>> action_types_cache_;
};

std::unique_ptr<protocol::ChromiumRL::ModelDOM>
StructuredModelDOMRenderer::Render() {
  auto sections =
      std::make_unique<protocol::Array<protocol::ChromiumRL::ModelDOMSection>>();
  auto append_section = [&sections](const String& name,
                                    const Vector<String>& lines) {
    if (lines.empty())
      return;
    auto protocol_lines = std::make_unique<protocol::Array<String>>();
    for (const String& line : lines)
      protocol_lines->push_back(line);
    sections->push_back(protocol::ChromiumRL::ModelDOMSection::create()
                            .setName(name)
                            .setLines(std::move(protocol_lines))
                            .build());
  };

  append_section("header", RenderHeader());
  HashSet<String> covered;
  Vector<String> tables = RenderTables(&covered);
  append_section("tables", tables);
  HashSet<String> rendered;
  Vector<String> content = RenderContent(covered, &rendered);
  append_section("content", content);
  HashSet<String> media_covered = covered;
  for (const String& ref : rendered) {
    media_covered.insert(ref);
    for (const String& child : Descendants(ref))
      media_covered.insert(child);
  }
  Vector<String> media = RenderMedia(media_covered);
  append_section("media", media);
  Vector<String> actions = RenderActions(rendered);
  append_section("actions", actions);
  Vector<String> scroll = RenderScrollRegions();
  append_section("scroll_regions", scroll);

  return protocol::ChromiumRL::ModelDOM::create()
      .setRendererName("chromiumrl-model-dom")
      .setSections(std::move(sections))
      .build();
}

}  // namespace

std::unique_ptr<protocol::ChromiumRL::ModelDOM>
InspectorChromiumRLAgent::BuildModelDOM(
    protocol::ChromiumRL::StructuredPageSnapshot* snapshot) {
  return StructuredModelDOMRenderer(snapshot).Render();
}

protocol::Response InspectorChromiumRLAgent::getModelDOM(
    std::unique_ptr<protocol::ChromiumRL::StructuredPageSnapshot> snapshot,
    std::unique_ptr<protocol::ChromiumRL::ModelDOM>* model_dom) {
  if (!snapshot)
    return protocol::Response::InvalidParams("snapshot is required");
  *model_dom = BuildModelDOM(snapshot.get());
  return protocol::Response::Success();
}


}  // namespace blink
