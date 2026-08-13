// Copyright 2025 The Chromium Authors
// File: third_party/blink/renderer/core/inspector/chromiumrl_trace_buffer.h

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_CHROMIUMRL_TRACE_BUFFER_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_CHROMIUMRL_TRACE_BUFFER_H_

#include "base/memory/scoped_refptr.h"
#include "third_party/blink/renderer/core/core_export.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/heap/visitor.h"
#include "third_party/blink/renderer/platform/heap/collection_support/heap_hash_map.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#include "ui/gfx/geometry/rect_f.h"

namespace cc {
class Layer;
class LayerTreeHost;
}

namespace blink {

class LocalFrame;
class LayoutView;

// =============================================================================
// Trace Entry Structures (internal storage)
// =============================================================================

struct TouchTraceEntry {
  double timestamp = 0;
  String event_type;
  double x = 0, y = 0;
  double pressure = 0;
  double radius_x = 0, radius_y = 0;
  double rotation_angle = 0;
  int node_id = 0;
  String selector_path;
  bool was_handled = false;
  bool triggered_scroll = false;
  bool triggered_navigation = false;
  int pointer_id = 0;
};

struct LayoutTimingEntry {
  int node_id = 0;
  String selector_path;
  String tag_name;
  double layout_time_ms = 0;
  int layout_invalidations = 0;
  double style_recalc_time_ms = 0;
  int style_recalc_count = 0;
  double paint_time_ms = 0;
  gfx::RectF bounding_box;
  bool is_in_critical_path = false;
  bool caused_sibling_reflow = false;
  int parent_node_id = 0;
};

struct ShiftedElementEntry {
  int node_id = 0;
  String selector_path;
  gfx::RectF previous_rect;
  gfx::RectF current_rect;
  double shift_distance = 0;
  double score_contribution = 0;
};

struct CLSTraceEntry {
  double timestamp = 0;
  double score = 0;
  bool had_recent_input = false;
  Vector<ShiftedElementEntry> shifted_elements;
  String session_window_id;
};

struct CompositorLayerEntry {
  scoped_refptr<cc::Layer> layer;
  int gpu_memory_bytes = 0;
};

struct FrameMetricsEntry {
  double total_layout_time_ms = 0;
  double total_style_recalc_time_ms = 0;
  double total_paint_time_ms = 0;
  double first_contentful_paint = 0;
  double largest_contentful_paint = 0;
  double time_to_first_byte = 0;
  double dom_content_loaded = 0;
  double total_blocking_time = 0;
  int long_task_count = 0;
};

// =============================================================================
// Trace Buffer - stores captured signals
// =============================================================================

class CORE_EXPORT ChromiumRLTraceBuffer final
    : public GarbageCollected<ChromiumRLTraceBuffer> {
 public:
  static constexpr size_t kMaxTouchTraces = 10000;
  static constexpr size_t kMaxCLSEntries = 1000;
  
  ChromiumRLTraceBuffer() = default;
  
  void Clear();
  
  // Touch traces
  void AddTouchTrace(TouchTraceEntry entry);
  const Vector<TouchTraceEntry>& GetTouchTraces() const { return touch_traces_; }
  
  // Layout timings
  void RecordLayoutComplete(LocalFrame* frame, LayoutView* layout_view, FrameMetricsEntry metrics);
void UpdateFrameMetrics(LocalFrame* frame, FrameMetricsEntry metrics);
  const Vector<LayoutTimingEntry>& GetLayoutTimings(LocalFrame* frame) const;
  const FrameMetricsEntry& GetFrameMetrics(LocalFrame* frame) const;
  
  // CLS attribution
  void AddCLSEntry(CLSTraceEntry entry);
  const Vector<CLSTraceEntry>& GetCLSEntries() const { return cls_entries_; }
  double GetTotalCLS() const { return total_cls_; }
  
  // Compositor layers
  void RecordCompositorState(const cc::LayerTreeHost* layer_tree_host);
  const Vector<CompositorLayerEntry>& GetCompositorLayers() const { 
    return compositor_layers_; 
  }
  int GetPendingTextureUploads() const { return pending_texture_uploads_; }
  
  void Trace(Visitor* visitor) const {
    visitor->Trace(layout_timings_);
    visitor->Trace(frame_metrics_);
  }
  
 private:
  Vector<TouchTraceEntry> touch_traces_;
  HeapHashMap<Member<LocalFrame>, Vector<LayoutTimingEntry>> layout_timings_;
  HeapHashMap<Member<LocalFrame>, FrameMetricsEntry> frame_metrics_;
  Vector<CLSTraceEntry> cls_entries_;
  Vector<CompositorLayerEntry> compositor_layers_;
  
  double total_cls_ = 0;
  int pending_texture_uploads_ = 0;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_INSPECTOR_CHROMIUMRL_TRACE_BUFFER_H_