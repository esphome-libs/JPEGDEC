# JPEGDEC

ESPHome's fork of [JPEGDEC](https://github.com/bitbank2/JPEGDEC) by Larry Bank (BitBank Software), an optimized JPEG decoder for microcontrollers and PCs. ESPHome's `runtime_image` component uses it to decode JPEG images.

[![A project from the Open Home Foundation](https://www.openhomefoundation.org/badges/ohf-project.png)](https://www.openhomefoundation.org/)

## Changes from upstream

Based on upstream 1.8.4, with:

- **Decoding in steps**: `decodeBegin()` and `decodeRows()` decode a few MCU rows per call, and can wait for input that has not arrived yet
- **Progressive images**: decoded in full at every scale, in steps and as their input arrives, where upstream only draws the first scan at 1/8 size
- **Extended sequential images** (SOF1) with 8 bits per sample, decoded as baseline with up to 3 DC and 3 AC Huffman tables numbered 0-3, as libjpeg writes when an image uses tables past 1 (Immich previews, for one); upstream refuses them. A third table of either kind is allocated by `decodeBegin()` and freed when the image is finished, or by `close()`, and takes 4.8 KB; if it cannot be allocated, `decodeBegin()` fails with `JPEG_ERROR_MEMORY`
- **Fixes**: progressive images with skipped blocks, cropping with scaling, RGB8888 output of grayscale images, grayscale output of images whose two chroma components use different Huffman tables, NEON colour order, short reads in the input filter, AC coefficients whose code and extra bits straddle a refill, 1/4 scale RGB8888 output of 4:4:0 images, frames with more than 4 components
- **ESP-IDF**: the ESP32-S3 SIMD colour conversion is used under ESP-IDF as well as Arduino
- **Build**: compiled with `-O2`

Later upstream fixes are taken as they are needed.

## Usage

Add to `platformio.ini`, replacing `<tag>` with a [release](https://github.com/esphome-libs/JPEGDEC/releases) tag:

```ini
lib_deps =
    https://github.com/esphome-libs/JPEGDEC#<tag>
```

The API is upstream's, described in its [wiki](https://github.com/bitbank2/JPEGDEC/wiki).

### Decoding in steps

`decode()` decodes the whole image in one call. To spread the work over a main loop, call `decodeBegin()` with the same arguments, then `decodeRows(n)` to decode up to `n` more MCU rows. It returns `JPEG_DECODE_MORE` while rows remain, 1 when the image is finished and 0 on an error. The draw callback is called as it is by `decode()`. An image is decoded once per `open()`: a second `decode()` or `decodeBegin()` fails with `JPEG_INVALID_PARAMETER`, so open it again to decode it again.

```cpp
jpeg.openRAM(data, size, draw);
if (jpeg.decodeBegin(0, 0, 0)) {
  while (jpeg.decodeRows(2) == JPEG_DECODE_MORE) {
    // do other work here
  }
}
jpeg.close();
```

Input may arrive slowly, as from a download: the read callback can return fewer bytes than asked for while the file is incomplete. When less than `JPEG_INPUT_MARGIN` bytes are buffered, `decodeRows()` returns `JPEG_DECODE_NEED_DATA`, and the next call carries on from the same MCU. The headers must be readable when the image is opened.

### Progressive images

A progressive image comes in several scans, each adding detail to the whole image, so nothing can be drawn before the last one. `decodeBegin()` allocates a buffer for the DCT coefficients of the whole image, `decodeRows()` decodes the scans into it as their input arrives, and then draws the image a few MCU rows per call as for a baseline image. Until then a row of a scan counts as a row, so `decodeRows()` returns `JPEG_DECODE_MORE` and `JPEG_DECODE_NEED_DATA` without drawing anything. The buffer is freed when the image is finished, on an error, by `close()`, by the destructor and by opening another image. A `JPEGDEC` must be made by its constructor, so not with `malloc()` alone, as what it holds is freed when another image is opened. With the C API, `JPEG_close()` frees the buffer, and a `JPEGIMAGE` need not be cleared before it is opened, so call `JPEG_close()` before opening another image with it. A `JPEGDEC` cannot be copied, as both copies would free the buffer.

A file opened by name, with `open(name, ...)` or `JPEG_openFile()`, is closed by `close()`, when `open()` fails, by the destructor and by opening another image. A `File` or handle that you opened and passed in is only closed by `close()`, which can be called any number of times.

The buffer takes 2 bytes per coefficient: 3 bytes per pixel for 4:2:0 colour, 4 for 4:2:2, 6 for 4:4:4 and 2 for grayscale. Grayscale output (`EIGHT_BIT_GRAYSCALE`, `JPEG_LUMA_ONLY` or dithered) keeps only the luma. `JPEG_SCALE_QUARTER` keeps the first 5 coefficients of each block and a flag for each of the rest, 1/7 of the size, and `JPEG_SCALE_EIGHTH` only the DC, 1/64. On ESP32 it is allocated in PSRAM when there is PSRAM with room for it, whatever `CONFIG_SPIRAM_USE_CAPS_ALLOC` and `CONFIG_SPIRAM_USE_MALLOC` say, and otherwise in internal RAM only when that leaves `JPEG_INTERNAL_RESERVE` bytes of it (48K unless you define it) for WiFi, the network stack and other tasks; elsewhere with `malloc()`. Define `JPEG_MALLOC(size)` and `JPEG_FREE(p)` to use another allocator. If the allocation fails, only the DC of each block is kept, and the image is drawn at the scale asked for with each block in one colour. If even that fails, `decodeBegin()` fails with `JPEG_ERROR_MEMORY`, and can be called again, as with a smaller scale.

A file that ends early draws the scans that arrived. Grayscale and colour images with 1x1 chroma and up to 2x2 luma blocks are supported, as for baseline images, using Huffman tables 0 and 1.

## Tests

```sh
cd linux/step_test
make
```

This needs Python with Pillow, and `jpegtran` and `cjpeg` from libjpeg-turbo, to write the test images. `step_test` checks that decoding in steps gives the same output as `decode()`; `crop_test` checks cropping and framebuffer output at every scale; `prog_test` checks that progressive files made by `jpegtran` give exactly the same output as the baseline files they were made from, which hold the same coefficients, and that a progressive file cut short still draws.

## Releases

Releases are tagged `<upstream version>-esphome.<n>` from `main`. Fixes are pull requests against `main`.

## License

Apache License 2.0, see [LICENSE](LICENSE). Copyright 2020 BitBank Software, Inc.
