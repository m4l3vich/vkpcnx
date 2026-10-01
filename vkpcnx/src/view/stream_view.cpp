#include "view/stream_view.hpp"

#include <glad/glad.h>

#include <borealis/core/application.hpp>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace vkpcnx {

static const char *VERTEX_SHADER = R"(#version 150 core
in vec2 aPos;
in vec2 aUv;
out vec2 vUv;
void main() {
  vUv = aUv;
  gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// planes: 0 = Y, 1 = U or UV, 2 = V; uNv12 selects the chroma layout
static const char *FRAGMENT_SHADER = R"(#version 150 core
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D uY;
uniform sampler2D uU;
uniform sampler2D uV;
uniform int uNv12;
uniform mat3 uYuv2Rgb;
uniform vec3 uOffset;
void main() {
  float y = texture(uY, vUv).r;
  float u, v;
  if (uNv12 == 1) {
    vec2 uv = texture(uU, vUv).rg;
    u = uv.r;
    v = uv.g;
  } else {
    u = texture(uU, vUv).r;
    v = texture(uV, vUv).r;
  }
  vec3 yuv = vec3(y, u, v) - uOffset;
  vec3 rgb = uYuv2Rgb * yuv;
  fragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

struct StreamView::Gl {
  GLuint program = 0;
  GLuint vao = 0, vbo = 0;
  GLuint textures[3] = {0, 0, 0};
  GLint locY = -1, locU = -1, locV = -1, locNv12 = -1, locMatrix = -1, locOffset = -1;
};

// Shown until the server sends its first SC_CURSOR (the web client starts its
// cursor service with a default arrow, §7.1). X = black, . = white.
static const char *const DEFAULT_ARROW[] = {
  "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
  "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
  "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ", "X    X..X   ",
  "     X..X   ", "      X..X  ", "      X..X  ", "       XX   ",
};

static stream::CursorImage defaultArrowCursor() {
  stream::CursorImage c;
  c.width = 12;
  c.height = sizeof DEFAULT_ARROW / sizeof DEFAULT_ARROW[0];
  c.hotspotX = c.hotspotY = 0;
  c.rgba.assign(static_cast<size_t>(c.width) * c.height * 4, 0);
  for (int y = 0; y < c.height; y++)
    for (int x = 0; x < c.width; x++) {
      char ch = DEFAULT_ARROW[y][x];
      if (ch == ' ')
        continue;
      uint8_t *d = &c.rgba[(static_cast<size_t>(y) * c.width + x) * 4];
      d[0] = d[1] = d[2] = ch == 'X' ? 0 : 255;
      d[3] = 255;
    }
  return c;
}

static GLuint compileShader(GLenum type, const char *src) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, nullptr);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetShaderInfoLog(shader, sizeof log, nullptr, log);
    brls::Logger::error("StreamView: shader compile failed: {}", log);
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

StreamView::StreamView() {
  this->setFocusable(false);
  fpsWindowStart_ = std::chrono::steady_clock::now();
  setCursor(defaultArrowCursor());
}

StreamView::~StreamView() {
  destroyGl();
  if (cursorImage_)
    nvgDeleteImage(brls::Application::getNVGContext(), cursorImage_);
}

bool StreamView::initGl() {
  if (gl_)
    return true;
  auto *gl = new Gl();
  GLuint vs = compileShader(GL_VERTEX_SHADER, VERTEX_SHADER);
  GLuint fs = compileShader(GL_FRAGMENT_SHADER, FRAGMENT_SHADER);
  if (!vs || !fs) {
    delete gl;
    return false;
  }
  gl->program = glCreateProgram();
  glAttachShader(gl->program, vs);
  glAttachShader(gl->program, fs);
  glBindAttribLocation(gl->program, 0, "aPos");
  glBindAttribLocation(gl->program, 1, "aUv");
  glLinkProgram(gl->program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(gl->program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[1024];
    glGetProgramInfoLog(gl->program, sizeof log, nullptr, log);
    brls::Logger::error("StreamView: program link failed: {}", log);
    glDeleteProgram(gl->program);
    delete gl;
    return false;
  }
  gl->locY = glGetUniformLocation(gl->program, "uY");
  gl->locU = glGetUniformLocation(gl->program, "uU");
  gl->locV = glGetUniformLocation(gl->program, "uV");
  gl->locNv12 = glGetUniformLocation(gl->program, "uNv12");
  gl->locMatrix = glGetUniformLocation(gl->program, "uYuv2Rgb");
  gl->locOffset = glGetUniformLocation(gl->program, "uOffset");

  // Full-screen quad; texture rows are top-down so flip V
  static const float quad[] = {
    -1.f,
    -1.f,
    0.f,
    1.f, //
    1.f,
    -1.f,
    1.f,
    1.f, //
    -1.f,
    1.f,
    0.f,
    0.f, //
    1.f,
    1.f,
    1.f,
    0.f, //
  };
  glGenVertexArrays(1, &gl->vao);
  glGenBuffers(1, &gl->vbo);
  glBindVertexArray(gl->vao);
  glBindBuffer(GL_ARRAY_BUFFER, gl->vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(
    1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void *>(2 * sizeof(float))
  );
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glGenTextures(3, gl->textures);
  for (GLuint t : gl->textures) {
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  gl_ = gl;
  return true;
}

void StreamView::destroyGl() {
  if (!gl_)
    return;
  glDeleteTextures(3, gl_->textures);
  glDeleteBuffers(1, &gl_->vbo);
  glDeleteVertexArrays(1, &gl_->vao);
  glDeleteProgram(gl_->program);
  delete gl_;
  gl_ = nullptr;
}

void StreamView::upload(const AVFrame *frame) {
  bool nv12 = frame->format == AV_PIX_FMT_NV12;
  bool yuv420 = frame->format == AV_PIX_FMT_YUV420P || frame->format == AV_PIX_FMT_YUVJ420P;
  if (!nv12 && !yuv420) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      brls::Logger::error("StreamView: unsupported pixel format {}", frame->format);
    }
    return;
  }

  bool realloc =
    frame->width != frameWidth_ || frame->height != frameHeight_ || frame->format != texFormat_;
  frameWidth_ = frame->width;
  frameHeight_ = frame->height;
  texFormat_ = frame->format;
  colorspace_ = frame->colorspace;
  colorRange_ = frame->color_range;
  int cw = (frame->width + 1) / 2, ch = (frame->height + 1) / 2;

  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  auto uploadPlane = [&](
                       int unit,
                       GLenum internal,
                       GLenum format,
                       int w,
                       int h,
                       const uint8_t *data,
                       int linesize,
                       int bpp
                     ) {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, gl_->textures[unit]);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize / bpp);
    if (realloc)
      glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, GL_UNSIGNED_BYTE, data);
    else
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, GL_UNSIGNED_BYTE, data);
  };
  uploadPlane(0, GL_R8, GL_RED, frame->width, frame->height, frame->data[0], frame->linesize[0], 1);
  if (nv12) {
    uploadPlane(1, GL_RG8, GL_RG, cw, ch, frame->data[1], frame->linesize[1], 2);
  } else {
    uploadPlane(1, GL_R8, GL_RED, cw, ch, frame->data[1], frame->linesize[1], 1);
    uploadPlane(2, GL_R8, GL_RED, cw, ch, frame->data[2], frame->linesize[2], 1);
  }
  glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void StreamView::drawVideo(float x, float y, float width, float height) {
  // Content → framebuffer pixels. nanovg maps windowWidth × windowHeight onto
  // the whole GL viewport (on the GL backends borealis already stores the
  // framebuffer size there, so windowScale alone covers HiDPI); read the
  // viewport back rather than the window bookkeeping, which lags resizes.
  GLint vp[4];
  glGetIntegerv(GL_VIEWPORT, vp);
  int fbW = vp[2], fbH = vp[3];
  float scale = brls::Application::windowScale * static_cast<float>(fbW) /
                static_cast<float>(brls::Application::windowWidth);
  int vx = static_cast<int>(x * scale);
  int vy = static_cast<int>(y * scale);
  int vw = static_cast<int>(width * scale);
  int vh = static_cast<int>(height * scale);

  glViewport(vx, fbH - vy - vh, vw, vh);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glUseProgram(gl_->program);

  bool nv12 = texFormat_ == AV_PIX_FMT_NV12;
  bool fullRange = colorRange_ == AVCOL_RANGE_JPEG || texFormat_ == AV_PIX_FMT_YUVJ420P;
  bool bt601 = colorspace_ == AVCOL_SPC_BT470BG || colorspace_ == AVCOL_SPC_SMPTE170M;
  // Column-major for glUniformMatrix3fv
  float kr = bt601 ? 0.299f : 0.2126f;
  float kb = bt601 ? 0.114f : 0.0722f;
  float kg = 1.f - kr - kb;
  float yScale = fullRange ? 1.f : 255.f / 219.f;
  float cScale = fullRange ? 1.f : 255.f / 224.f;
  float m[9] = {
    yScale,
    yScale,
    yScale, // Y column
    0.f,
    -cScale * 2.f * (1.f - kb) * kb / kg,
    cScale * 2.f * (1.f - kb), // U column
    cScale * 2.f * (1.f - kr),
    -cScale * 2.f * (1.f - kr) * kr / kg,
    0.f, // V column
  };
  float offset[3] = {fullRange ? 0.f : 16.f / 255.f, 128.f / 255.f, 128.f / 255.f};
  glUniformMatrix3fv(gl_->locMatrix, 1, GL_FALSE, m);
  glUniform3fv(gl_->locOffset, 1, offset);
  glUniform1i(gl_->locNv12, nv12 ? 1 : 0);
  glUniform1i(gl_->locY, 0);
  glUniform1i(gl_->locU, 1);
  glUniform1i(gl_->locV, 2);
  for (int i = 0; i < 3; i++) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, gl_->textures[i]);
  }
  glBindVertexArray(gl_->vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  for (int i = 2; i >= 0; i--) {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  glUseProgram(0);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
}

void StreamView::setCursor(const stream::CursorImage &image) {
  std::lock_guard<std::mutex> lock(cursorMutex_);
  pendingCursor_ = image;
  cursorPending_ = true;
}

void StreamView::draw(
  NVGcontext *vg,
  float x,
  float y,
  float width,
  float height,
  brls::Style style,
  brls::FrameContext *ctx
) {
  if (onDraw)
    onDraw();
  if (!gl_ && !initGl())
    return;

  if (decoder_) {
    if (auto frame = decoder_->takeFrame()) {
      upload(frame.get());
      fpsCounter_++;
      if (!firstFrame_) {
        firstFrame_ = true;
        if (onFirstFrame)
          onFirstFrame();
      }
    }
  }

  auto now = std::chrono::steady_clock::now();
  double elapsed = std::chrono::duration<double>(now - fpsWindowStart_).count();
  if (elapsed >= 1.0) {
    renderedFps_ = fpsCounter_ / elapsed;
    fpsCounter_ = 0;
    fpsWindowStart_ = now;
  }

  // Letterbox
  brls::Rect rect(x, y, width, height);
  if (frameWidth_ > 0 && frameHeight_ > 0) {
    float ar = static_cast<float>(frameWidth_) / frameHeight_;
    float w = width, h = width / ar;
    if (h > height) {
      h = height;
      w = height * ar;
    }
    rect = brls::Rect(x + (width - w) / 2, y + (height - h) / 2, w, h);
  }
  videoRect_ = rect;
  int rw = static_cast<int>(rect.getWidth()), rh = static_cast<int>(rect.getHeight());
  if ((rw != reportedW_ || rh != reportedH_) && frameWidth_ > 0) {
    reportedW_ = rw;
    reportedH_ = rh;
    if (onVideoRectChanged)
      onVideoRectChanged(rw, rh);
  }

  // Black behind the video (letterbox bars)
  nvgBeginPath(vg);
  nvgRect(vg, x, y, width, height);
  nvgFillColor(vg, nvgRGB(0, 0, 0));
  nvgFill(vg);

  if (frameWidth_ > 0) {
    // Flush what nanovg has recorded so far, draw with raw GL, resume the frame
    nvgEndFrame(vg);
    drawVideo(rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight());
    brls::Application::getPlatform()->getVideoContext()->resetState();
    nvgBeginFrame(
      vg,
      brls::Application::windowWidth,
      brls::Application::windowHeight,
      static_cast<float>(brls::Application::getPlatform()->getVideoContext()->getScaleFactor())
    );
    nvgScale(vg, brls::Application::windowScale, brls::Application::windowScale);
  }

  if (onDrawBackdrop)
    onDrawBackdrop(vg, rect);

  // Cursor
  {
    std::lock_guard<std::mutex> lock(cursorMutex_);
    if (cursorPending_) {
      cursorPending_ = false;
      if (cursorImage_) {
        nvgDeleteImage(vg, cursorImage_);
        cursorImage_ = 0;
      }
      if (!pendingCursor_.rgba.empty()) {
        cursorImage_ = nvgCreateImageRGBA(
          vg, pendingCursor_.width, pendingCursor_.height, 0, pendingCursor_.rgba.data()
        );
        cursorW_ = pendingCursor_.width;
        cursorH_ = pendingCursor_.height;
        cursorHotX_ = pendingCursor_.hotspotX;
        cursorHotY_ = pendingCursor_.hotspotY;
      }
    }
  }
  if (cursorVisible_ && cursorImage_) {
    float cx = rect.getMinX() + cursorX_ - cursorHotX_;
    float cy = rect.getMinY() + cursorY_ - cursorHotY_;
    nvgSave(vg);
    nvgScissor(vg, rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight());
    nvgBeginPath(vg);
    nvgRect(vg, cx, cy, cursorW_, cursorH_);
    nvgFillPaint(vg, nvgImagePattern(vg, cx, cy, cursorW_, cursorH_, 0, cursorImage_, 1.0f));
    nvgFill(vg);
    nvgRestore(vg);
  }
}

} // namespace vkpcnx
