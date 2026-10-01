#include "view/qrcode.hpp"

namespace vkpcnx {
void QrCode::setText(const std::string &text) {
  this->qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
  this->invalidate();
}

void QrCode::draw(
  NVGcontext *vg,
  float x,
  float y,
  float width,
  float height,
  brls::Style style,
  brls::FrameContext *ctx
) {
  int size = this->qr.getSize();
  if (size <= 0)
    return;

  float module = std::min(width, height) / size;
  float offsetX = x + (width - module * size) / 2;
  float offsetY = y + (height - module * size) / 2;

  nvgFillColor(vg, nvgRGB(0, 0, 0));
  nvgBeginPath(vg);
  for (int row = 0; row < size; row++)
    for (int col = 0; col < size; col++)
      if (this->qr.getModule(col, row))
        nvgRect(vg, offsetX + col * module, offsetY + row * module, module, module);
  nvgFill(vg);
}

brls::View *QrCode::create() { return new QrCode(); }

} // namespace vkpcnx