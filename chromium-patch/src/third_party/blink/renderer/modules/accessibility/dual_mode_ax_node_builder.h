#ifndef THIRD_PARTY_BLINK_RENDERER_MODULES_ACCESSIBILITY_DUAL_MODE_AX_NODE_BUILDER_H_
#define THIRD_PARTY_BLINK_RENDERER_MODULES_ACCESSIBILITY_DUAL_MODE_AX_NODE_BUILDER_H_

#include "third_party/blink/renderer/modules/accessibility/dual_mode_ax_node.h"
#include "third_party/blink/renderer/modules/accessibility/ax_object.h"
#include "third_party/blink/renderer/platform/wtf/allocator/allocator.h"

namespace blink {

class DualModeAXNodeBuilder {
  STATIC_ONLY(DualModeAXNodeBuilder);
 public:
  static DualModeAXNode Build(const AXObject* ax_object);

 private:
  static CoreNode BuildCoreNode(const AXObject* ax_object);
  static DesignPerceptionMode BuildDesignMode(const AXObject* ax_object);
  static XRaySemanticMode BuildXRayMode(const AXObject* ax_object);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_MODULES_ACCESSIBILITY_DUAL_MODE_AX_NODE_BUILDER_H_
