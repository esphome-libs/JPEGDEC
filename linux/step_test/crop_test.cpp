// Checks that a crop gives exactly the same pixels as that part of the uncropped image, at every scale, through
// the draw callback and into a framebuffer, for every JPEG file named on the command line.
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../../src/JPEGDEC.h"
#include "../../src/JPEGDEC.cpp"

static std::vector<uint8_t> canvas;
static int canvas_w, canvas_h, bytes_per_pixel, outside;

static int draw_cb(JPEGDRAW *d) {
  const auto *p = reinterpret_cast<const uint8_t *>(d->pPixels);
  for (int y = 0; y < d->iHeight; y++) {
    for (int x = 0; x < d->iWidthUsed; x++) {
      if (d->x + x >= canvas_w || d->y + y >= canvas_h) {
        outside++;
        continue;
      }
      for (int b = 0; b < bytes_per_pixel; b++)
        canvas[(static_cast<size_t>(d->y + y) * canvas_w + d->x + x) * bytes_per_pixel + b] =
            p[(y * d->iWidth + x) * bytes_per_pixel + b];
    }
  }
  return 1;
}

enum CropKind { CROP_NONE, CROP_MIDDLE, CROP_CORNER };

// Decodes into canvas, which is sized to the output; returns false if the file cannot be opened
static bool decode(JPEGDEC &jpeg, std::vector<uint8_t> &file, int type, int options, int scale, CropKind crop,
                   bool framebuffer, int &x0, int &y0) {
  if (!jpeg.openRAM(file.data(), static_cast<int>(file.size()), draw_cb))
    return false;
  jpeg.setPixelType(type);
  bytes_per_pixel = type == RGB8888 ? 4 : type == EIGHT_BIT_GRAYSCALE ? 1 : 2;
  int w = jpeg.getWidth(), h = jpeg.getHeight();
  x0 = y0 = 0;
  if (crop != CROP_NONE) {
    if (crop == CROP_MIDDLE)
      jpeg.setCropArea(32, 16, w / 2, h / 2);
    else
      jpeg.setCropArea(w / 2, h / 2, w, h);
    jpeg.getCropArea(&x0, &y0, &w, &h);
  }
  x0 /= scale;
  y0 /= scale;
  canvas_w = (w + scale - 1) / scale;
  canvas_h = (h + scale - 1) / scale;
  outside = 0;
  canvas.assign(static_cast<size_t>(canvas_w) * canvas_h * bytes_per_pixel, 0);
  if (framebuffer) {
    // Exactly the size of the output, so ASan catches an MCU drawn past its right or bottom edge
    std::vector<uint8_t> fb(static_cast<size_t>(canvas_w) * canvas_h * bytes_per_pixel, 0);
    jpeg.setFramebuffer(fb.data());
    jpeg.decode(0, 0, options);
    for (size_t i = 0; i < canvas.size(); i++)
      canvas[i] = fb[i];
  } else {
    jpeg.decode(0, 0, options);
  }
  jpeg.close();
  return true;
}

int main(int argc, char **argv) {
  static JPEGDEC jpeg;
  static const int options[] = {0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH};
  int checks = 0, failures = 0;
  for (int a = 1; a < argc; a++) {
    FILE *f = fopen(argv[a], "rb");
    if (f == nullptr) {
      printf("cannot open %s\n", argv[a]);
      return 2;
    }
    std::vector<uint8_t> file;
    int ch;
    while ((ch = fgetc(f)) != EOF)
      file.push_back(static_cast<uint8_t>(ch));
    fclose(f);
    jpeg.openRAM(file.data(), static_cast<int>(file.size()), draw_cb);
    const bool progressive = jpeg.getJPEGType() == JPEG_MODE_PROGRESSIVE;
    jpeg.close();
    for (int type : {RGB565_LITTLE_ENDIAN, RGB8888, EIGHT_BIT_GRAYSCALE}) {
      for (int k = 0; k < 4; k++) {
        if (progressive && k != 0)
          continue;  // a progressive image always comes out at 1/8 size
        const int scale = progressive ? 8 : 1 << k;
        int fx, fy;
        decode(jpeg, file, type, options[k], scale, CROP_NONE, false, fx, fy);
        const std::vector<uint8_t> full = canvas;
        const int full_w = canvas_w, full_h = canvas_h;
        for (CropKind crop : {CROP_NONE, CROP_MIDDLE, CROP_CORNER}) {
          for (bool framebuffer : {false, true}) {
            if (crop == CROP_NONE && !framebuffer)
              continue;  // that is the reference
            checks++;
            int x0, y0;
            decode(jpeg, file, type, options[k], scale, crop, framebuffer, x0, y0);
            int worst = 0;
            for (int y = 0; y < canvas_h && y0 + y < full_h; y++)
              for (int x = 0; x < canvas_w && x0 + x < full_w; x++)
                for (int b = 0; b < bytes_per_pixel; b++)
                  worst = std::max(worst, abs(canvas[(static_cast<size_t>(y) * canvas_w + x) * bytes_per_pixel + b] -
                                              full[(static_cast<size_t>(y0 + y) * full_w + x0 + x) * bytes_per_pixel + b]));
            if (worst != 0 || outside != 0) {
              printf("FAIL %s type %d scale 1/%d %s crop%s: max difference %d, %d pixels drawn outside the crop\n",
                     argv[a], type, scale, crop == CROP_NONE ? "no" : crop == CROP_MIDDLE ? "middle" : "corner",
                     framebuffer ? " into a framebuffer" : "", worst, outside);
              failures++;
            }
          }
        }
      }
    }
  }
  printf("%d crop checks, %d failures\n", checks, failures);
  return failures != 0;
}
