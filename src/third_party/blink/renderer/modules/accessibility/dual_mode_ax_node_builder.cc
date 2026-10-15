#include "third_party/blink/renderer/modules/accessibility/dual_mode_ax_node_builder.h"

#include <algorithm>
#include "base/strings/string_util.h"
#include "third_party/blink/renderer/core/css/css_property_names.h"
#include "third_party/blink/renderer/core/css/properties/css_property.h"
#include "third_party/blink/renderer/core/css/properties/longhand.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element.h"
#include "third_party/blink/renderer/core/frame/local_frame_view.h"
#include "third_party/blink/renderer/core/layout/layout_object.h"
#include "third_party/blink/renderer/core/layout/layout_view.h"
#include "third_party/blink/renderer/core/paint/paint_layer.h"
#include "third_party/blink/renderer/core/paint/paint_layer_scrollable_area.h"
#include "third_party/blink/renderer/core/style/computed_style.h"
#include "third_party/blink/renderer/modules/accessibility/ax_object-inl.h"
#include "third_party/blink/renderer/modules/accessibility/ax_object.h"
#include "third_party/blink/renderer/modules/accessibility/ax_object_cache_impl.h"
#include "third_party/blink/renderer/platform/graphics/color.h"
#include "third_party/blink/renderer/platform/wtf/casting.h"
#include "ui/gfx/geometry/rect_f.h"

namespace blink {

DualModeAXNode DualModeAXNodeBuilder::Build(const AXObject* ax_object) {
  DualModeAXNode node;
  node.core = BuildCoreNode(ax_object);
  node.design_mode = BuildDesignMode(ax_object);
  node.xray_mode = BuildXRayMode(ax_object);
  
  node.timestamp_ms = base::Time::Now().InMillisecondsSinceUnixEpoch();
  
  if (!ax_object->IsVisible()) {
    node.viewport_state = "offscreen";
  } else {
    node.viewport_state = "visible";
  }

  return node;
}

CoreNode DualModeAXNodeBuilder::BuildCoreNode(const AXObject* ax_object) {
  CoreNode core;

  core.node_id = static_cast<int>(ax_object->AXObjectID());
  
  if (auto* parent = ax_object->ParentObject()) {
    core.parent_id = static_cast<int>(parent->AXObjectID());
  }
  
  for (const auto& child : ax_object->ChildrenIncludingIgnored()) {
    core.children_ids.push_back(static_cast<int>(child->AXObjectID()));
  }

  std::string tag_name =
      ax_object->GetNode()
          ? base::ToLowerASCII(ax_object->GetNode()->nodeName().Utf8())
          : "unknown";
  String ax_role = AXObject::RoleName(ax_object->RoleValue());
  core.class_name = tag_name + "." + ax_role.Utf8();

  if (auto* document = ax_object->GetDocument()) {
    core.package_name = document->Url().Host().Utf8();
  }

  if (auto* element = ax_object->GetElement()) {
    if (element->HasID()) {
      core.resource_id = element->GetIdAttribute().Utf8();
    } else {
      core.resource_id = "generated_" + String::Number(core.node_id).Utf8();
    }
  }

  String name = ax_object->ComputedName();
  if (!name.empty()) {
    core.text = name.Utf8();
  }

  ax::mojom::blink::NameFrom name_from = ax::mojom::blink::NameFrom::kNone;
  ax::mojom::blink::DescriptionFrom desc_from = ax::mojom::blink::DescriptionFrom::kNone;
  AXObject::AXObjectVector desc_objs;
  String desc = ax_object->Description(name_from, desc_from, &desc_objs);
  if (!desc.empty()) {
    core.content_description = desc.Utf8();
  }

  String placeholder = ax_object->Placeholder(name_from);
  if (!placeholder.empty()) {
    core.hint_text = placeholder.Utf8();
  }

  PhysicalRect p_bounds = ax_object->GetBoundsInFrameCoordinates();
  gfx::RectF bounds = static_cast<gfx::RectF>(p_bounds);
  
  core.bounds.left = static_cast<int>(bounds.x());
  core.bounds.top = static_cast<int>(bounds.y());
  core.bounds.right = static_cast<int>(bounds.right());
  core.bounds.bottom = static_cast<int>(bounds.bottom());

  core.clickable = ax_object->IsClickable();
  core.scrollable = ax_object->IsScrollableContainer();
  core.focusable = ax_object->CanSetFocusAttribute();
  core.editable = ax_object->IsEditable();
  
  core.checkable = (ax_object->RoleValue() == ax::mojom::blink::Role::kCheckBox ||
                    ax_object->RoleValue() == ax::mojom::blink::Role::kRadioButton ||
                    ax_object->RoleValue() == ax::mojom::blink::Role::kSwitch);

  core.enabled = !ax_object->IsDisabled();
  core.focused = ax_object->IsFocused();
  core.selected = ax_object->IsSelected() == kSelectedStateTrue;
  core.checked = ax_object->CheckedState() == ax::mojom::CheckedState::kTrue;
  core.visible = !ax_object->IsHiddenViaStyle();

  return core;
}

DesignPerceptionMode DualModeAXNodeBuilder::BuildDesignMode(const AXObject* ax_object) {
  DesignPerceptionMode design;

  LayoutObject* layout = ax_object->GetLayoutObject();
  if (!layout) {
    return design;
  }

  const ComputedStyle& style = layout->StyleRef();

  if (style.HasBackground()) {
    const Longhand& bg_prop = To<Longhand>(CSSProperty::Get(CSSPropertyID::kBackgroundColor));
    Color bg = style.VisitedDependentColor(bg_prop);
    design.visual.background_color = bg.SerializeAsCSSColor().Utf8();
  }

  const Longhand& fg_prop = To<Longhand>(CSSProperty::Get(CSSPropertyID::kColor));
  Color fg = style.VisitedDependentColor(fg_prop);
  design.visual.foreground_color = fg.SerializeAsCSSColor().Utf8();
  design.visual.opacity = style.Opacity();
  
  design.visual.font_size = style.GetFont()->GetFontDescription().ComputedSize();
  design.visual.font_weight = style.GetFont()->GetFontDescription().Weight();

  design.layout.z_index = style.ZIndex();
  design.layout.is_fixed_position = style.GetPosition() == EPosition::kFixed;
  design.layout.is_sticky = style.GetPosition() == EPosition::kSticky;

  LocalFrameView* frame_view = ax_object->DocumentFrameView();
  if (frame_view) {
    PhysicalRect p_bounds = ax_object->GetBoundsInFrameCoordinates();
    gfx::RectF bounds = static_cast<gfx::RectF>(p_bounds);
    
    ScrollableArea* scrollable_area = frame_view->LayoutViewport();
    int scroll_offset_y = scrollable_area ? scrollable_area->GetScrollOffset().y() : 0;
    
    design.spatial.distance_from_viewport_top = 
        static_cast<int>(bounds.y()) - scroll_offset_y;
    design.spatial.is_above_fold = (bounds.y() - scroll_offset_y) < frame_view->Height();

    float viewport_area = static_cast<float>(frame_view->Width() * frame_view->Height());
    if (viewport_area > 0) {
      float element_area = bounds.width() * bounds.height();
      design.spatial.visual_prominence_score = 
          std::min(1.0f, element_area / (viewport_area * 0.1f));
    }
  }

  return design;
}

XRaySemanticMode DualModeAXNodeBuilder::BuildXRayMode(const AXObject* ax_object) {
  XRaySemanticMode xray;

  xray.semantics.role = AXObject::RoleName(ax_object->RoleValue()).Utf8();
  
  if (ax_object->RoleValue() == ax::mojom::blink::Role::kButton) {
    std::string name = base::ToLowerASCII(ax_object->ComputedName().Utf8());
    if (name.find("submit") != std::string::npos ||
        name.find("send") != std::string::npos) {
      xray.semantics.sub_role = "submit-button";
    } else if (name.find("search") != std::string::npos) {
      xray.semantics.sub_role = "search-button";
    }
    
    if (Element* element = ax_object->GetElement()) {
        if (element->hasAttribute(html_names::kAriaPressedAttr)) {
            xray.semantics.sub_role = "toggle-button";
        } else if (element->hasAttribute(html_names::kAriaHaspopupAttr)) {
            xray.semantics.sub_role = "menu-button";
        }
    }
  }

  xray.semantics.name = ax_object->ComputedName().Utf8();
  
  ax::mojom::blink::NameFrom name_from = ax::mojom::blink::NameFrom::kNone;
  ax::mojom::blink::DescriptionFrom desc_from = ax::mojom::blink::DescriptionFrom::kNone;
  AXObject::AXObjectVector desc_objs;
  String desc = ax_object->Description(name_from, desc_from, &desc_objs);
  xray.semantics.description = desc.Utf8();

  Vector<std::string> tags;
  auto role = ax_object->RoleValue();
  std::string name_lower =
      base::ToLowerASCII(ax_object->ComputedName().Utf8());
  
  if (role == ax::mojom::blink::Role::kButton || role == ax::mojom::blink::Role::kLink) {
    tags.push_back("interactive");
    if (name_lower.find("login") != std::string::npos ||
        name_lower.find("sign in") != std::string::npos)
      tags.push_back("authentication");
    if (name_lower.find("buy") != std::string::npos ||
        name_lower.find("cart") != std::string::npos ||
        name_lower.find("checkout") != std::string::npos)
      tags.push_back("commerce");
    if (name_lower.find("search") != std::string::npos ||
        name_lower.find("find") != std::string::npos)
      tags.push_back("search");
  }
  
  if (role == ax::mojom::blink::Role::kNavigation) {
    tags.push_back("navigation");
  }
  
  xray.semantics.intent_tags = std::move(tags);

  float importance = 0.5f;
  if (!ax_object->ComputedName().empty()) importance += 0.3f;
  if (ax_object->IsClickable()) importance += 0.3f;
  if (!ax_object->IsHiddenViaStyle()) importance += 0.2f;
  xray.semantics.importance_score = std::min(1.0f, importance);

  xray.interaction.clickable = ax_object->IsClickable();
  xray.interaction.editable = ax_object->IsEditable();
  xray.interaction.scrollable = ax_object->IsScrollableContainer();
  xray.interaction.focusable = ax_object->CanSetFocusAttribute();

  xray.interaction.enabled = !ax_object->IsDisabled();
  xray.interaction.focused = ax_object->IsFocused();
  xray.interaction.expanded = ax_object->IsExpanded() == kExpandedExpanded;

  Vector<std::string> actions;
  if (xray.interaction.clickable) actions.push_back("click");
  if (xray.interaction.focusable) actions.push_back("focus");
  if (xray.interaction.editable) actions.push_back("input");
  if (xray.interaction.scrollable) actions.push_back("scroll");
  xray.interaction.available_actions = std::move(actions);

  if (xray.interaction.editable) {
    xray.interaction.primary_action = "input";
  } else if (xray.interaction.clickable) {
    xray.interaction.primary_action = "click";
  }

  float affordance = 0.5f;
  if (xray.interaction.enabled) affordance += 0.2f;
  if (!ax_object->ComputedName().empty()) affordance += 0.2f;
  xray.interaction.affordance_score = std::min(1.0f, affordance);

  if (AXObject* parent = ax_object->ParentObject()) {
      if (parent->IsScrollableContainer()) {
          xray.scroll_context.scrollable_container_id = static_cast<int>(parent->AXObjectID());
      }
  }

  return xray;
}

}  // namespace blink
