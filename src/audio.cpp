#include "audio.h"

#include <cmath>

#include "esp_check.h"
#include "esp_log.h"

static const char* TAG = "ToneGenerator";

// ---------------------------------------------------------------------------
// ES8311 register addresses (subset needed for basic DAC playback).
// See the ES8311 datasheet for the full register map.
// ---------------------------------------------------------------------------
#define ES8311_RESET_REG00 0x00
#define ES8311_CLK_MANAGER_REG01 0x01
#define ES8311_CLK_MANAGER_REG02 0x02
#define ES8311_CLK_MANAGER_REG03 0x03
#define ES8311_CLK_MANAGER_REG04 0x04
#define ES8311_CLK_MANAGER_REG05 0x05
#define ES8311_CLK_MANAGER_REG06 0x06
#define ES8311_CLK_MANAGER_REG07 0x07
#define ES8311_CLK_MANAGER_REG08 0x08
#define ES8311_SDPIN_REG09 0x09
#define ES8311_SDPOUT_REG0A 0x0A
#define ES8311_SYSTEM_REG0B 0x0B
#define ES8311_SYSTEM_REG0C 0x0C
#define ES8311_SYSTEM_REG0D 0x0D
#define ES8311_SYSTEM_REG0E 0x0E
#define ES8311_SYSTEM_REG0F 0x0F
#define ES8311_SYSTEM_REG10 0x10
#define ES8311_SYSTEM_REG11 0x11
#define ES8311_SYSTEM_REG12 0x12
#define ES8311_SYSTEM_REG13 0x13
#define ES8311_SYSTEM_REG14 0x14
#define ES8311_ADC_REG16 0x16
#define ES8311_ADC_REG1B 0x1B
#define ES8311_ADC_REG1C 0x1C
#define ES8311_DAC_REG31 0x31 // bit6 = mute
#define ES8311_DAC_REG32 0x32 // DAC volume, 0x00=mute .. 0xFF=max
#define ES8311_DAC_REG37 0x37
#define ES8311_GP_REG45 0x45

AudioDac::AudioDac()
 {}

AudioDac::~AudioDac() {

 
}
void AudioDac::begin(TwoWire* wire, uint8_t vol, uint32_t sample_rate, bool stereo_mode, bool is_16bit) {
    if (_began) {
        return;
    }

    _volume = vol;
    _sample_rate = sample_rate;
    _slot_mode = stereo_mode ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO;
    _data_bit_width = is_16bit ? I2S_DATA_BIT_WIDTH_16BIT : I2S_DATA_BIT_WIDTH_8BIT;
    _Wire = wire;
    es8311Init();
    i2sInit();



    _began = true;
    ESP_LOGI(TAG, "AudioDac initialized");
}

void AudioDac::setAmpVolume(uint8_t vol) {
    _volume = vol;
    es8311WriteReg(ES8311_DAC_REG32, _volume);
}




esp_err_t AudioDac::es8311WriteReg(uint8_t reg, uint8_t val) {
    if (_Wire == nullptr) return 1;
    _Wire->beginTransmission(ES8311_ADDR);
    _Wire->write(reg);
    _Wire->write(val);
    return _Wire->endTransmission() == 0;
}
 
esp_err_t AudioDac::es8311ReadReg(uint8_t reg, uint8_t* val) {
    if (_Wire == nullptr) return 1;
    _Wire->beginTransmission(ES8311_ADDR);
    _Wire->write(reg);
    _Wire->endTransmission(false);

    _Wire->requestFrom(ES8311_ADDR, (uint8_t)1);
    if (_Wire->available()) {
        *val = _Wire->read();
    }
    return 0;
}



void AudioDac::es8311Init() {
    es8311WriteReg(ES8311_RESET_REG00, 0x80);   // RESET / CSM power on
    vTaskDelay(pdMS_TO_TICKS(20));
    es8311WriteReg(ES8311_CLK_MANAGER_REG01, 0xB5);   // CLK manager: MCLK = BCLK
    es8311WriteReg(ES8311_CLK_MANAGER_REG02, 0x18);   // CLK manager: premulti=3 -> внутр. клок = 256*fs
    es8311WriteReg(ES8311_SYSTEM_REG0D, 0x01);   // power up analog
    es8311WriteReg(ES8311_SYSTEM_REG12, 0x00);   // power up DAC
    es8311WriteReg(ES8311_SYSTEM_REG13, 0x10);   // enable output to drive
    
    // Output volume (0x00 mute .. 0xFF max)
    es8311WriteReg(ES8311_DAC_REG32, _volume);

    es8311WriteReg(ES8311_DAC_REG37, 0x08);  // bypass DAC equalizer

    setMute(true);
}

void AudioDac::setMute(bool mute) {
    if (!_began) return;
    uint8_t val = 0;
    es8311ReadReg(ES8311_DAC_REG31, &val);
    if (mute) {
        val |= 0x40;
    } else {
        val &= ~0x40;
    }
    es8311WriteReg(ES8311_DAC_REG31, val);
}

// ---------------------------------------------------------------------------
// I2S setup (ESP-IDF i2s_std driver, 3-wire master: BCLK/LRCK/DOUT)
// ---------------------------------------------------------------------------
void AudioDac::i2sInit() {
    i2s_chan_config_t chanCfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chanCfg, &_txHandle, nullptr));

    i2s_std_config_t stdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            _data_bit_width, _slot_mode),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = static_cast<gpio_num_t>(_pinSclk),
                .ws = static_cast<gpio_num_t>(_pinLrck),
                .dout = static_cast<gpio_num_t>(_pinDsdin),
                .din = I2S_GPIO_UNUSED,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(_txHandle, &stdCfg));
    ESP_ERROR_CHECK(i2s_channel_enable(_txHandle));
}

void AudioDac::writeSamples(void* buf, size_t len)
{
    size_t bytesWritten = 0;
    i2s_channel_write(_txHandle, buf, len, &bytesWritten,
                           portMAX_DELAY);
}