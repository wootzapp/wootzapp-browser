// Copyright 2025 The Chromium Authors
// File: third_party/blink/renderer/core/inspector/chromiumrl_trace_buffer.cc

#include "base/strings/string_util.h"
#include "third_party/blink/renderer/core/inspector/chromiumrl_trace_buffer.h"

#include "base/logging.h"
#include "cc/layers/layer.h"
#include "cc/trees/layer_tree_host.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/inspector/identifiers_factory.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/dom/node.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"

namespace blink {

void ChromiumRLTraceBuffer::Clear() {
  touch_traces_.clear();
  layout_timings_.clear();
  frame_metrics_.clear();
  cls_entries_.clear();
  compositor_layers_.clear();
  total_cls_ = 0;
  pending_texture_uploads_ = 0;
}

void ChromiumRLTraceBuffer::AddTouchTrace(TouchTraceEntry entry) {
  LOG(INFO) << "ChromiumRL: [Buffer] Storing trace. Total Count: " << (touch_traces_.size() + 1);
  if (touch_traces_.size() >= kMaxTouchTraces) {
    touch_traces_.erase(touch_traces_.begin());
  }
  touch_traces_.push_back(std::move(entry));
}

namespace {

String BuildSelectorPathHelper(Node* node) {
  if (!node) return String();
  
  StringBuilder builder;
  Vector<String> path;
  Node* current = node;
  
  while (current && !current->IsDocumentNode()) {
    if (auto* element = DynamicTo<Element>(current)) {
      StringBuilder step;
      step.Append(String::FromUtf8(base::as_byte_span(base::ToLowerASCII(element->tagName().Utf8()))));
      
      if (element->HasID()) {
        step.Append("#");
        step.Append(element->GetIdAttribute());
      } else if (element->HasClass()) {
        if (element->ClassNames().size() > 0) {
            step.Append(".");
            step.Append(element->ClassNames()[0]);
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

void CollectLayoutEntries(LayoutObject* object, Vector<LayoutTimingEntry>& entries) {
  if (!object) return;
  
  // Depth-first traversal
  for (LayoutObject* child = object->SlowFirstChild(); child; child = child->NextSibling()) {
    CollectLayoutEntries(child, entries);
  }
  
  // Process current
  Node* node = object->GetNode();
  if (node && node->IsElementNode()) {
    LayoutTimingEntry entry;
    entry.node_id = IdentifiersFactory::IntIdForNode(node);
    entry.selector_path = BuildSelectorPathHelper(node);
    entry.tag_name = String::FromUtf8(base::as_byte_span(base::ToLowerASCII(To<Element>(node)->tagName().Utf8())));
    
    // Geometry
    gfx::RectF rect = gfx::RectF(object->AbsoluteBoundingBoxRect());
    entry.bounding_box = rect;
    
    // Parent Node ID
    LayoutObject* parent = object->Parent();
    while (parent && !parent->GetNode()) {
        parent = parent->Parent(); // Skip anonymous blocks
    }
    if (parent && parent->GetNode()) {
        entry.parent_node_id = IdentifiersFactory::IntIdForNode(parent->GetNode());
    }
    
    // Timing (Fake/Placeholder for now)
    entry.layout_time_ms = 0; 
    
    entries.push_back(std::move(entry));
  }
}

} // namespace

void ChromiumRLTraceBuffer::RecordLayoutComplete(LocalFrame* frame, LayoutView* layout_view, FrameMetricsEntry metrics) {
  if (!frame || !layout_view) return;
  
  Vector<LayoutTimingEntry> entries;
  CollectLayoutEntries(layout_view, entries);
  
  LOG(INFO) << "ChromiumRL: Recorded layout for " << entries.size() << " elements.";
  
  layout_timings_.Set(frame, std::move(entries));
  // Note: metrics are set via UpdateFrameMetrics now.
}

const Vector<LayoutTimingEntry>& ChromiumRLTraceBuffer::GetLayoutTimings(LocalFrame* frame) const {
  auto it = layout_timings_.find(frame);
  if (it != layout_timings_.end()) {
    return it->value;
  }
  DEFINE_STATIC_LOCAL(Vector<LayoutTimingEntry>, empty, ());
  return empty;
}

const FrameMetricsEntry& ChromiumRLTraceBuffer::GetFrameMetrics(LocalFrame* frame) const {
  auto it = frame_metrics_.find(frame);
  if (it != frame_metrics_.end()) {
    return it->value;
  }
  DEFINE_STATIC_LOCAL(FrameMetricsEntry, empty, ());
  return empty;
}

void ChromiumRLTraceBuffer::UpdateFrameMetrics(LocalFrame* frame, FrameMetricsEntry metrics) {
    frame_metrics_.Set(frame, metrics);
}

void ChromiumRLTraceBuffer::AddCLSEntry(CLSTraceEntry entry) {
  if (cls_entries_.size() >= kMaxCLSEntries) {
    cls_entries_.erase(cls_entries_.begin());
  }
  total_cls_ += entry.score;
  cls_entries_.push_back(std::move(entry));
}

void ChromiumRLTraceBuffer::RecordCompositorState(const cc::LayerTreeHost* layer_tree_host) {
  LOG(INFO) << "ChromiumRL: [Buffer] RecordCompositorState called.";
  compositor_layers_.clear();
  pending_texture_uploads_ = 0;
  
  if (!layer_tree_host) {
    LOG(ERROR) << "ChromiumRL: [Buffer] FAILED: layer_tree_host is null.";
    return;
  }
  const cc::Layer* root = layer_tree_host->root_layer();
  if (!root) {
    LOG(ERROR) << "ChromiumRL: [Buffer] FAILED: root_layer is null.";
    return;
  }

  Vector<const cc::Layer*> queue;
  queue.push_back(root);
  
  while (!queue.empty()) {
    const cc::Layer* layer = queue.back();
    queue.pop_back();
    
    CompositorLayerEntry entry;
    entry.layer = const_cast<cc::Layer*>(layer);
    
    gfx::Size bounds = layer->bounds();
    entry.gpu_memory_bytes = bounds.width() * bounds.height() * 4;
    
    compositor_layers_.push_back(std::move(entry));
    
    for (const auto& child : layer->children()) {
      queue.push_back(child.get());
    }
  }
  LOG(INFO) << "ChromiumRL: [Buffer] SUCCESS: Recorded " << compositor_layers_.size() << " compositor layers.";
}

}  // namespace blink
