/*
ESP32-S3 video player designed for boards with no PSRAM (like the Cardputer ADV)
Encode videos with:
ffmpeg -i input.mp4 -c:v libx264 -profile:v baseline -tune fastdecode -preset veryslow\
 -vf "format=yuv420p, scale=240:136, fps=30" -x264opts slices=1:ref=1 -an output.264
And their audio tracks with:
ffmpeg -i input.mp4 -vn -ac 1 -ar 22050 -c:a pcm_u8 -f u8 input.pcm
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "lgfx_config.h"
#include "esp_heap_caps.h"
#include "message_box.h"
#include "file_picker.h"
#include "audio.h"
extern "C" {
#include "esp_h264_dec.h"
#include "esp_h264_dec_sw.h"
}



static LGFX tft;
CardputerKeypad keyboard;

// SD card
#define SD_CS 12
#define SD_MOSI 14
#define SD_MISO 39
#define SD_SCK 40
#define SD_FREQ 40000000

static SPIClass sdSPI(HSPI);
// Video
// Debug: 1 = ignore chroma and show only the Y plane (grayscale) to isolate colour problems
#define DEBUG_LUMA_ONLY 0
// 1 = pixels are byte-swapped by us and pushed as swap565_t (LovyanGFX does zero-copy DMA, no conversion)
// 0 = native RGB565 pushed as uint16_t (LovyanGFX converts into its own DMA buffer)
#define USE_SWAP565_PUSH 1

constexpr uint16_t VID_W = 240;
constexpr uint16_t VID_W_MB = (VID_W + 15) & ~15u;
constexpr uint16_t VID_H = 135;
constexpr uint16_t VID_H_MB = (VID_H + 15) & ~15u;

#define FRAME_US 33333  // 30 fps

#define STRIP_H 16  // rows per DMA strip (must be even)
static uint16_t *strip[2];  // two small RGB565 DMA buffers
static uint32_t stripCounter = 0;

static esp_h264_dec_handle_t dec = nullptr;
esp_h264_resolution_t h264_res {0,0};
esp_h264_dec_param_sw_handle_t dec_param;
// Plane pointers of the most recently decoded picture (owned by the decoder)
static const uint8_t *gY, *gU, *gV;
static size_t gSize = 0;  // size in bytes of the decoder's output buffer

// Annex-B NAL reader
#define NAL_BUF_SIZE (32 * 1024)
#define NAL_READ_CHUNK (8 * 1024)

static uint8_t *nalBuf = nullptr;
static size_t nalLen = 0;   // valid bytes in nalBuf
static size_t nalPos = 0;   // bytes of the previously returned NAL (dropped on next call)
static bool nalEof = false;
static bool nalOverflow = false;
static File vidFile;
static File audFile;

AudioDac audio_dac;
constexpr size_t BUF_SAMPLES = 735; // 735 samples per frame
volatile uint8_t raw_audio_buf[BUF_SAMPLES];
volatile uint8_t audio_vol = 0x0;
constexpr uint32_t AUDIO_SAMPLE_RATE = 22050;
volatile bool audioPlaying = false;
volatile bool audioPause = false;
volatile bool audioStopRequested = false;
TaskHandle_t audioTaskHandle = nullptr;
static portMUX_TYPE _spinlock = portMUX_INITIALIZER_UNLOCKED;

void audioTask(void *parameter)
{
    static int16_t audio_buf[BUF_SAMPLES];
    int16_t empty_buf[BUF_SAMPLES] {};

    while (true)
    {
        // Wait until playback is requested
        audio_dac.writeSamples((uint8_t*) empty_buf, BUF_SAMPLES * sizeof(int16_t));
        if (!audioPlaying)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        audioStopRequested = false;
        while (audioPlaying && !audioStopRequested)
        {

          taskENTER_CRITICAL(&_spinlock);
          // Convert unsinged 8-bit PCM to singed 16-bit PCM
            for (size_t i = 0; i < BUF_SAMPLES; i++) {
                audio_buf[i] = ((int16_t)raw_audio_buf[i] - 128) * audio_vol; // -32768 .. +32512
            }
            taskEXIT_CRITICAL(&_spinlock);

            audio_dac.writeSamples((uint8_t*) audio_buf, BUF_SAMPLES * sizeof(int16_t));
            audioPause = true;
            while (audioPause&&!audioStopRequested)
            {
              vTaskDelay(1);
            }
        }
        audioPlaying = false;
    }
}


std::optional<String> audioTrackExists(const String& name)
{
    const int dot = name.lastIndexOf('.');
    if (dot < 0) {
        return std::nullopt;
    }
    String audio_track_fname = name.substring(0, dot);
    audio_track_fname.toLowerCase();
    audio_track_fname += ".pcm";
    Serial.println(audio_track_fname);
    if(SD.exists(audio_track_fname)) return audio_track_fname;
    return std::nullopt;
}

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
  nalOverflow = false;
}

// Returns pointer/size of next NAL (including its start code). Pointer is valid until next call.
static bool nextNal(uint8_t *&p, size_t &n) {
  if (nalPos > 0) {
    memmove(nalBuf, nalBuf + nalPos, nalLen - nalPos);
    nalLen -= nalPos;
    nalPos = 0;
  }
  for (;;) {
    int first = (nalLen >= 3) ? findStartCode(nalBuf, nalLen, 0) : -1;
    if (first > 0) {  // skip leading junk
      memmove(nalBuf, nalBuf + first, nalLen - first);
      nalLen -= first;
    }
    if (first >= 0) {
      int next = (nalLen > 3) ? findStartCode(nalBuf, nalLen, 3) : -1;
      if (next >= 0) {
        size_t end = next;
        while (end > 3 && nalBuf[end - 1] == 0) end--;  // trailing zero of 4-byte start codes
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
    if (nalLen >= NAL_BUF_SIZE) {  // single NAL larger than the buffer
      nalOverflow = true;
      return false;
    }
    size_t want = min((size_t)NAL_READ_CHUNK, NAL_BUF_SIZE - nalLen);
    size_t r = vidFile.read(nalBuf + nalLen, want);
    if (r == 0) nalEof = true;
    else nalLen += r;
  }
}

// --- Color conversion ---
static inline uint8_t clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static inline uint16_t toRGB565(int y, int rOff, int gOff, int bOff) {
  int c = (y - 16) * 298 + 128;
  uint8_t r = clamp8((c + rOff) >> 8);
  uint8_t g = clamp8((c + gOff) >> 8);
  uint8_t b = clamp8((c + bOff) >> 8);
  uint16_t px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
#if USE_SWAP565_PUSH
  px = __builtin_bswap16(px);
#endif
  return px;
}

// Converts rows [rowStart, rowStart+nRows) of an I420 picture to RGB565 (BT.601 limited range).
// dst holds nRows * VID_W pixels.
static void i420ToRgb565(const uint8_t *yP, const uint8_t *uP, const uint8_t *vP,
                         int yStride, int cStride, int rowStart, int nRows, uint16_t *dst) {
  for (int r = 0; r < nRows; r += 2) {
    int row = rowStart + r;
    const uint8_t *y0 = yP + row * yStride;
    const uint8_t *y1 = y0 + yStride;
    const uint8_t *u = uP + (row >> 1) * cStride;
    const uint8_t *v = vP + (row >> 1) * cStride;
    uint16_t *d0 = dst + r * VID_W;
    uint16_t *d1 = d0 + VID_W;
    bool hasRow1 = (r + 1) < nRows;  // 135 is odd: the last row has no partner

    for (int x = 0; x < VID_W; x += 2) {
#if DEBUG_LUMA_ONLY
      int cu = 0, cv = 0;
#else
      int cu = u[x >> 1] - 128;
      int cv = v[x >> 1] - 128;
#endif
      int rOff = 516 * cu;
      int gOff = -100 * cu - 208 * cv;
      int bOff = 409 * cv;

      d0[x]     = toRGB565(y0[x],     rOff, gOff, bOff);
      d0[x + 1] = toRGB565(y0[x + 1], rOff, gOff, bOff);
      if (hasRow1) {
        d1[x]     = toRGB565(y1[x],     rOff, gOff, bOff);
        d1[x + 1] = toRGB565(y1[x + 1], rOff, gOff, bOff);
      }
    }
  }
}

// Convert + push the current picture to the display strip by strip, overlapping DMA with conversion.
static void showFrame() {
  for (int row = 0; row < VID_H; row += STRIP_H) {
    int n = min(STRIP_H, VID_H - row);
    uint16_t *buf = strip[stripCounter++ & 1];
    i420ToRgb565(gY, gU, gV, VID_W, VID_W / 2, row, n, buf);
    tft.waitDMA();  // previous strip finished (and buffer we just wrote is no longer in use)
#if USE_SWAP565_PUSH
    tft.pushImageDMA(0, row, VID_W, n, (const lgfx::swap565_t *)buf);
#else
    tft.pushImageDMA(0, row, VID_W, n, buf);
#endif
  }
}

// --- Decoder ---
static void printHeap(const char *tag) {
  Serial.printf("[%s] free internal: %u, largest block: %u\n", tag,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

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

// Feeds one NAL. Returns 1 if a picture was produced (planes in gY/gU/gV), 0 if not, -1 on error.
static int decodeNal(uint8_t *nal, size_t len) {
  esp_h264_dec_in_frame_t in = {};
  esp_h264_dec_out_frame_t out = {};
  in.raw_data.buffer = nal;
  in.raw_data.len = len;

  int result = 0;
  while (in.raw_data.len > 0) {
    esp_h264_err_t ret = esp_h264_dec_process(dec, &in, &out);
    
    if (h264_res.width == 0) {
      esp_h264_dec_sw_get_param_hd(dec, &dec_param);
      esp_h264_dec_get_resolution(dec_param, &h264_res);
    }
    
    if (ret != ESP_H264_ERR_OK) return -1;

    if (out.outbuf && out.out_size > 0) {
      size_t hy = h264_res.height;
      size_t wy = h264_res.width;
      static bool logged = false;
      if (!logged) {
        logged = true;
        Serial.printf("Decoder output: out_size=%u -> Y rows=%u (exact 4:2:0 size for %ux%u = %u)\n",
                      (unsigned)out.out_size, (unsigned)hy, (unsigned)hy, (unsigned)wy, (unsigned)(wy * hy));
                    }
      if (hy != VID_H_MB) {
        Serial.printf("Unexpected output size %u - assuming %hu rows\n", (unsigned)out.out_size, (unsigned short) VID_H_MB);
        hy = VID_H_MB;
      }
      if (wy != VID_W_MB) {
        Serial.printf("Unexpected output size %u - assuming %hu columns\n", (unsigned)out.out_size, (unsigned short) VID_W_MB);
        wy = VID_W_MB;
      }
      size_t ySize = wy * hy;
      size_t cPlane = (out.out_size - ySize) / 2;
      gSize = out.out_size;
      gY = out.outbuf;
      gU = gY + ySize;
      gV = gU + cPlane;
      result = 1;
    }
    if (in.consume == 0) break;
    in.raw_data.buffer += in.consume;
    in.raw_data.len -= in.consume;
  }
  return result;
}

// ---------------------------------------------------------------- Playback
static bool playFile(const char *path) {
  vidFile = SD.open(path, FILE_READ);
  if (!vidFile) {
    Serial.printf("Cannot open %s\n", path);
    return false;
  }
  
  std::optional<String> audio_fname = audioTrackExists(path);
  if (audio_fname != std::nullopt) {
    audFile = SD.open(audio_fname.value(), FILE_READ);
    if (audFile) {
      audioPlaying = true;
    }
  }
  audio_dac.setMute(!audioPlaying);
  Serial.printf("Playing %s (%u bytes)\n", path, (unsigned)vidFile.size());

  nalReset();
  printHeap("before decoder open");
  if (!decoderOpen()) {
    Serial.println("Decoder open failed");
    vidFile.close();
    return false;
  }
  printHeap("after decoder open");

  tft.startWrite();
  uint32_t frames = 0;
  int64_t nextDeadline = micros();
  uint32_t t0 = micros();
  bool ok = true;
  bool paused = false;
  bool fast_forward = true;
  uint8_t *nal;
  size_t nalSize;
  auto LadjustVol = [&](uint8_t new_vol) {
      String vol_ind = String(static_cast<uint8_t>((static_cast<float>(new_vol))/255.0f*100)) + "%";
      messageBoxDraw(tft, -15, "Volume", vol_ind.c_str(), 0xFFFF,
                           0xFFFF, 2, 2, 4);
      audio_vol = new_vol;
      keyboard.waitKeyReleased(';');
      keyboard.waitKeyReleased('.');
  };
  while (nextNal(nal, nalSize)) {
    int off = (nal[2] == 1) ? 3 : 4;
    uint8_t nalType = nal[off] & 0x1F;

    if (nalType == 7 && nalSize > (size_t)off + 3) {  // SPS: report profile/level
      Serial.printf("SPS: profile_idc=%u level_idc=%u (level %u.%u)\n",
                    nal[off + 1], nal[off + 3], nal[off + 3] / 10, nal[off + 3] % 10);
    }

    int r = decodeNal(nal, nalSize);
    if (r < 0) {
      Serial.printf("Decode error on NAL type %u - aborting file\n", nalType);
      printHeap("at error");
      ok = false;
      break;
    }
    if (r == 0) continue;  // SPS/PPS/SEI etc. produce no picture
    if (nalType != 1 && nalType != 5) continue;  // only show output of real slice NALs

    if (!fast_forward) {
      // Pace to 30 fps
      nextDeadline += FRAME_US;
      int64_t now = micros();
      if (now < nextDeadline) {
        int64_t wait = nextDeadline - now;
        if (wait > 2000) vTaskDelay(pdMS_TO_TICKS((uint32_t)(wait / 1000) - 1));  // sleep: lets IDLE run
        while ((int64_t)micros() < nextDeadline) { }                               // fine-grained remainder
      } else if (now - nextDeadline > 2 * FRAME_US) {
        nextDeadline = now;  // fell behind; resync instead of fast-forwarding
      }

    }
    if (!fast_forward||!(frames%10)) {
      showFrame();
    }
    frames++;
    if (audioPlaying) {
      if (fast_forward) {
        audFile.seek(audFile.position() + BUF_SAMPLES); // BUF_SAMPLES*2 for stereo
        memset((uint8_t *)raw_audio_buf, 0, BUF_SAMPLES);
        if (!audFile.available()) {
          audioStopRequested = true;
        }
      } else {
        size_t bytesRead = audFile.read(
                  const_cast<uint8_t*>(raw_audio_buf),
                  BUF_SAMPLES
              );
        if (!bytesRead) {
          audioStopRequested = true;
        }
      }
      
      audioPause = false;
    
    }
    
    do {
      keyboard.updateKeyStatus();
      if (keyboard.getKey('`')) {
        goto exit_video;
      } else if (keyboard.getKey('p')) {
        audio_dac.setMute(1);
        keyboard.waitAllKeysReleased();
        paused = !paused;
        if (!paused) {
          audio_dac.setMute(0);
        }
      } else if (keyboard.getKey(KEY_FN)) {
        if (keyboard.getKey('.')) {
          LadjustVol(audio_vol-0xf);
        } else if (keyboard.getKey(';')) {
          LadjustVol(audio_vol+0xf);
        }
      }
      fast_forward = keyboard.getKey('f');
      if (paused) {
        vTaskDelay(static_cast<TickType_t>(20 / portTICK_PERIOD_MS));
      } 
    } while (paused);
    
  }
  exit_video:
  if (nalOverflow) Serial.println("A NAL is bigger than NAL_BUF_SIZE - re-encode with lower bitrate or enlarge the buffer");

  tft.waitDMA();
  tft.endWrite();
  decoderClose();
  vidFile.close();  
  audFile.close();
  audioStopRequested = true;
  float secs = (micros() - t0) / 1e6f;
  Serial.printf("Done: %u frames, %.1f fps\n", (unsigned)frames, frames / secs);
  return ok;
}

static void showError(const char *msg) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_RED, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(4, 4);
  tft.println(msg);
  Serial.println(msg);
}

// Pushes red / green / blue bars through the exact same DMA path the video uses.
// Expected, top to bottom: RED, GREEN, BLUE.
static void displaySelfTest() {
  const uint16_t cols[3] = {0xF800, 0x07E0, 0x001F};
  tft.startWrite();
  for (int i = 0; i < 3; i++) {
    tft.waitDMA();
    for (int j = 0; j < VID_W * 15; j++) strip[0][j] = cols[i];
    for (int j = 0; j < 3; j++) {
      tft.waitDMA();
      tft.pushImageDMA(0, i * 45 + j * 15, VID_W, 15, strip[0]);
    }
  }
  tft.waitDMA();
  tft.endWrite();
  messageBoxDraw(tft, -15, "Running display test", "please wait...", TFT_WHITE, TFT_WHITE,2,2,4);
  delay(2000);
}


void setup() {
  Serial.begin(115200);
  //disableCore1WDT();  // decode loop can hog core 1 for long stretches when behind schedule
  Wire.begin(KEYPAD_SDA, KEYPAD_SCL, 400000);
  if(keyboard.begin(Wire)) {
    Serial.println("Failed to initialize keypad!");
    while (0) {};
  }

  tft.init();
  tft.setRotation(1);  // landscape 240x135 (use 3 if the image is upside down)
  tft.fillScreen(TFT_BLACK);

  audio_dac.begin(&Wire, 191U, AUDIO_SAMPLE_RATE, false);
  audio_dac.setMute(1);
  for (int i = 0; i < 2; i++) {
    strip[i] = (uint16_t *)heap_caps_malloc(VID_W * STRIP_H * 2, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!strip[i]) { showError("Strip buffer alloc failed"); while (true) delay(1000); }
  }
  nalBuf = (uint8_t *)heap_caps_malloc(NAL_BUF_SIZE, MALLOC_CAP_8BIT);
  if (!nalBuf) { showError("NAL buffer alloc failed"); while (true) delay(1000); }

  displaySelfTest();
  
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI, SD_FREQ, "/sd", 2)) {
    showError("SD init failed");
    while (true) delay(1000);
  }

  xTaskCreatePinnedToCore(
    audioTask,
    "AudioTask",
    4096,
    NULL,
    2,
    &audioTaskHandle,
    1
  );
  printHeap("setup done");
}

void loop() {
  char outPath[kMaxPath] {};
  tft.fillScreen(TFT_BLACK);

  while (!strcmp(outPath, "")) {
    filePicker(tft, outPath, kMaxPath);
  }
  tft.fillScreen(TFT_BLACK);
  messageBoxDraw(tft, -15, "Loading", "video file...", 0xFFFF,
                           0xFFFF, 2, 2, 4);
  playFile(outPath);
}

