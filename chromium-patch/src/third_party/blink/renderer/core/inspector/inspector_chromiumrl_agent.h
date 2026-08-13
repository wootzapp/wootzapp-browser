// Copyright 2025 The Chromium Authors
// File: third_party/blink/renderer/core/inspector/inspector_chromiumrl_agent.h

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTOR_CHROMIUMRL_AGENT_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTOR_CHROMIUMRL_AGENT_H_

#include "base/memory/scoped_refptr.h"
#include <optional>
#include "ui/gfx/geometry/rect_f.h"
#include "third_party/blink/public/common/input/web_pointer_event.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/core/inspector/inspector_base_agent.h"
#include "third_party/blink/renderer/core/inspector/protocol/chromium_rl.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/layout/layout_shift_tracker.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/core/dom/mutation_observer.h"
#include "third_party/blink/renderer/core/inspector/chromiumrl_trace_buffer.h"
#include "third_party/blink/renderer/core/inspector/inspector_session_state.h"

namespace cc {
class Layer;
class LayerTreeHost;
}

namespace blink {

class InspectedFrames;

// =============================================================================
// Inspector Agent - handles CDP commands
// =============================================================================

class CORE_EXPORT InspectorChromiumRLAgent final
    : public InspectorBaseAgent<protocol::ChromiumRL::Metainfo> {
  class DOMDiffObserver;

 public:
  explicit InspectorChromiumRLAgent(InspectedFrames* inspected_frames);
  ~InspectorChromiumRLAgent() override;

  // InspectorBaseAgent overrides
  void Restore() override;
  void DidCommitLoadForLocalFrame(LocalFrame* frame) override;

  // Protocol command handlers
  protocol::Response enable(
      std::optional<bool> capture_touch_traces,
      std::optional<bool> capture_layout_timings,
      std::optional<bool> capture_cls_attribution,
      std::optional<bool> capture_compositor_layers,
      String* out_session_id) override;

  protocol::Response disable() override;

  protocol::Response getTouchTraces(
      std::optional<int> root_node_id,
      std::optional<double> start_time,
      std::optional<double> end_time,
      std::unique_ptr<protocol::Array<protocol::ChromiumRL::TouchTrace>>* traces,
      int* total_events,
      double* capture_duration) override;

  protocol::Response getLayoutTimings(
      const String& frame_id,
      std::optional<double> threshold_ms,
      std::optional<bool> include_zero_time,
      std::unique_ptr<protocol::Array<protocol::ChromiumRL::ElementLayoutTiming>>* elements,
      std::unique_ptr<protocol::ChromiumRL::FrameLayoutMetrics>* frame_metrics) override;

  protocol::Response getCLSAttribution(
      std::optional<double> min_shift_score,
      std::unique_ptr<protocol::Array<protocol::ChromiumRL::CLSEntry>>* entries,
      double* total_cls,
      bool* exceeds_good_threshold) override;

  protocol::Response getCompositorLayers(
      std::optional<bool> include_paint_info,
      std::optional<bool> include_transforms,
      std::unique_ptr<protocol::Array<protocol::ChromiumRL::CompositorLayer>>* layers,
      int* total_gpu_memory,
      int* pending_texture_uploads) override;

  protocol::Response captureInteraction(
      const String& interaction_type,
      int target_node_id,
      std::optional<double> capture_duration_ms,
      std::unique_ptr<protocol::Array<protocol::ChromiumRL::TouchTrace>>* touch_traces,
      std::unique_ptr<protocol::Array<protocol::ChromiumRL::ElementLayoutTiming>>* layout_timings,
      std::unique_ptr<protocol::Array<protocol::ChromiumRL::CLSEntry>>* cls_entries,
      std::unique_ptr<protocol::Array<protocol::ChromiumRL::CompositorLayer>>* compositor_layers) override;

  protocol::Response getVisualHash(String* visual_hash,
                                 bool* has_visual_update) override;

  protocol::Response startDOMDiff() override;
  protocol::Response stopDOMDiff() override;

  protocol::Response captureStateSnapshot(
      std::unique_ptr<protocol::Array<protocol::ChromiumRL::SnapshotNode>>* snapshot) override;

  protocol::Response computeStateDiff(
      std::unique_ptr<protocol::Array<protocol::ChromiumRL::SnapshotNode>> target_snapshot,
      std::unique_ptr<protocol::ChromiumRL::SnapshotDiff>* diff) override;

  // Advanced DOM Diffing
  protocol::Response saveDOMState(
      std::unique_ptr<protocol::ChromiumRL::RichDOMState>* state) override;

  protocol::Response compareDOMState(
      std::unique_ptr<protocol::ChromiumRL::RichDOMState> reference_state,
      std::unique_ptr<protocol::Array<protocol::ChromiumRL::DeltaNode>> delta_nodes,
      std::unique_ptr<protocol::ChromiumRL::DOMDiffResult>* result) override;

  protocol::Response getAgentObservation(
      std::optional<bool> in_viewport_only,
      std::optional<String> root_selector,
      std::optional<int> max_elements,
      std::optional<bool> include_content,
      std::optional<bool> include_diff,
      std::optional<bool> update_baseline,
      std::optional<int> max_interactive_elements,
      std::optional<int> max_content_blocks,
      std::optional<int> max_diff_items,
      std::unique_ptr<protocol::ChromiumRL::AgentObservation>* observation) override;

  protocol::Response captureStructuredSnapshot(
      std::optional<bool> in_viewport_only,
      std::optional<String> root_selector,
      std::optional<int> max_nodes,
      std::optional<int> max_text_chars,
      std::optional<bool> include_offscreen,
      std::optional<bool> include_diff,
      std::optional<bool> update_baseline,
      std::unique_ptr<protocol::ChromiumRL::StructuredPageSnapshot>* snapshot) override;

  // Callbacks from browser internals (hook these into Chromium)
  void OnTouchEvent(const blink::WebPointerEvent& event, Node* target_node);
  void OnLayoutComplete(LocalFrame* frame);
  void OnLayoutShift(double score,
                     bool had_recent_input,
                     const Vector<LayoutShiftTracker::Attribution>& attributions);
  void OnCompositorCommit(const cc::LayerTreeHost* layer_tree_host);
  void OnDOMMutation(const HeapVector<Member<MutationRecord>>& records);

  static InspectorChromiumRLAgent* GetAgentForFrame(LocalFrame* frame);

  void Trace(Visitor* visitor) const override;

 private:
  void RegisterAgent();
  void UnregisterAgent();

  // Build protocol objects from internal entries
  std::unique_ptr<protocol::ChromiumRL::TouchTrace> 
      BuildTouchTrace(const TouchTraceEntry& entry);
  std::unique_ptr<protocol::ChromiumRL::ElementLayoutTiming> 
      BuildLayoutTiming(const LayoutTimingEntry& entry);
  std::unique_ptr<protocol::ChromiumRL::CLSEntry> 
      BuildCLSEntry(const CLSTraceEntry& entry);
  std::unique_ptr<protocol::ChromiumRL::CompositorLayer> 
      BuildCompositorLayer(const cc::Layer* layer, bool paint_info, bool transforms);

  std::unique_ptr<protocol::ChromiumRL::SnapshotNode> BuildSnapshotNode(Node* node);
  std::unique_ptr<protocol::ChromiumRL::RichNode> BuildRichNode(Node* node);
  std::unique_ptr<protocol::ChromiumRL::RichNode> BuildLightweightNode(Node* node);
  static double CalculateIoU(protocol::DOM::Rect* r1, protocol::DOM::Rect* r2);
  
  String GenerateSessionId();
  String BuildSelectorPath(Node* node);
  String BuildXPath(Node* node);
  String GetNodeFingerprint(Node* node);
  String NormalizeTextContent(const String& text);
  LocalFrame* FrameForId(const String& frame_id);
  void EnableInstrumentation();
  void DisableInstrumentation();

  bool IsInteractiveElement(Element* element);
  bool IsContentElement(Element* element);
  bool IsSemanticBoundary(Element* element);
  bool IsElementInViewport(const gfx::RectF& box,
                           double scroll_top,
                           double viewport_width,
                           double viewport_height);
  bool IsElementHitTestable(Element* element, const gfx::RectF& box);
  String GetElementContext(Element* element);
  String GetAriaRoleName(Element* element);
  String GetAccessibleName(Node* node);
  String BuildObservationDedupKey(Element* element);
  bool HasHumanReadableLabel(Element* element);
  bool IsJunkContentText(const String& text);
  double ScoreInteractiveElement(Element* element,
                                 bool is_in_viewport,
                                 bool is_hit_testable);
  double ScoreContentElement(Element* element,
                             bool is_in_viewport,
                             const String& text);
  bool IsStructuredSnapshotCandidate(Node* node,
                                     bool is_visible,
                                     bool is_in_viewport,
                                     bool include_offscreen);
  bool IsDecorativeOrUnsupported(Element* element);
  bool IsScrollableElement(Element* element);
  String ComputeDirectText(Node* node, unsigned max_chars, bool* truncated);
  String ComputeSubtreeTextCapped(Node* node,
                                  unsigned max_chars,
                                  bool* truncated);
  String DetectSemanticBoundary(Element* element);
  std::unique_ptr<protocol::Array<protocol::ChromiumRL::NodeAttribute>>
      CollectSelectedAttributes(Element* element);
  std::unique_ptr<protocol::Array<protocol::ChromiumRL::NodeState>>
      CollectNodeStates(Element* element);
  std::unique_ptr<protocol::Array<String>> CollectActionTypes(
      Element* element,
      bool scrollable);
  std::unique_ptr<protocol::ChromiumRL::ObservedElement> BuildObservedElement(
      Element* element,
      int idx,
      const gfx::RectF& box,
      bool is_in_viewport,
      bool is_hit_testable);
  std::unique_ptr<protocol::ChromiumRL::PageNode> BuildPageNode(
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
      int max_text_chars);
  std::unique_ptr<protocol::ChromiumRL::TableInfo> BuildTableInfo(
      Element* element,
      String* table_text);
  std::unique_ptr<protocol::ChromiumRL::ContentBlock> BuildContentBlock(
      Element* element,
      const String& precomputed_text);

  Member<InspectedFrames> inspected_frames_;
  Member<ChromiumRLTraceBuffer> trace_buffer_;
  
  // Session state
  String current_session_id_;
  InspectorAgentState::Boolean capture_touch_traces_;
  InspectorAgentState::Boolean capture_layout_timings_;
  InspectorAgentState::Boolean capture_cls_attribution_;
  InspectorAgentState::Boolean capture_compositor_layers_;
  int last_source_frame_number_ = 0;
  InspectorAgentState::Boolean enabled_; 

  Member<MutationObserver> dom_diff_observer_;
  Member<DOMDiffObserver> dom_diff_delegate_;
  std::unique_ptr<protocol::ChromiumRL::RichDOMState> agent_observation_baseline_;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_INSPECTOR_CHROMIUMRL_AGENT_H_
