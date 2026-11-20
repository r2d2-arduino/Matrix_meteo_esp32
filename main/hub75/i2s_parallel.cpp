#include "i2s_parallel.h"
#include "soc/soc.h"
#include "soc/gpio_sig_map.h"
#include "esp_private/periph_ctrl.h"
#include "esp_rom_gpio.h"
#include "esp_intr_alloc.h"
#include "driver/gpio.h"

#define H75_DATA_SIG_OFS 8 
#define H75_I2S (&I2S0)

static void route_pin(int gpio, int sig, bool inv) {
    if (gpio < 0) return;
    gpio_reset_pin((gpio_num_t)gpio);
    gpio_set_direction((gpio_num_t)gpio, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability((gpio_num_t)gpio, GPIO_DRIVE_CAP_3);
    esp_rom_gpio_connect_out_signal(gpio, sig, inv, false);
}

void hub75_ll::bus_init(const BusCfg& c) {
    periph_module_enable(PERIPH_I2S0_MODULE);
    periph_module_reset(PERIPH_I2S0_MODULE);

    for (int i = 0; i < 16; ++i)
            route_pin(c.gpio[i], I2S0O_DATA_OUT0_IDX + H75_DATA_SIG_OFS + i, false);
    route_pin(c.clk, I2S0O_WS_OUT_IDX, c.inv_clk);

    i2s_dev_t* d = &I2S0;

    // ---------- сброс ----------
    d->conf.tx_start = 0;
    d->out_link.stop = 1;
    d->conf.tx_reset      = 1; d->conf.tx_reset      = 0;
    d->conf.tx_fifo_reset = 1; d->conf.tx_fifo_reset = 0;
    d->lc_conf.out_rst        = 1; d->lc_conf.out_rst        = 0;
    d->lc_conf.ahbm_rst       = 1; d->lc_conf.ahbm_rst       = 0;
    d->lc_conf.ahbm_fifo_rst  = 1; d->lc_conf.ahbm_fifo_rst  = 0;

    // ---------- I2S_CONF_REG ----------
    d->conf.val            = 0;
    d->conf.tx_slave_mod   = 0;   // master
    d->conf.tx_msb_right   = 1;
    d->conf.tx_right_first = 1;
    d->conf.tx_msb_shift   = 0;   // не «Philips I2S», а параллельная шина
    d->conf.tx_mono        = 0;
    d->conf.tx_short_sync  = 0;

    // ---------- I2S_CONF1_REG  <<< здесь pcm_bypass ----------
    d->conf1.val            = 0;
    d->conf1.tx_pcm_bypass  = 1;  // сырые данные, мимо PCM-компрессора
    d->conf1.tx_stop_en     = 0;  // НЕ останавливать TX при опустошении FIFO
    d->conf1.tx_zeros_rm_en = 0;

    // ---------- I2S_CONF2_REG ----------
    d->conf2.val            = 0;
    d->conf2.lcd_en         = 1;  // <<< LCD (parallel) mode
    d->conf2.camera_en      = 0;
    d->conf2.lcd_tx_wrx2_en = 0;  // 1 такт WS на слово (1 — удвоить частоту CLK)
    d->conf2.lcd_tx_sdx2_en = 0;
    d->conf2.data_enable    = 0;
    d->conf2.data_enable_test_en = 0;



    d->pdm_conf.val = 0;          // PDM выключен
    d->pd_conf.val  = 0;          // FIFO не в power-down

    // ---------- канал / FIFO ----------
    d->conf_chan.val         = 0;
    d->conf_chan.tx_chan_mod = 1; // один канал

    d->fifo_conf.val                  = 0;
    d->fifo_conf.tx_fifo_mod          = 1;  // 16 бит на сэмпл (2 байта = 1 такт CLK)
    d->fifo_conf.tx_fifo_mod_force_en = 1;
    d->fifo_conf.dscr_en              = 1;  // работа по DMA-дескрипторам

    // ---------- тактирование ----------
    d->sample_rate_conf.val            = 0;
    d->sample_rate_conf.tx_bits_mod    = 16;
    d->sample_rate_conf.tx_bck_div_num = 2;   // >= 2

    d->clkm_conf.val          = 0;
    d->clkm_conf.clk_en       = 1;            // <<< обязательно в IDF 5.x
    d->clkm_conf.clka_en      = 0;            // источник PLL_160M
    d->clkm_conf.clkm_div_a   = 1;
    d->clkm_conf.clkm_div_b   = 0;
    d->clkm_conf.clkm_div_num = c.clk_div;    // >= 2

    d->timing.val = 0;
    //d->timing.tx_ws_out_delay = 1;

    // ---------- DMA ----------
    d->lc_conf.val               = 0;
    d->lc_conf.out_data_burst_en = 1;
    d->lc_conf.indscr_burst_en   = 1;
    d->lc_conf.outdscr_burst_en  = 1;
    d->lc_conf.out_eof_mode      = 1;   // EOF после отправки последних данных
    d->lc_conf.check_owner       = 0;
    d->lc_conf.out_auto_wrback   = 0;

    d->int_ena.val = 0;
    d->int_clr.val = 0xFFFFFFFF;
}

void hub75_ll::bus_start(DmaDesc* first) {
    H75_I2S->out_link.addr  = ((uint32_t)(uintptr_t)first) & 0xFFFFF; // 20 бит
    H75_I2S->out_link.start = 1;
    H75_I2S->conf.tx_start  = 1;
}

void hub75_ll::bus_stop() {
    H75_I2S->conf.tx_start = 0;
    H75_I2S->out_link.stop = 1;
}

void hub75_ll::bus_install_isr(void (*isr)(void*), void* arg) {
    intr_handle_t h = nullptr;
    esp_intr_alloc(ETS_I2S0_INTR_SOURCE, ESP_INTR_FLAG_LEVEL1 | ESP_INTR_FLAG_IRAM, isr, arg, &h);
    H75_I2S->int_clr.val = 0xFFFFFFFF;
    H75_I2S->int_ena.out_eof = 1;
}