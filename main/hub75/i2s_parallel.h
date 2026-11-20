#pragma once
#include <cstdint>
#include "soc/i2s_struct.h"

namespace hub75_ll {

// Формат linked-list дескриптора DMA ESP32 (совместим с lldesc_t), ровно 12 байт
struct DmaDesc {
    volatile uint32_t size   : 12;   // размер буфера, байт (<= 4092)
    volatile uint32_t length : 12;   // сколько байт отдать
    volatile uint32_t offset : 5;
    volatile uint32_t sosf   : 1;
    volatile uint32_t eof    : 1;    // конец кадра -> прерывание
    volatile uint32_t owner  : 1;    // 1 = владеет DMA
    volatile uint32_t buf;
    volatile uint32_t next;          // указатель на следующий дескриптор (кольцо)
};
static_assert(sizeof(DmaDesc) == 12, "bad DMA descriptor layout");

struct BusCfg {
    const int8_t* gpio;   // 16 пинов шины, -1 = не используется
    int8_t  clk;          // CLK берётся с сигнала WS
    bool    inv_clk;
    uint8_t clk_div;      // N >= 2; реальную частоту меряем в auto_tune()
};

void bus_init(const BusCfg& cfg);
void bus_start(DmaDesc* first);
void bus_stop();
void bus_install_isr(void (*isr)(void*), void* arg);

} // namespace hub75_ll