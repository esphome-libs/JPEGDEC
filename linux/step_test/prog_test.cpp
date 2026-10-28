// Checks that a progressive JPEG file gives exactly the same draw callbacks and pixels as the baseline file it was
// made from by jpegtran, which holds the same DCT coefficients, whether it is decoded in one call or in steps as its
// bytes trickle in. Also checks that a progressive file cut short still draws the whole image, from the scans that
// arrived. The pairs of files are listed in the file named on the command line, a baseline and a progressive file
// on each line.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

// The coefficient buffer comes from these, which count the buffers held and can be made to fail
static int g_buffers = 0;     // allocated and not yet freed
static int g_fail_allocs = 0; // the next allocations to fail
static void *test_malloc(size_t size) {
  if (g_fail_allocs > 0) {
    g_fail_allocs--;
    return nullptr;
  }
  void *p = malloc(size);
  if (p != nullptr)
    g_buffers++;
  return p;
}
static void test_free(void *p) {
  g_buffers--;
  free(p);
}
#define JPEG_MALLOC(size) test_malloc(size)
#define JPEG_FREE(p) test_free(p)

#include "../../src/JPEGDEC.h"
#include "../../src/JPEGDEC.cpp"

// A copy would free the coefficient buffer twice
static_assert(!std::is_copy_constructible<JPEGDEC>::value && !std::is_copy_assignable<JPEGDEC>::value,
              "JPEGDEC can be copied");

struct Record {
  std::vector<int> geometry;
  std::vector<uint8_t> pixels;
  int calls{0};
};

// Only the pixels inside the image are compared: a progressive scan of one component leaves out the blocks that
// pad the MCUs at the right and bottom edges, which a baseline file has
static int draw_cb(JPEGDRAW *d) {
  auto *r = static_cast<Record *>(d->pUser);
  r->geometry.insert(r->geometry.end(), {d->x, d->y, d->iWidth, d->iWidthUsed, d->iHeight, d->iBpp});
  const size_t pitch = static_cast<size_t>(d->iWidth) * d->iBpp / 8, used = static_cast<size_t>(d->iWidthUsed) * d->iBpp / 8;
  const auto *p = reinterpret_cast<const uint8_t *>(d->pPixels);
  for (int y = 0; y < d->iHeight; y++)
    r->pixels.insert(r->pixels.end(), p + y * pitch, p + y * pitch + used);
  r->calls++;
  return 1;
}

// A file in memory that may only have part of its bytes delivered so far, like a slow download
struct MemFile {
  const std::vector<uint8_t> *data;
  int32_t delivered;
};

static int32_t read_cb(JPEGFILE *f, uint8_t *buf, int32_t len) {
  auto *m = static_cast<MemFile *>(f->fHandle);
  const int32_t n = std::max<int32_t>(0, std::min<int32_t>(len, m->delivered - f->iPos));
  memcpy(buf, m->data->data() + f->iPos, n);
  f->iPos += n;
  return n;
}

static int32_t seek_cb(JPEGFILE *f, int32_t pos) {
  f->iPos = pos;
  return pos;
}

static void close_cb(void *) {}

struct Config {
  const char *name;
  int pixel_type;
  int options;
  bool crop;
  bool framebuffer;
};

struct Result {
  int status{0};
  int error{0};
  int need_data_count{0};
  Record record;
  std::vector<uint8_t> framebuffer;
};

// The first bytes delivered before open(), enough for every test file's headers
static constexpr int32_t FIRST_DELIVERY = 4096;

// step 0 means a single decode(). trickle, when not 0, delivers the file this many bytes at a time between
// decodeRows() calls, after a first delivery of FIRST_DELIVERY bytes.
static Result run(const std::vector<uint8_t> &file, const Config &c, int step, int trickle) {
  static JPEGDEC jpeg;  // large; keep it off the stack
  Result res;
  const int32_t size = static_cast<int32_t>(file.size());
  MemFile mem{&file, trickle ? std::min(size, FIRST_DELIVERY) : size};
  if (!jpeg.open(&mem, size, close_cb, read_cb, seek_cb, draw_cb)) {
    res.error = jpeg.getLastError();
    return res;
  }
  jpeg.setUserPointer(&res.record);
  jpeg.setPixelType(c.pixel_type);
  if (c.crop)
    jpeg.setCropArea(16, 8, jpeg.getWidth() / 2, jpeg.getHeight() / 2);
  if (c.framebuffer) {
    const int bpp = c.pixel_type == RGB8888 ? 4 : (c.pixel_type == EIGHT_BIT_GRAYSCALE ? 1 : 2);
    res.framebuffer.assign(static_cast<size_t>(jpeg.getWidth() + 16) * (jpeg.getHeight() + 16) * bpp, 0xA5);
    jpeg.setFramebuffer(res.framebuffer.data());
  }
  if (step == 0) {
    res.status = jpeg.decode(0, 0, c.options);
  } else if (jpeg.decodeBegin(0, 0, c.options)) {
    for (;;) {
      res.status = jpeg.decodeRows(step);
      if (res.status == JPEG_DECODE_NEED_DATA) {
        res.need_data_count++;
        if (mem.delivered == size)
          break;  // waiting for bytes that will never come
      } else if (res.status != JPEG_DECODE_MORE) {
        break;
      }
      if (trickle)
        mem.delivered = std::min(size, mem.delivered + trickle);
    }
  }
  res.error = jpeg.getLastError();
  jpeg.close();
  return res;
}

static std::vector<uint8_t> load(const char *name) {
  std::vector<uint8_t> file;
  FILE *f = fopen(name, "rb");
  if (f == nullptr)
    return file;
  int ch;
  while ((ch = fgetc(f)) != EOF)
    file.push_back(static_cast<uint8_t>(ch));
  fclose(f);
  return file;
}

// Decode an image that is open with decodeBegin() and decodeRows()
static int decode_open(JPEGDEC &jpeg, int options, Record *rec) {
  jpeg.setUserPointer(rec);
  if (!jpeg.decodeBegin(0, 0, options))
    return 0;
  int status;
  while ((status = jpeg.decodeRows(1000)) == JPEG_DECODE_MORE) {
  }
  return status;
}

// Running out of memory, decoding after close(), opening another image part way through and bad frame sizes
static int edge_checks(const std::vector<uint8_t> &prog, const char *name, int *checks) {
  static JPEGDEC jpeg;
  int failures = 0;
  const int32_t size = static_cast<int32_t>(prog.size());
  MemFile mem{&prog, size};
  auto open = [&]() { return jpeg.open(&mem, size, close_cb, read_cb, seek_cb, draw_cb); };
  auto fail = [&](const char *what, int options) {
    printf("FAIL %s options %d: %s\n", name, options, what);
    failures++;
  };
  for (int options : {0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER}) {
    Record ref;
    open();
    const int ref_status = decode_open(jpeg, options, &ref);
    jpeg.close();
    // A decodeBegin() that had not the memory for even the DC can be retried
    (*checks)++;
    Record got;
    open();
    g_fail_allocs = 2;
    if (jpeg.decodeBegin(0, 0, options) || jpeg.getLastError() != JPEG_ERROR_MEMORY)
      fail("no JPEG_ERROR_MEMORY", options);
    g_fail_allocs = 0;
    if (decode_open(jpeg, options, &got) != ref_status || got.geometry != ref.geometry || got.pixels != ref.pixels)
      fail("retry after JPEG_ERROR_MEMORY differs", options);
    jpeg.close();
    // Without the memory for every coefficient, only the DC: blocks of one color, in the same places
    (*checks)++;
    Record dc;
    open();
    g_fail_allocs = 1;
    if (decode_open(jpeg, options, &dc) != 1 || dc.geometry != ref.geometry)
      fail("DC only, short of memory, differs", options);
    g_fail_allocs = 0;
    jpeg.close();
  }
  // Opening another image frees the buffer of one that was not finished
  (*checks)++;
  Record rec;
  open();
  jpeg.setUserPointer(&rec);
  jpeg.decodeBegin(0, 0, 0);
  jpeg.decodeRows(1);
  if (g_buffers != 1)
    fail("no buffer part way through", 0);
  open();
  if (g_buffers != 0)
    fail("open() kept the buffer", 0);
  jpeg.close();
  // decodeRows() after close() is refused
  (*checks)++;
  open();
  jpeg.setUserPointer(&rec);
  jpeg.decodeBegin(0, 0, 0);
  jpeg.close();
  if (jpeg.decodeRows(1) != 0 || jpeg.getLastError() != JPEG_INVALID_PARAMETER)
    fail("decodeRows() after close() not refused", 0);
  // An image is decoded once per open(): a second decodeBegin() is refused, also part way through the first,
  // which goes on as if it had not been called
  (*checks)++;
  Record ref, again;
  int status;
  open();
  decode_open(jpeg, 0, &ref);
  if (jpeg.decodeBegin(0, 0, 0) || jpeg.getLastError() != JPEG_INVALID_PARAMETER)
    fail("second decodeBegin() not refused", 0);
  jpeg.close();
  open();
  jpeg.setUserPointer(&again);
  jpeg.decodeBegin(0, 0, 0);
  jpeg.decodeRows(1);
  if (jpeg.decodeBegin(0, 0, 0) || jpeg.decode(0, 0, 0))
    fail("decodeBegin() part way through not refused", 0);
  while ((status = jpeg.decodeRows(1000)) == JPEG_DECODE_MORE) {
  }
  if (status != 1 || again.geometry != ref.geometry || again.pixels != ref.pixels)
    fail("decode after a refused decodeBegin() differs", 0);
  jpeg.close();
  // A JPEGDEC from malloc(), as JPEGDisplay makes, whose constructor did not run: open() does not free what is left
  // in its memory, but does free the buffer of an image that was not finished
  for (int fill : {0x00, 0xa5, 0xff}) {
    (*checks)++;
    void *raw = malloc(sizeof(JPEGDEC));
    memset(raw, fill, sizeof(JPEGDEC));
    auto *heap = static_cast<JPEGDEC *>(raw);
    Record got;
    if (!heap->open(&mem, size, close_cb, read_cb, seek_cb, draw_cb) || decode_open(*heap, 0, &got) != 1 ||
        got.pixels != ref.pixels || g_buffers != 0)
      fail("JPEGDEC from malloc() differs", fill);
    heap->open(&mem, size, close_cb, read_cb, seek_cb, draw_cb);
    heap->setUserPointer(&got);
    heap->decodeBegin(0, 0, 0);
    heap->decodeRows(1);
    heap->open(&mem, size, close_cb, read_cb, seek_cb, draw_cb);
    if (g_buffers != 0)
      fail("JPEGDEC from malloc() kept the buffer", fill);
    heap->close();
    free(raw);
  }
  // Color output after a decodeBegin() for grayscale, which keeps no chroma
  (*checks)++;
  open();
  jpeg.setUserPointer(&rec);
  jpeg.setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg.decodeBegin(0, 0, 0);
  jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
  while ((status = jpeg.decodeRows(1000)) == JPEG_DECODE_MORE) {
  }
  if (status != 1)
    fail("color after grayscale decodeBegin()", 0);
  jpeg.close();
  // A width or height of 0 is refused
  for (int field : {5, 7}) {
    (*checks)++;
    std::vector<uint8_t> zero = prog;
    for (size_t i = 0; i + 9 < zero.size(); i++) {
      if (zero[i] == 0xff && zero[i + 1] == 0xc2) {
        zero[i + field] = zero[i + field + 1] = 0;
        break;
      }
    }
    MemFile zero_mem{&zero, size};
    if (jpeg.open(&zero_mem, size, close_cb, read_cb, seek_cb, draw_cb)) {
      jpeg.setUserPointer(&rec);
      if (jpeg.decode(0, 0, 0) != 0)
        fail(field == 5 ? "height 0 decoded" : "width 0 decoded", 0);
      jpeg.close();
    }
  }
  if (g_buffers != 0)
    fail("buffers left", 0);
  return failures;
}

static const char *differs(const Result &got, const Result &ref, bool pixels = true) {
  if (got.status != ref.status)
    return "status";
  if (got.record.geometry != ref.record.geometry)
    return "callback geometry";
  if (pixels && got.record.pixels != ref.record.pixels)
    return "pixels";
  if (pixels && got.framebuffer != ref.framebuffer)
    return "framebuffer";
  return nullptr;
}

int main(int argc, char **argv) {
  static const Config configs[] = {
      {"rgb565", RGB565_LITTLE_ENDIAN, 0, false, false},
      {"rgb565be", RGB565_BIG_ENDIAN, 0, false, false},
      {"rgb8888", RGB8888, 0, false, false},
      {"gray", EIGHT_BIT_GRAYSCALE, 0, false, false},
      {"luma", RGB565_LITTLE_ENDIAN, JPEG_LUMA_ONLY, false, false},
      {"half", RGB565_LITTLE_ENDIAN, JPEG_SCALE_HALF, false, false},
      {"quarter", RGB565_LITTLE_ENDIAN, JPEG_SCALE_QUARTER, false, false},
      {"eighth", RGB8888, JPEG_SCALE_EIGHTH, false, false},
      {"crop", RGB565_LITTLE_ENDIAN, 0, true, false},
      {"framebuffer", RGB565_LITTLE_ENDIAN, 0, false, true},
      {"crop_quarter_framebuffer", RGB8888, JPEG_SCALE_QUARTER, true, true},
      {"quarter_eighth", RGB565_LITTLE_ENDIAN, JPEG_SCALE_QUARTER | JPEG_SCALE_EIGHTH, false, false},
  };
  if (argc != 2) {
    printf("usage: %s pairs.txt\n", argv[0]);
    return 2;
  }
  FILE *list = fopen(argv[1], "r");
  if (list == nullptr) {
    printf("cannot open %s\n", argv[1]);
    return 2;
  }
  int failures = 0, checks = 0;
  char base_name[1024], prog_name[1024];
  while (fscanf(list, "%1023s %1023s", base_name, prog_name) == 2) {
    const std::vector<uint8_t> base = load(base_name), prog = load(prog_name);
    if (base.empty() || prog.empty()) {
      printf("cannot open %s or %s\n", base_name, prog_name);
      return 2;
    }
    const bool large = prog.size() > 65536;
    for (const Config &c : configs) {
      const Result ref = run(base, c, 0, 0);
      if (ref.status != 1) {
        printf("FAIL %s %s: baseline decode status %d error %d\n", base_name, c.name, ref.status, ref.error);
        failures++;
        continue;
      }
      // In one call, in steps with all of it there, and in steps as it trickles in. With many rows a step, the
      // decoder catches up with the input and has to wait for it.
      const struct {
        int step, trickle;
      } ways[] = {{0, 0}, {1, 0}, {1000, 0}, {1, 61}, {3, 700}, {1000, large ? 997 : 61}, {1000, 700}};
      for (const auto &w : ways) {
        checks++;
        const Result got = run(prog, c, w.step, w.trickle);
        const char *why = differs(got, ref);
        if (why == nullptr && w.trickle == 0 && got.need_data_count != 0)
          why = "a pause with all the input there";
        if (why == nullptr && w.step == 1000 && w.trickle != 0 &&
            prog.size() > static_cast<size_t>(FIRST_DELIVERY) + JPEG_FILE_BUF_SIZE && got.need_data_count == 0)
          why = "no pause for data";
        if (why != nullptr) {
          printf("FAIL %s %s step %d trickle %d: %s differs (status %d/%d error %d, calls %d/%d)\n", prog_name, c.name,
                 w.step, w.trickle, why, got.status, ref.status, got.error, got.record.calls, ref.record.calls);
          failures++;
        }
      }
    }
    failures += edge_checks(prog, prog_name, &checks);
    // Cut short: the whole image is still drawn, from what arrived
    const Config &c = configs[0];
    const Result ref = run(base, c, 0, 0);
    for (int percent : {5, 30, 60, 90, 99}) {
      // At least enough for the headers
      const size_t size = std::min(prog.size(), std::max<size_t>(prog.size() * percent / 100, 600));
      const std::vector<uint8_t> cut(prog.begin(), prog.begin() + size);
      for (int step : {0, 2}) {
        checks++;
        const Result got = run(cut, c, step, 0);
        const char *why = differs(got, ref, false);
        if (why != nullptr) {
          printf("FAIL %s cut to %d%% step %d: %s differs (status %d/%d error %d, calls %d/%d)\n", prog_name, percent,
                 step, why, got.status, ref.status, got.error, got.record.calls, ref.record.calls);
          failures++;
        }
      }
    }
  }
  fclose(list);
  printf("%d progressive checks, %d failures\n", checks, failures);
  return failures != 0;
}
