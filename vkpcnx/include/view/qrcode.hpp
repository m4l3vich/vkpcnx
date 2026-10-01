#pragma once

#include <borealis.hpp>
#include <qrcodegen.hpp>

namespace vkpcnx {
class QrCode : public brls::View {
public:
  QrCode() {
    this->registerStringXMLAttribute("text", [this](const std::string &v) { this->setText(v); });
  };

  void setText(const std::string &text);

  void draw(
    NVGcontext *vg,
    float x,
    float y,
    float width,
    float height,
    brls::Style style,
    brls::FrameContext *ctx
  ) override;

  static brls::View *create();

private:
  qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(" ", qrcodegen::QrCode::Ecc::MEDIUM);
};
}; // namespace vkpcnx