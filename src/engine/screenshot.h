// screenshot.h — framebuffer capture utilities for automated testing.
//
// captureFramebufferBase64: reads the back buffer, encodes as PNG, and prints
//   it base64-encoded between CV_SHOT_BEGIN / CV_SHOT_END markers on stdout.
//   Works identically on desktop (stdout) and web (console.log via emrun).
//
// framebufferNonBlank: returns false if the framebuffer appears to be entirely
//   the clear colour — catches blank-screen regressions without human review.
#pragma once

namespace cv {

void captureFramebufferBase64(int w, int h);
bool framebufferNonBlank(int w, int h);

} // namespace cv
