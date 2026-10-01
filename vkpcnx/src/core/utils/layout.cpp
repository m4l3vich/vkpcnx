#include "core/utils/layout.hpp"
#include "borealis/core/box.hpp"
#include "borealis/core/view.hpp"

namespace vkpcnx::utils {
void enableWireframeRecursive(brls::View *view) {
  view->setWireframeEnabled(true);
  if (auto *box = dynamic_cast<brls::Box *>(view))
    for (brls::View *child : box->getChildren())
      enableWireframeRecursive(child);
}
} // namespace vkpcnx::utils