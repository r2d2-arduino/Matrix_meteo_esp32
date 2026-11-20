#include "hub75_dma.h"
#include <algorithm>
#include <cstring>
#include <cmath>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "HUB75";

Hub75::Hub75(const Hub75Config& c) : GFX_FB<16>(c.width, c.height), cfg_(c) {}

Hub75::~Hub75() {
    shutdown();
    for (int i = 0; i < 2; ++i) {
        if (buf_[i])  heap_caps_free(buf_[i]);
        if (desc_[i]) heap_caps_free(desc_[i]);
    }
}

uint32_t Hub75::oe_words_(int plane, uint8_t br) const {
    if (br == 0) return 0;
    uint32_t w = (uint32_t)base_oe_ * (1u << plane) * br / 255u;
    w = (w + 1) & ~1u;                 // округление ВВЕРХ до чётного
    return w < 2 ? 2 : w;              // минимум одна пара
}

size_t Hub75::calc_words_(uint8_t br) const {
    const size_t row_fixed = 4 * (size_t)phys_width + 2
                             + cfg_.latch_blanking + cfg_.pre_blank;

    size_t total = 0;
    for (int p = 0; p < depth_; ++p)
        total += (size_t)rows_ * (row_fixed + oe_words_(p, br));
    return total;
}

void Hub75::make_luts_() {
    const uint8_t maxv = (1u << depth_) - 1;
    for (int i = 0; i < 32; ++i) {
        float n = (cfg_.gamma > 0.01f) ? powf(i / 31.0f, cfg_.gamma) : (i / 31.0f);
        lut5_[i] = (uint8_t)lroundf(n * maxv);
    }
    for (int i = 0; i < 64; ++i) {
        float n = (cfg_.gamma > 0.01f) ? powf(i / 63.0f, cfg_.gamma) : (i / 63.0f);
        lut6_[i] = (uint8_t)lroundf(n * maxv);
    }
}

inline uint16_t Hub75::addr_(int row) const {
    uint16_t v = 0;
    if (row & 1)  v |= B_A;
    if (row & 2)  v |= B_B;
    if (row & 4)  v |= B_C;
    if (row & 8)  v |= B_D;
    if (row & 16) v |= B_E;
    return v;
}

// GFX framebuffer (RGB565) -> поток 16-битных слов для DMA
size_t IRAM_ATTR Hub75::build_(uint16_t* dst) {
    const uint16_t W = phys_width;
    const uint16_t* src = reinterpret_cast<const uint16_t*>(buffer);
    size_t p = 0;
    for (int pl = 0; pl < depth_; ++pl) {
        const uint8_t  m  = 1u << pl;
        const uint32_t on = oe_words_(pl, brightness_);
        for (int row = 0; row < rows_; ++row) {
            const uint16_t a = addr_(row);
            const uint16_t* t = src + (size_t)row * W;            // верхняя половина
            const uint16_t* b = src + (size_t)(row + rows_) * W;  // нижняя половина

            const uint16_t blank = a | B_OE;
            for (int x = 0; x < W; ++x) {
                int cx = x + pshift_; if (cx >= W) cx -= W;
                uint16_t ct = t[cx], cb = b[cx], v = blank;
                if (lut5_[ ct >> 11        ] & m) v |= B_R1;
                if (lut6_[(ct >> 5) & 0x3F ] & m) v |= B_G1;
                if (lut5_[ ct       & 0x1F ] & m) v |= B_B1;
                if (lut5_[ cb >> 11        ] & m) v |= B_R2;
                if (lut6_[(cb >> 5) & 0x3F ] & m) v |= B_G2;
                if (lut5_[ cb       & 0x1F ] & m) v |= B_B2;
                
                dst[p++] = v;          
                dst[p++] = v;           // CLK=0, обе половины одинаковы
                dst[p++] = v | B_CLK;  
                dst[p++] = v | B_CLK;   // CLK=1, обе половины одинаковы
                for (int h = 0; h < cfg_.clk_hold_pairs; ++h) {
                    dst[p++] = v;      dst[p++] = v;           // доп. hold (тоже парой)
                }
            }
            dst[p++] = blank | B_LAT;
            dst[p++] = blank | B_LAT;
            std::fill_n(dst + p, cfg_.latch_blanking, blank); 
            p += cfg_.latch_blanking;
            std::fill_n(dst + p, on, a);                      
            p += on;
            // НОВОЕ: гасим OE при НЕИЗМЕННОМ адресе текущей строки
            std::fill_n(dst + p, cfg_.pre_blank, blank);      
            p += cfg_.pre_blank;
        }
    }
    return p;
}

void Hub75::link_(int idx, size_t words) {
    if (words > cap_words_) { ESP_LOGE(TAG, "frame overflow"); return; }
    uint8_t* base = (uint8_t*)buf_[idx];
    size_t n = 0, off = 0;
    while (off < words) {
        size_t cnt = std::min(CHUNK_WORDS, words - off);
        auto& d = desc_[idx][n];
        d.size = cnt * 2; d.length = cnt * 2;
        d.offset = 0; d.sosf = 0; d.eof = 0; d.owner = 1;
        d.buf  = (uint32_t)(uintptr_t)(base + off * 2);
        d.next = 0;
        if (n) desc_[idx][n - 1].next = (uint32_t)(uintptr_t)&d;
        off += cnt; ++n;
    }
    desc_[idx][n - 1].eof  = 1;
    desc_[idx][n - 1].next = (uint32_t)(uintptr_t)&desc_[idx][0];  // кольцо = вечная регенерация
    words_[idx] = words;
    last_[idx]  = (int)n - 1;
}

void IRAM_ATTR Hub75::isr_(void* arg) {
    auto* self = static_cast<Hub75*>(arg);
    I2S0.int_clr.out_eof = 1;
    self->frames_+= 1;
    if (self->pending_) {                    // бесшовный flip на границе кадра
        self->pending_ = false;
        int f = self->front_, b = f ^ 1;
        self->desc_[f][self->last_[f]].next = (uint32_t)(uintptr_t)&self->desc_[b][0];
        self->front_ = b;
    }
}

bool Hub75::begin() {
    if (phys_height != 32 && phys_height != 64) {
        ESP_LOGE(TAG, "height must be 32 (1/8) or 64 (1/16)"); return false;
    }
    rows_       = phys_height / 2;
    depth_      = std::clamp<int>(cfg_.color_depth, 1, 5);
    base_oe_    = std::max<int>(cfg_.base_oe_words, 4);
    brightness_ = cfg_.brightness;
    make_luts_();

    pshift_ = cfg_.pixel_shift;
    if (cfg_.latch_blanking & 1) cfg_.latch_blanking++;
    if (cfg_.pre_blank      & 1) cfg_.pre_blank++;      // инвариант одинаковых пар

    const int8_t m[16] = {
        cfg_.pins.r1, cfg_.pins.g1, cfg_.pins.b1,
        cfg_.pins.r2, cfg_.pins.g2, cfg_.pins.b2,
        cfg_.pins.a,  cfg_.pins.b,  cfg_.pins.c, cfg_.pins.d, cfg_.pins.e,
        cfg_.pins.lat, cfg_.pins.oe, cfg_.pins.clk, -1, -1   // clk -> бит 13 (OUT21)
    };
    memcpy(bus_, m, sizeof(m));

    cap_words_ = calc_words_(255);
    cap_desc_  = (cap_words_ + CHUNK_WORDS - 1) / CHUNK_WORDS;
    for (int i = 0; i < nbuf(); ++i) {
        buf_[i]  = (uint16_t*)heap_caps_aligned_calloc(4, cap_words_ * 2, 1,
                                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        desc_[i] = (hub75_ll::DmaDesc*)heap_caps_calloc(cap_desc_, sizeof(hub75_ll::DmaDesc),
                                                      MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!buf_[i] || !desc_[i]) { ESP_LOGE(TAG, "no DMA memory"); return false; }
        link_(i, build_(buf_[i]));
    }
    ESP_LOGI(TAG, "%ux%u depth=%u, DMA %u B x%d", phys_width, phys_height,
             depth_, (unsigned)(cap_words_ * 2), nbuf());

    hub75_ll::BusCfg bc{bus_, -1, false, cfg_.clk_div};   // clk=-1: WS наружу не идёт
    hub75_ll::bus_init(bc);
    hub75_ll::bus_install_isr(&Hub75::isr_, this);
    hub75_ll::bus_start(desc_[0]);
    started_ = true;
    fps_t_ = esp_timer_get_time(); fps_f_ = frames_;
    return true;
}

void Hub75::show() {
    if (!started_) return;
    int b = cfg_.double_buffer ? (front_ ^ 1) : front_;
    //link_(b, build_(buf_[b]));
    
    size_t n = build_(buf_[b]);
    assert(n == calc_words_(brightness_));   // #include <cassert>
    link_(b, n);

    if (cfg_.double_buffer) pending_ = true;   // применится в ISR на границе кадра
}

void Hub75::rebuild_all_() {
    bool was = started_;
    if (was) hub75_ll::bus_stop();
    for (int i = 0; i < nbuf(); ++i) {
        size_t n = build_(buf_[i]);
        assert(n == calc_words_(brightness_));   // #include <cassert>
        link_(i, n);
    }
    front_ = 0; pending_ = false;
    if (was) hub75_ll::bus_start(desc_[0]);
}

void Hub75::set_brightness(uint8_t b) {
    brightness_ = b;
    if (started_) rebuild_all_();
}


// Меряем реальную скорость DMA и подбираем длительность LSB-плоскости под target_fps
void Hub75::auto_tune(uint16_t target_fps) {
    if (!started_ || target_fps == 0) return;
    uint32_t f0 = frames_; int64_t t0 = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(120));
    uint32_t df = frames_ - f0; int64_t dt = esp_timer_get_time() - t0;
    if (!df || dt <= 0) { ESP_LOGW(TAG, "auto_tune: no frames"); return; }

    double word_rate = (double)df * words_[front_] * 1e6 / (double)dt;   // слов/с
    ESP_LOGI(TAG, "pixel clock ~ %.2f MHz, %.1f fps", word_rate / 1e6, df * 1e6 / dt);

    const size_t row_fixed = 4 * (size_t)phys_width + 2
                             + cfg_.latch_blanking + cfg_.pre_blank;

    double fixed = (double)rows_ * depth_ * row_fixed;
    double w = (word_rate / target_fps - fixed) / ((double)rows_ * ((1 << depth_) - 1));
    base_oe_ = (uint16_t)std::max(4.0, w);
    while (base_oe_ > 4 && calc_words_(255) > cap_words_) base_oe_--;
    ESP_LOGI(TAG, "base_oe_words=%u -> ~%u fps", base_oe_, target_fps);
    rebuild_all_();
}

float Hub75::fps() {
    int64_t t = esp_timer_get_time(); uint32_t f = frames_;
    float r = (t > fps_t_) ? (f - fps_f_) * 1e6f / (float)(t - fps_t_) : 0.f;
    fps_t_ = t; fps_f_ = f;
    return r;
}

void Hub75::shutdown() {
    if (!started_) return;
    hub75_ll::bus_stop();
    started_ = false;
    if (cfg_.pins.oe >= 0) {                       // погасить панель
        esp_rom_gpio_connect_out_signal(cfg_.pins.oe, SIG_GPIO_OUT_IDX, false, false);
        gpio_set_direction((gpio_num_t)cfg_.pins.oe, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)cfg_.pins.oe, 1);
    }
}



void Hub75::set_pixel_shift(uint8_t s) {
    pshift_ = s;
    if (started_) rebuild_all_();
}