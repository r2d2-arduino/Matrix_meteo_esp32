#pragma once
#include <cstdint>
#include "../gfx/GFX_FB.h"
#include "i2s_parallel.h"

struct Hub75Pins {
    int8_t r1, g1, b1;
    int8_t r2, g2, b2;
    int8_t a, b, c, d, e;      // e = -1 для панели 1/8 scan (64x32)
    int8_t clk, lat, oe;
};

struct Hub75Config {
    Hub75Pins pins{};
    uint16_t width  = 64;
    uint16_t height = 64;        // 32 (1/8) или 64 (1/16)
    uint8_t  color_depth   = 4;  // 1..5 бит на канал
    uint8_t  clk_div       = 8;  // делитель I2S
    uint16_t base_oe_words = 24; // длительность младшего битплейна, тактов
    uint8_t  latch_blanking = 1; // гашение вокруг LAT (борьба с ghosting)
    uint8_t  brightness    = 255;
    float    gamma         = 2.2f;  // 0 = линейно
    bool     double_buffer = true;
    uint8_t  pixel_shift = 0;    // компенсация постоянного горизонтального сдвига строки
    uint8_t  clk_hold_words = 1;
    uint8_t  clk_hold_pairs = 0;
    uint8_t  pre_blank = 4;
};

class Hub75 : public GFX_FB<16> {
public:
    explicit Hub75(const Hub75Config& cfg);
    ~Hub75() override;

    bool begin();
    void show();                       // конвертация GFX-буфера -> DMA + flip
    void set_brightness(uint8_t b);
    void shutdown();                   // остановить DMA и погасить панель
    void auto_tune(uint16_t target_fps);// подбор таймингов по реальной частоте DMA
    float fps();
    uint32_t frame_count() const { return frames_; }

    // совместимость с 3-битной палитрой MicroPython: 1=B, 2=G, 4=R
    static uint32_t color8(uint8_t i) {
        return rgb(i & 4 ? 255 : 0, i & 2 ? 255 : 0, i & 1 ? 255 : 0);
    }

    void set_pixel_shift(uint8_t s);

private:
    enum : uint16_t {
        B_R1 = 1u<<0, B_G1 = 1u<<1, B_B1 = 1u<<2,
        B_R2 = 1u<<3, B_G2 = 1u<<4, B_B2 = 1u<<5,
        B_A  = 1u<<6, B_B  = 1u<<7, B_C  = 1u<<8, B_D = 1u<<9, B_E = 1u<<10,
        //B_LAT= 1u<<11, B_OE = 1u<<12          // OE активный низкий: 1 = погашено
        B_LAT= 1u<<11, B_OE = 1u<<12, B_CLK = 1u<<13   // CLK теперь бит данных
    };
    static constexpr size_t CHUNK_WORDS = 1024;   // 2048 байт на дескриптор

    Hub75Config cfg_;
    int8_t   bus_[16]{};
    uint16_t rows_ = 0;
    uint8_t  depth_ = 4, brightness_ = 255;
    uint16_t base_oe_ = 24;
    uint8_t  lut5_[32]{}, lut6_[64]{};
    uint16_t* buf_[2]{nullptr, nullptr};
    hub75_ll::DmaDesc* desc_[2]{nullptr, nullptr};
    size_t   cap_words_ = 0, cap_desc_ = 0, words_[2]{0, 0};
    int      last_[2]{0, 0};
    volatile int  front_ = 0;
    volatile bool pending_ = false;
    volatile uint32_t frames_ = 0;
    bool     started_ = false;
    int64_t  fps_t_ = 0; uint32_t fps_f_ = 0;

    int      nbuf() const { return cfg_.double_buffer ? 2 : 1; }
    uint32_t oe_words_(int plane, uint8_t br) const;
    size_t   calc_words_(uint8_t br) const;
    void     make_luts_();
    uint16_t addr_(int row) const;
    size_t   build_(uint16_t* dst);
    void     link_(int idx, size_t words);
    void     rebuild_all_();
    static void IRAM_ATTR isr_(void* arg);

    uint8_t pshift_ = 0;
    size_t fix_row();
};