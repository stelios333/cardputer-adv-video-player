#pragma once
// Written with AI assistance.

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <atomic>
#include "audio.h"
/*
 * ESP32-S3 + I2S audio : raw 16-bit PCM player from SD card
 *
 *  - ESP-IDF "new" I2S channel API (driver/i2s_std.h), Arduino-ESP32 core 3.x
 *  - Playback runs in its own FreeRTOS task, loop() stays free
 *  - Thread-safe control from loop(): play / pause / resume / stop / seek / volume
 *      * commands go through a FreeRTOS queue (never blocks the caller)
 *      * volume + status are lock-free atomics
 *  - DMA buffers are pre-filled (i2s_channel_preload_data) before the channel is
 *    enabled, on every start and every seek, so there is no stale audio / glitch
 *
 * Make a compatible file with:
 *   16-bit: ffmpeg -i in.mp3 -f s16le -acodec pcm_s16le -ar 44100 -ac 2 track.raw
 *    8-bit: ffmpeg -i in.mp3 -f u8    -acodec pcm_u8    -ar 22050 -ac 1 track.raw
 * (headerless, copy to SD root; set PCM_BITS / SAMPLE_RATE / AUDIO_CHANNELS to match)
 *
 * PCM_BITS = 16 : signed 16-bit little endian
 * PCM_BITS = 8  : UNSIGNED 8-bit (0..255, 128 = silence). It is converted to signed
 *                 16-bit on the fly because the MAX98357A / I2S bus runs at 16 bit.
 */

// ------------------------------ config -------------------------------------
static constexpr uint32_t SAMPLE_RATE    = 22050;
static constexpr uint8_t  AUDIO_CHANNELS = 1;    // 1 = mono, 2 = stereo
static constexpr uint8_t  PCM_BITS       = 8;   // file format: 16 = signed 16-bit LE, 8 = unsigned 8-bit

static_assert(PCM_BITS == 8 || PCM_BITS == 16, "PCM_BITS must be 8 or 16");
static_assert(AUDIO_CHANNELS == 1 || AUDIO_CHANNELS == 2, "AUDIO_CHANNELS must be 1 or 2");

static constexpr uint32_t FILE_FRAME_BYTES = AUDIO_CHANNELS * (PCM_BITS / 8); // bytes per frame in the file
static constexpr uint32_t OUT_FRAME_BYTES  = AUDIO_CHANNELS * 2;              // bytes per frame sent to I2S (always 16 bit)

// DMA: 4 descriptors * 256 frames = 1024 frames (~23 ms @ 44.1k).
// Small DMA = fast response to volume/pause/seek. Raise if SD reads stutter.
static constexpr uint32_t DMA_DESC_NUM   = 4;
static constexpr uint32_t DMA_FRAME_NUM  = 384;
static constexpr uint32_t DMA_BYTES      = DMA_DESC_NUM * DMA_FRAME_NUM * OUT_FRAME_BYTES; // output bytes

static constexpr size_t   OUT_CHUNK_BYTES  = DMA_FRAME_NUM * OUT_FRAME_BYTES;  // one chunk, I2S side
static constexpr size_t   FILE_CHUNK_BYTES = DMA_FRAME_NUM * FILE_FRAME_BYTES; // one chunk, SD side
// -----------------------------------------------------------------------------

class PcmPlayer {
public:
  enum State : int { STOPPED = 0, PLAYING = 1, PAUSED = 2 };

  bool begin(TwoWire *wire) {
    _wire = wire;
    _queue = xQueueCreate(8, sizeof(Cmd));
    if (!_queue) return false;
    if (!initI2S()) return false;
    setVolume(0.5f);
    return xTaskCreatePinnedToCore(taskTrampoline, "pcm_player", 6144, this,
                                   5, nullptr, 0) == pdPASS;   // core 0, loop() is on core 1
  }

  // ---- thread-safe API, call from loop() or anywhere (never blocks) ----
  bool play(const char* path) {
    Cmd c{CMD_PLAY, 0, {0}};
    strlcpy(c.path, path, sizeof(c.path));
    return send(c);
  }
  bool pause()           { return send({CMD_PAUSE,  0, {0}}); }
  bool resume()          { return send({CMD_RESUME, 0, {0}}); }
  bool stop()            { return send({CMD_STOP,   0, {0}}); }
  bool seekMs(uint32_t ms) { return send({CMD_SEEK, ms, {0}}); }

  // 0.0 .. 1.0 (squared curve for a more natural feel). Applied on the next chunk.
  void setVolume(float v) {
    v = constrain(v, 0.0f, 1.0f);
    _gainQ15.store((uint32_t)(v * v * 32768.0f), std::memory_order_relaxed);
    _volume.store(v, std::memory_order_relaxed);
  }

  float    volume()     const { return _volume.load(); }
  State    state()      const { return (State)_state.load(); }
  bool     isPlaying()  const { return state() == PLAYING; }
  uint32_t positionMs() const { return bytesToMs(_posBytes.load()); }
  uint32_t durationMs() const { return bytesToMs(_sizeBytes.load()); }

private:
  enum CmdType : uint8_t { CMD_PLAY, CMD_PAUSE, CMD_RESUME, CMD_STOP, CMD_SEEK };
  struct Cmd { CmdType type; uint32_t arg; char path[96]; };
  AudioDac _dac;
  TwoWire*             _wire;
  QueueHandle_t        _queue = nullptr;
  i2s_chan_handle_t    _tx    = nullptr;
  File                 _file;
  bool                 _chanEnabled = false;

  // shared with other threads
  std::atomic<int>      _state{STOPPED};
  std::atomic<uint32_t> _gainQ15{16384};
  std::atomic<float>    _volume{0.5f};
  std::atomic<uint32_t> _posBytes{0};
  std::atomic<uint32_t> _sizeBytes{0};

  // owned by the player task only
  alignas(4) uint8_t _buf[OUT_CHUNK_BYTES];   // 16-bit samples ready for I2S
  uint8_t _rd[PCM_BITS == 8 ? FILE_CHUNK_BYTES : 1]; // raw 8-bit read buffer
  size_t _bufLen = 0, _bufOff = 0;          // unsent bytes in _buf (output bytes)

  static uint32_t msToBytes(uint32_t ms) {
    return (uint32_t)(((uint64_t)ms * SAMPLE_RATE / 1000) * FILE_FRAME_BYTES);   // file bytes
  }
  static uint32_t bytesToMs(uint32_t b) {
    return (uint32_t)((uint64_t)(b / FILE_FRAME_BYTES) * 1000 / SAMPLE_RATE);    // file bytes
  }
  static uint32_t alignFrame(uint32_t b) { return b - (b % FILE_FRAME_BYTES); } // file bytes

  bool send(const Cmd& c) { return xQueueSend(_queue, &c, 0) == pdTRUE; }

  // --- I2S setup ---
  bool initI2S() {
    return _dac.begin(_wire, 0xCF, SAMPLE_RATE, AUDIO_CHANNELS-1, true) == ESP_OK;
  }



  // --- file / samples ---
  // Reads one chunk, frame-aligned, converts to signed 16-bit and applies volume.
  // Returns the number of OUTPUT bytes now in _buf (0 = end of file).
  size_t readChunk() {
    size_t out_bytes;
    int16_t* buf16 = (int16_t*)_buf;

    if constexpr (PCM_BITS == 16) {
      out_bytes = _file.read(_buf, OUT_CHUNK_BYTES);
      out_bytes -= out_bytes % OUT_FRAME_BYTES;
    } else {
      size_t in_bytes = _file.read(_rd, FILE_CHUNK_BYTES);
      in_bytes -= in_bytes % FILE_FRAME_BYTES;
      for (size_t i = 0; i < in_bytes; i++)                 // unsigned 8 bit -> signed 16 bit
        buf16[i] = (int16_t)(((int)_rd[i] - 128) << 8);
      out_bytes = in_bytes * 2;
    }
    if (out_bytes == 0) return 0;

    uint32_t g = _gainQ15.load(std::memory_order_relaxed);
    for (size_t i = 0, cnt = out_bytes / 2; i < cnt; i++)
        buf16[i] = (int16_t)(((int32_t)buf16[i] * (int32_t)g) >> 15);
    
    return out_bytes;
  }

  // Position actually being heard (in FILE bytes) = file position - data still queued.
  uint32_t estimatePlayedBytes() {
    uint32_t pos      = _file.position();
    uint32_t aheadOut = (uint32_t)(_bufLen - _bufOff) + (_chanEnabled ? DMA_BYTES : 0);
    uint32_t ahead    = (aheadOut / OUT_FRAME_BYTES) * FILE_FRAME_BYTES;   // output -> file bytes
    return pos > ahead ? alignFrame(pos - ahead) : 0;
  }

  // Pre-fill DMA, then enable the channel. Channel must be disabled.
  void startStream() {
    _bufLen = _bufOff = 0;
    for (;;) {
      size_t n = readChunk();
      if (n == 0) break;                              // very short file / EOF
      size_t loaded = 0;
      i2s_channel_preload_data(_tx, _buf, n, &loaded);
      if (loaded < n) { _bufLen = n; _bufOff = loaded; break; }   // DMA full, keep rest
    }
    if (_dac.chanEnable() == 0) _chanEnabled = true;
    _state.store(PLAYING);
  }

  void closeAndStop() {
    _dac.chanDisable();
    if (_file) _file.close();
    _bufLen = _bufOff = 0;
    _posBytes.store(0);
    _state.store(STOPPED);
  }

  // --- commands ---
  void handle(const Cmd& c) {
    switch (c.type) {
      case CMD_PLAY:
        closeAndStop();
        _file = SD.open(c.path, FILE_READ);
        if (!_file) { Serial.printf("[pcm] cannot open %s\n", c.path); _sizeBytes.store(0); break; }
        _sizeBytes.store(alignFrame(_file.size()));
        startStream();
        break;

      case CMD_PAUSE:
        if (state() == PLAYING) {
          uint32_t pos = estimatePlayedBytes();   // remember where we really were
          _dac.chanDisable();                          // drops whatever is in DMA
          _file.seek(pos);
          _bufLen = _bufOff = 0;
          _posBytes.store(pos);
          _state.store(PAUSED);
        }
        break;

      case CMD_RESUME:
        if (state() == PAUSED) startStream();     // re-prefills DMA
        break;

      case CMD_STOP:
        closeAndStop();
        break;

      case CMD_SEEK:
        if (_file) {
          uint32_t target = alignFrame(msToBytes(c.arg));
          uint32_t size   = _sizeBytes.load();
          if (target >= size) target = size ? size - FILE_FRAME_BYTES : 0;
          bool wasPlaying = (state() == PLAYING);
          _dac.chanDisable();                          // flush old audio
          _file.seek(target);
          _bufLen = _bufOff = 0;
          _posBytes.store(target);
          if (wasPlaying) startStream();          // pre-fill at new position
        }
        break;
    }
  }

  // --- streaming pump ---
  void pump() {
    if (_bufOff >= _bufLen) {                     // need new data
      _bufLen = readChunk();
      _bufOff = 0;
      if (_bufLen == 0) {                         // end of file
        static uint8_t silence[DMA_BYTES] = {0};  // let the DMA drain to the end
        size_t w;
        _dac.writeSamples(silence, sizeof(silence), &w, 500);
        closeAndStop();
        return;
      }
    }
    size_t written = 0;
    // Blocks until DMA has room (max 100 ms) -> naturally paces the task
    _dac.writeSamples(_buf + _bufOff, _bufLen - _bufOff, &written, 100);
    _bufOff += written;
    _posBytes.store(estimatePlayedBytes());
  }

  // --- task ---
  static void taskTrampoline(void* p) { ((PcmPlayer*)p)->run(); }

  void run() {
    for (;;) {
      // Idle: sleep until a command arrives. Playing: just poll the queue.
      TickType_t wait = (state() == PLAYING) ? 0 : portMAX_DELAY;
      Cmd c;
      while (xQueueReceive(_queue, &c, wait) == pdTRUE) {
        handle(c);
        wait = (state() == PLAYING) ? 0 : portMAX_DELAY;
      }
      if (state() == PLAYING) pump();
    }
  }
};
