// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/compositor_opengl.h"

#include "GLES3/gl3.h"
#include "flutter/shell/platform/windows/flutter_windows_view.h"

namespace flutter {

namespace {

// The metadata for an OpenGL framebuffer backing store.
struct FramebufferBackingStore {
  uint32_t framebuffer_id;
  uint32_t texture_id;
};

// Based off Skia's logic:
// https://github.com/google/skia/blob/4738ed711e03212aceec3cd502a4adb545f38e63/src/gpu/ganesh/gl/GrGLCaps.cpp#L1963-L2116
int GetSupportedTextureFormat(const impeller::DescriptionGLES* description) {
  if (description->HasExtension("GL_EXT_texture_format_BGRA8888")) {
    return GL_BGRA8_EXT;
  } else if (description->HasExtension("GL_APPLE_texture_format_BGRA8888") &&
             description->GetGlVersion().IsAtLeast(impeller::Version(3, 0))) {
    return GL_BGRA8_EXT;
  } else {
    return GL_RGBA8;
  }
}

}  // namespace

CompositorOpenGL::CompositorOpenGL(FlutterWindowsEngine* engine,
                                   impeller::ProcTableGLES::Resolver resolver)
    : engine_(engine), resolver_(resolver) {}

bool CompositorOpenGL::CreateBackingStore(
    const FlutterBackingStoreConfig& config,
    FlutterBackingStore* result) {
  if (!is_initialized_ && !Initialize()) {
    return false;
  }

  auto store = std::make_unique<FramebufferBackingStore>();

  gl_->GenTextures(1, &store->texture_id);
  gl_->GenFramebuffers(1, &store->framebuffer_id);

  gl_->BindFramebuffer(GL_FRAMEBUFFER, store->framebuffer_id);

  gl_->BindTexture(GL_TEXTURE_2D, store->texture_id);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  gl_->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  gl_->TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, config.size.width,
                  config.size.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  gl_->BindTexture(GL_TEXTURE_2D, 0);

  gl_->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0_EXT,
                            GL_TEXTURE_2D, store->texture_id, 0);

  result->type = kFlutterBackingStoreTypeOpenGL;
  result->open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
  result->open_gl.framebuffer.name = store->framebuffer_id;
  result->open_gl.framebuffer.target = format_;
  result->open_gl.framebuffer.user_data = store.release();
  result->open_gl.framebuffer.destruction_callback = [](void* user_data) {
    // Backing store destroyed in `CompositorOpenGL::CollectBackingStore`, set
    // on FlutterCompositor.collect_backing_store_callback during engine start.
  };
  return true;
}

bool CompositorOpenGL::CollectBackingStore(const FlutterBackingStore* store) {
  FML_DCHECK(is_initialized_);
  FML_DCHECK(store->type == kFlutterBackingStoreTypeOpenGL);
  FML_DCHECK(store->open_gl.type == kFlutterOpenGLTargetTypeFramebuffer);

  auto user_data = static_cast<FramebufferBackingStore*>(
      store->open_gl.framebuffer.user_data);

  gl_->DeleteFramebuffers(1, &user_data->framebuffer_id);
  gl_->DeleteTextures(1, &user_data->texture_id);

  delete user_data;
  return true;
}

bool CompositorOpenGL::Present(const FlutterLayer** layers,
                               size_t layers_count) {
  // TODO: Support compositing layers and platform views.
  // See: https://github.com/flutter/flutter/issues/31713
  FML_DCHECK(is_initialized_);
  FML_DCHECK(layers_count == 1);
  FML_DCHECK(layers[0]->type == kFlutterLayerContentTypeBackingStore);
  FML_DCHECK(layers[0]->backing_store->type == kFlutterBackingStoreTypeOpenGL);
  FML_DCHECK(layers[0]->backing_store->open_gl.type ==
             kFlutterOpenGLTargetTypeFramebuffer);

  if (!engine_->view()) {
    return false;
  }

  auto width = layers[0]->size.width;
  auto height = layers[0]->size.height;

  // Acquiring the view's framebuffer ID resizes its surface if necessary.
  auto destination_id = engine_->view()->GetFrameBufferId(width, height);
  auto source_id = layers[0]->backing_store->open_gl.framebuffer.name;

  if (!engine_->surface_manager()->MakeCurrent()) {
    return false;
  }

  gl_->BindFramebuffer(GL_READ_FRAMEBUFFER, source_id);
  gl_->BindFramebuffer(GL_DRAW_FRAMEBUFFER, destination_id);

  gl_->BlitFramebuffer(0,                    // srcX0
                       0,                    // srcY0
                       width,                // srcX1
                       height,               // srcY1
                       0,                    // dstX0
                       0,                    // dstY0
                       width,                // dstX1
                       height,               // dstY1
                       GL_COLOR_BUFFER_BIT,  // mask
                       GL_NEAREST            // filter
  );

  return engine_->view()->SwapBuffers();
}

bool CompositorOpenGL::Initialize() {
  FML_DCHECK(!is_initialized_);

  if (!engine_->surface_manager()->MakeCurrent()) {
    return false;
  }

  gl_ = std::make_unique<impeller::ProcTableGLES>(resolver_);
  if (!gl_->IsValid()) {
    gl_.reset();
    return false;
  }

  format_ = GetSupportedTextureFormat(gl_->GetDescription());
  is_initialized_ = true;
  return true;
}

}  // namespace flutter
