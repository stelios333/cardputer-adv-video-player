/*
 * ESP32-S3 H.264 (Annex-B, baseline, 240x135) video player
 *
 *  - Decoding : esp_h264 (software decoder)
 *  - Storage  : Arduino SD (on its own SPI bus, HSPI/SPI3)
 *  - Display  : LovyanGFX (ST7789 on SPI2, DMA)
 *
 * Plays every *.264 file in the SD root, in a loop.
 *
 * Encode with:
 *   ffmpeg -i input.mp4 -c:v libx264 -profile:v baseline -tune fastdecode \
 *     -preset veryslow -vf "format=yuv420p, scale=240:135, fps=30" \
 *     -x264opts slices=1 -an OUTPUT.264
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <LovyanGFX.hpp>
#include "esp_heap_caps.h"

extern "C" {
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"
}

// ---------------------------------------------------------------- Display
class LGFX : public lgfx::LGFX_Device
{
private:
  lgfx::Panel_ST7789 _panel_instance;
  lgfx::Bus_SPI _bus_instance;

public:
  LGFX(void)
  {
    // Turn on backlight
    lgfx::gpio_hi(GPIO_NUM_38);
    lgfx::pinMode(GPIO_NUM_38, lgfx::v1::pin_mode_t::output);
    {
      auto cfg = _bus_instance.config();

      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 80000000;
      cfg.freq_read = 20000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;

      cfg.pin_sclk = 36;
      cfg.pin_mosi = 35;
      cfg.pin_miso = -1;
      cfg.pin_dc = 34;

      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }

    {
      auto cfg = _panel_instance.config();

      cfg.pin_cs = 37;
      cfg.pin_rst = 33;
      cfg.pin_busy = -1;

      cfg.panel_width = 135;
      cfg.panel_height = 240;
      cfg.offset_y = 40;
      cfg.offset_x = 52;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;
      cfg.invert = true;
      cfg.rgb_order = true;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;

      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};

static LGFX tft;

// ---------------------------------------------------------------- SD card
#define SD_CS 12
#define SD_MOSI 14
#define SD_MISO 39
#define SD_SCK 40
#define SD_FREQ 40000000  // lower to 20000000 if your card/wiring is flaky

static SPIClass sdSPI(HSPI);  // SPI3 - display owns SPI2

// ---------------------------------------------------------------- Video
#define VID_W 240
#define VID_H 135
#define FRAME_US 33333  // 30 fps

static uint16_t *fb[2];  // RGB565 DMA frame buffers
static int fbIdx = 0;

static esp_h264_dec_handle_t dec = nullptr;

// ---------------------------------------------------------------- Annex-B NAL reader
#define NAL_BUF_SIZE (96 * 1024)
#define NAL_READ_CHUNK (16 * 1024)

static uint8_t *nalBuf = nullptr;
static size_t nalLen = 0;   // valid bytes in nalBuf
static size_t nalPos = 0;   // bytes of the previously returned NAL (to drop on next call)
static bool nalEof = false;
static File vidFile;

static int findStartCode(const uint8_t *d, size_t n, size_t from) {
  for (size_t i = from; i + 2 < n; i++) {
    if (d[i + 2] > 1) { i += 2; continue; }
    if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) return (int)i;
  }
  return -1;
}

static void nalReset() {
  nalLen = nalPos = 0;
  nalEof = false;
}

// Returns pointer/size of next NAL (including its start code). Pointer is valid until next call.
static bool nextNal(uint8_t *&p, size_t &n) {
  if (nalPos > 0) {
    memmove(nalBuf, nalBuf + nalPos, nalLen - nalPos);
    nalLen -= nalPos;
    nalPos = 0;
  }
  for (;;) {
    // Make sure the buffer begins at a start code (skips any leading junk)
    int first = (nalLen >= 3) ? findStartCode(nalBuf, nalLen, 0) : -1;
    if (first > 0) {
      memmove(nalBuf, nalBuf + first, nalLen - first);
      nalLen -= first;
    }
    if (first >= 0) {
      int next = (nalLen > 3) ? findStartCode(nalBuf, nalLen, 3) : -1;
      if (next >= 0) {
        size_t end = next;
        while (end > 3 && nalBuf[end - 1] == 0) end--;  // trailing zero bytes (4-byte start codes)
        p = nalBuf;
        n = end;
        nalPos = next;
        return true;
      }
    }
    if (nalEof) {
      if (first < 0 || nalLen < 4) return false;
      size_t end = nalLen;
      while (end > 3 && nalBuf[end - 1] == 0) end--;
      p = nalBuf;
      n = end;
      nalPos = nalLen;
      return true;
    }
    if (nalLen >= NAL_BUF_SIZE) return false;  // NAL too large
    size_t want = min((size_t)NAL_READ_CHUNK, NAL_BUF_SIZE - nalLen);
    size_t r = vidFile.read(nalBuf + nalLen, want);
    if (r == 0) nalEof = true;
    else nalLen += r;
  }
}

// ---------------------------------------------------------------- Color conversion
static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static inline uint16_t toRGB565(int y, int rOff, int gOff, int bOff) {
  int c = (y - 16) * 298 + 128;
  uint8_t r = clamp8((c + rOff) >> 8);
  uint8_t g = clamp8((c + gOff) >> 8);
  uint8_t b = clamp8((c + bOff) >> 8);
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// I420 -> RGB565 (BT.601 limited range)
static void i420ToRgb565(const uint8_t *yP, const uint8_t *uP, const uint8_t *vP,
                         int yStride, int cStride, uint16_t *dst) {
  for (int row = 0; row < VID_H; row += 2) {
    const uint8_t *y0 = yP + row * yStride;
    const uint8_t *y1 = y0 + yStride;
    const uint8_t *u = uP + (row >> 1) * cStride;
    const uint8_t *v = vP + (row >> 1) * cStride;
    uint16_t *d0 = dst + row * VID_W;
    uint16_t *d1 = d0 + VID_W;
    bool hasRow1 = (row + 1) < VID_H;  // 135 is odd: last row has no partner

    for (int x = 0; x < VID_W; x += 2) {
      int cu = u[x >> 1] - 128;
      int cv = v[x >> 1] - 128;
      int rOff = 409 * cv;
      int gOff = -100 * cu - 208 * cv;
      int bOff = 516 * cu;

      d0[x]     = toRGB565(y0[x],     rOff, gOff, bOff);
      d0[x + 1] = toRGB565(y0[x + 1], rOff, gOff, bOff);
      if (hasRow1) {
        d1[x]     = toRGB565(y1[x],     rOff, gOff, bOff);
        d1[x + 1] = toRGB565(y1[x + 1], rOff, gOff, bOff);
      }
    }
  }
}

// ---------------------------------------------------------------- Decoder
static bool decoderOpen() {
  esp_h264_dec_cfg_sw_t cfg = {};
  cfg.pic_type = ESP_H264_RAW_FMT_I420;
  if (esp_h264_dec_sw_new(&cfg, &dec) != ESP_H264_ERR_OK) return false;
  if (esp_h264_dec_open(dec) != ESP_H264_ERR_OK) return false;
  return true;
}

static void decoderClose() {
  if (dec) {
    esp_h264_dec_close(dec);
    esp_h264_dec_del(dec);
    dec = nullptr;
  }
}

// Feeds one NAL; returns true if a full picture was produced and written into `dst`.
static bool decodeNal(uint8_t *nal, size_t len, uint16_t *dst) {
  esp_h264_dec_in_frame_t in = {};
  esp_h264_dec_out_frame_t out = {};
  in.raw_data.buffer = nal;
  in.raw_data.len = len;

  bool got = false;
  while (in.raw_data.len > 0) {
    esp_h264_err_t ret = esp_h264_dec_process(dec, &in, &out);
    if (ret != ESP_H264_ERR_OK) break;

    if (out.outbuf && out.out_size > 0) {
      // Planes: Y (240 x Hy), U, V. Hy is 135 if the decoder crops, 144 if it returns padded MBs.
      size_t hy = (out.out_size >= (size_t)VID_W * 144 * 3 / 2) ? 144 : VID_H;
      size_t ySize = (size_t)VID_W * hy;
      size_t cPlane = (out.out_size - ySize) / 2;
      const uint8_t *yP = out.outbuf;
      const uint8_t *uP = yP + ySize;
      const uint8_t *vP = uP + cPlane;
      i420ToRgb565(yP, uP, vP, VID_W, VID_W / 2, dst);
      got = true;
    }
    if (in.consume == 0) break;
    in.raw_data.buffer += in.consume;
    in.raw_data.len -= in.consume;
  }
  return got;
}

// ---------------------------------------------------------------- Playback
static bool playFile(const char *path) {
  vidFile = SD.open(path, FILE_READ);
  if (!vidFile) {
    Serial.printf("Cannot open %s\n", path);
    return false;
  }
  Serial.printf("Playing %s (%u bytes)\n", path, (unsigned)vidFile.size());

  nalReset();
  if (!decoderOpen()) {
    Serial.println("Decoder open failed");
    vidFile.close();
    return false;
  }

  tft.startWrite();
  uint32_t frames = 0;
  uint32_t t0 = micros();
  int64_t nextDeadline = t0;

  uint8_t *nal;
  size_t nalSize;
  while (nextNal(nal, nalSize)) {
    uint16_t *dst = fb[fbIdx];
    if (!decodeNal(nal, nalSize, dst)) continue;  // SPS/PPS/SEI etc. produce no picture

    // Pace to 30 fps
    nextDeadline += FRAME_US;
    int64_t now = micros();
    if (now < nextDeadline) {
      delayMicroseconds((uint32_t)(nextDeadline - now));
    } else if (now - nextDeadline > 2 * FRAME_US) {
      nextDeadline = now;  // fell behind; resync instead of fast-forwarding
    }

    tft.waitDMA();  // previous frame must be fully sent
    tft.pushImageDMA(0, 0, VID_W, VID_H, dst);
    fbIdx ^= 1;
    frames++;
  }

  tft.waitDMA();
  tft.endWrite();
  decoderClose();
  vidFile.close();

  float secs = (micros() - t0) / 1e6f;
  Serial.printf("Done: %u frames, %.1f fps\n", (unsigned)frames, frames / secs);
  return true;
}

static void showError(const char *msg) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.println(msg);
  Serial.println(msg);
}

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);  // landscape 240x135 (use 3 if the image is upside down)
  tft.fillScreen(TFT_BLACK);

  for (int i = 0; i < 2; i++) {
    fb[i] = (uint16_t *)heap_caps_malloc(VID_W * VID_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!fb[i]) { showError("Frame buffer alloc failed"); while (true) delay(1000); }
  }
  nalBuf = (uint8_t *)heap_caps_malloc(NAL_BUF_SIZE, MALLOC_CAP_8BIT);
  if (!nalBuf) { showError("NAL buffer alloc failed"); while (true) delay(1000); }

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, SD_FREQ)) {
    showError("SD init failed");
    while (true) delay(1000);
  }
}

void loop() {
  File root = SD.open("/");
  bool played = false;

  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String name = f.name();
    bool isVid = !f.isDirectory() && name.endsWith(".264") && !name.startsWith(".");
    f.close();
    if (!isVid) continue;

    String path = name.startsWith("/") ? name : "/" + name;
    played |= playFile(path.c_str());
  }
  root.close();

  if (!played) {
    showError("No .264 files on SD");
    delay(2000);
  }
}
