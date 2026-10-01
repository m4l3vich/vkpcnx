#include "core/utils/display.hpp"

#include <borealis.hpp>

#if defined(__GLFW__) && !defined(__SWITCH__)
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <borealis/platforms/glfw/glfw_video.hpp>
#endif

namespace vkpcnx::utils {

std::pair<int, int> displayPixelSize() {
#if defined(__GLFW__) && !defined(__SWITCH__)
  // Straight from the framebuffer: right whatever the renderer backend stores
  // in windowWidth (pixels for OpenGL / D3D11, points for Metal)
  if (auto *ctx = dynamic_cast<brls::GLFWVideoContext *>(
        brls::Application::getPlatform()->getVideoContext()
      )) {
    int w = 0, h = 0;
    glfwGetFramebufferSize(ctx->getGLFWWindow(), &w, &h);
    if (w > 0 && h > 0)
      return {w, h};
  }
#endif
  // Switch: 1920×1080 docked / 1280×720 handheld, scale factor 1
  return {static_cast<int>(brls::Application::windowWidth),
          static_cast<int>(brls::Application::windowHeight)};
}

} // namespace vkpcnx::utils
