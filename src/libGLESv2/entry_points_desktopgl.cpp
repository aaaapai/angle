//
// Copyright 2026 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// entry_points_desktop_compat.cpp:
//   Desktop-GL entry points that the iOS launcher's tinygl4angle shim still
//   branches into. ANGLE removed its Desktop GL front-end in
//   google/angle@b16d105, so these symbols no longer exist upstream. Keep two
//   thin exports so the shim keeps loading without carrying a private patch.
//

#include "libGLESv2/entry_points_gles_3_0_autogen.h"
#include "libGLESv2/entry_points_gles_ext_autogen.h"

extern "C" {

// glDrawBuffer is a Desktop GL entry point. ES only has GL_BACK/GL_NONE for
// the default framebuffer; map every desktop draw buffer to GL_BACK.
ANGLE_EXPORT void GL_APIENTRY GL_DrawBuffer(GLenum buf)
{
    GLenum drawBuffer = (buf == GL_NONE) ? GL_NONE : GL_BACK;
    GL_DrawBuffers(1, &drawBuffer);
}

// Also desktopgl-only function.
ANGLE_EXPORT void GL_APIENTRY GL_PolygonMode(GLenum face, GLenum mode)
{
    GL_PolygonModeNV(face, mode);
}

}  // extern "C"
