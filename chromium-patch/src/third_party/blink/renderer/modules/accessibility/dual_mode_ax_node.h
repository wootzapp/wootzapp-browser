#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_ACCESSIBILITY_DUAL_MODE_AX_NODE_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_ACCESSIBILITY_DUAL_MODE_AX_NODE_H_

#include <optional>
#include <string>
#include <vector>
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

// ============================================================================
// Core Node (AndroidWorld Compatible)
// ============================================================================

struct AndroidWorldBounds {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

struct CoreNode {
  int node_id = 0;
  std::optional<int> parent_id;
  Vector<int> children_ids;

  // Identity
  std::string class_name;
  std::optional<std::string> package_name;
  std::optional<std::string> resource_id;

  // Text Content
  std::optional<std::string> text;
  std::optional<std::string> content_description;
  std::optional<std::string> hint_text;

  // Layout
  AndroidWorldBounds bounds;

  // Capabilities
  bool clickable = false;
  bool long_clickable = false;
  bool scrollable = false;
  bool focusable = false;
  bool editable = false;
  bool checkable = false;

  // States
  bool enabled = true;
  bool focused = false;
  bool selected = false;
  bool checked = false;
  bool visible = true;
};

// ============================================================================
// Design Perception Mode
// ============================================================================

struct DesignPerceptionMode {
  struct VisualProperties {
    std::optional<std::string> background_color;
    std::optional<std::string> foreground_color;
    std::optional<float> opacity;
    std::optional<std::string> font_family;
    std::optional<int> font_size;
    std::optional<int> font_weight;
  } visual;

  struct LayoutContext {
    std::optional<std::string> display_type;
    std::optional<int> z_index;
    std::optional<bool> is_fixed_position;
    std::optional<bool> is_sticky;
    std::optional<int> stacking_context_depth;
  } layout;

  struct SpatialInfo {
    std::optional<int> distance_from_viewport_top;
    std::optional<bool> is_above_fold;
    std::optional<int> nearest_landmark_id;
    std::optional<float> visual_prominence_score;
  } spatial;
};

// ============================================================================
// X-Ray Semantic Mode
// ============================================================================

struct XRaySemanticMode {
  struct Semantics {
    std::string role;
    std::optional<std::string> sub_role;

    std::optional<std::string> name;
    std::optional<std::string> description;
    std::optional<std::string> value;
    std::optional<std::string> hint_text;
    std::optional<std::string> placeholder;

    Vector<std::string> intent_tags;
    std::optional<float> importance_score;

    std::optional<Vector<float>> text_embedding;
  } semantics;

  struct Interaction {
    bool clickable = false;
    bool editable = false;
    bool scrollable = false;
    bool checkable = false;
    bool focusable = false;
    bool long_clickable = false;

    bool enabled = true;
    bool focused = false;
    bool selected = false;
    bool checked = false;
    bool expanded = false;

    Vector<std::string> available_actions;
    std::optional<std::string> primary_action;
    std::optional<float> affordance_score;
  } interaction;

  struct TaskContext {
    std::optional<std::string> workflow_stage;
    std::optional<float> goal_alignment_score;
  } task_context;

  struct ScrollContext {
    std::optional<int> scrollable_container_id;
    std::optional<float> scroll_position;
    std::optional<float> scroll_range;
    bool virtualized_list = false;
  } scroll_context;
};

// ============================================================================
// Main DualModeAXNode
// ============================================================================

struct DualModeAXNode {
  CoreNode core;
  DesignPerceptionMode design_mode;
  XRaySemanticMode xray_mode;

  int64_t timestamp_ms = 0;
  std::optional<std::string> viewport_state;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_ACCESSIBILITY_DUAL_MODE_AX_NODE_H_
