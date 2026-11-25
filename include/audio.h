#pragma once

#include <atomic>
#include <cstdint>
#include <Wire.h>
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"


enum class Waveform {
    SINE = 1,
    SQUARE
};

class AudioDac {
public:
    AudioDac();
    ~AudioDac();

    // Sets up I2C, configures the ES8311 registers, and sets up the I2S
    // peripheral + audio task. Call once before play().
    bool begin(TwoWire* wire, uint8_t vol=0xBF, uint32_t sample_rate = 44100, bool is_stereo = true, bool is_16bit = true);

    void setAmpVolume(uint8_t vol);
    void setAmpMute(bool mute);
    void writeSamples(void* buf, size_t len, size_t *bytes_written, uint32_t timeout_ms=100);
    bool chanDisable();

    bool chanEnable();

private:
    // ---- Pin config ----
    int _pinSclk = 41;
    int _pinLrck = 43;
    int _pinDsdin = 42;

    uint8_t _volume;
    bool _began = false;
    // --- I2C config ---
    static constexpr uint8_t ES8311_ADDR = 0x18; // 7-bit address (CE pin low)
    TwoWire* _Wire = nullptr; 

    // ---- I2S config ----
    static constexpr i2s_port_t I2S_PORT = I2S_NUM_0;
    uint32_t _sample_rate;
    i2s_chan_handle_t _txHandle = nullptr;
    i2s_slot_mode_t _slot_mode;
    i2s_data_bit_width_t _data_bit_width;
    // ---- ES8311 low-level control ----
    void i2cInit();
    esp_err_t es8311WriteReg(uint8_t reg, uint8_t val);
    esp_err_t es8311ReadReg(uint8_t reg, uint8_t* val);
    void es8311Init();
    
 
    // ---- I2S setup ----
    void i2sInit();

    
};