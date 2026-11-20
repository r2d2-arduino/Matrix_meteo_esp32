// *** GFX_FB V 0.2.9 *** //
#pragma once
#include <stdlib.h>
#include <cmath>
#include "Rotation.h"
#include "../../fonts/gfx_font.h" 

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

enum class Colors {
    BLACK,
    BLUE,
    RED,
    GREEN,
    CYAN,
    MAGENTA,
    YELLOW,
    WHITE,
    GREY
};



template <uint8_t BITS_PER_PIXEL>
class GFX_FB {
private:
    const Font* default_font = nullptr;
public:
    uint8_t* buffer;
    void* buffer_alloc = nullptr;

    uint16_t screen_width;
    uint16_t screen_height;
    
    uint16_t phys_width;    // Физическая ширина матрицы (всегда 320)
    uint16_t phys_height;   // Физическая высота матрицы (всегда 480)

    uint32_t total_pixels;
    uint32_t total_bytes;
    Rotation rotation;


    // Универсальный метод конвертации RGB
    static inline uint32_t rgb(uint8_t red, uint8_t green, uint8_t blue) {
        if constexpr (BITS_PER_PIXEL == 16) {
            // Базовое формирование 16-битного цвета (RGB565)
            uint16_t color = (((red >> 3) & 0x1F) << 11) | (((green >> 2) & 0x3F) << 5) | ((blue >> 3) & 0x1F);
            
    #if defined(LCD_BUFFER_SWAP_BYTES)
        // swap для тех сборок, где нужен (SPI DMA и т.п.)
        return (uint32_t)((color >> 8) | (color << 8));
    #else
        // обычный порядок (для MIPI/MPI/DPI и esp32p4)
        return (uint32_t) color;
    #endif
        } else if constexpr (BITS_PER_PIXEL >= 18) {
            return ((uint32_t)red << 16) | ((uint32_t)green << 8) | (uint32_t)blue;
        }
        return 0; // Для 4-битного режима эта функция обычно не используется
    }

    static inline uint32_t get_color(Colors color_name) {
        switch (color_name) {
            case Colors::BLACK:   return rgb(0, 0, 0);
            case Colors::BLUE:    return rgb(0, 0, 255);
            case Colors::RED:     return rgb(255, 0, 0);
            case Colors::GREEN:   return rgb(0, 255, 0);
            case Colors::CYAN:    return rgb(0, 255, 255);
            case Colors::MAGENTA: return rgb(255, 0, 255);
            case Colors::YELLOW:  return rgb(255, 255, 0);
            case Colors::WHITE:   return rgb(255, 255, 255);
            case Colors::GREY:    return rgb(112, 160, 112); // Используем ваш оттенок серого
            default:                 return rgb(0, 0, 0);       // Возврат черного при ошибке
        }
    }
    

    GFX_FB(uint16_t width, uint16_t height) {
        this->phys_width = width;
        this->phys_height = height;

        this->screen_width = width;
        this->screen_height = height;
        
        this->rotation = Rotation::DEGREE_0;
        
        this->total_pixels = (uint32_t) width * height;
        
        if constexpr (BITS_PER_PIXEL == 16) {
            this->total_bytes = total_pixels * 2;
        } else if constexpr (BITS_PER_PIXEL >= 18) {
            this->total_bytes = total_pixels * 3;
        } else {
            this->total_bytes = total_pixels * BITS_PER_PIXEL / 8; 
        }

    #ifdef ESP_PLATFORM
        uint32_t caps = MALLOC_CAP_DMA;

        #if defined(CONFIG_SPIRAM)
            caps = MALLOC_CAP_SPIRAM;
        #endif        

        this->buffer_alloc = heap_caps_aligned_alloc(128, total_bytes, caps);
        if (!this->buffer_alloc) {
            // fallback: выделяем чуть больше и выравниваем вручную
            void* raw = heap_caps_malloc(total_bytes + 127, caps);
            if (raw) {
                uintptr_t aligned = ((uintptr_t)raw + 127) & ~(uintptr_t)127;
                this->buffer = (uint8_t*)aligned;
                this->buffer_alloc = raw;
            } else {
                this->buffer = nullptr; // обработка ошибки — нет памяти
            }
        } else {
            this->buffer = (uint8_t*)this->buffer_alloc;
        }
    #else
        this->buffer = new uint8_t[total_bytes]{0};
        this->buffer_alloc = this->buffer;
    #endif
    }

    virtual ~GFX_FB() {
    #ifdef ESP_PLATFORM
        if (buffer_alloc) heap_caps_free(buffer_alloc);
    #else
        delete[] buffer;
    #endif
    }

    void swap_hw() {
        uint16_t temp = screen_width;
        this->screen_width = screen_height;
        this->screen_height = temp;
    }
    
    void set_soft_rotation(bool enable) {
        set_rotation(enable ? Rotation::DEGREE_90 : Rotation::DEGREE_0);
    }

    void set_rotation(Rotation rot) {
        this->rotation = rot;
        if (rot == Rotation::DEGREE_90 || rot == Rotation::DEGREE_270) {
            this->screen_width  = this->phys_height;
            this->screen_height = this->phys_width;
        } else {
            this->screen_width  = this->phys_width;
            this->screen_height = this->phys_height;
        }
    }

    void clear( uint32_t color = 0 ){
        if constexpr (BITS_PER_PIXEL == 4) {
            uint16_t color8 = (uint8_t) color;
            uint8_t fill_val = (color8 & 0x0F) | ((color8 & 0x0F) << 4);
            
            for (int i = 0; i < total_bytes; i++) {
                buffer[i] = fill_val;
            }
        } else if constexpr (BITS_PER_PIXEL == 16) {
            uint16_t* buf16 = (uint16_t*) buffer;
            uint16_t c16 = (uint16_t) color;

            for (int i = 0; i < total_bytes / 2; i++) {
                buf16[i] = c16;
            }
        } else if constexpr (BITS_PER_PIXEL >= 18) {
            uint8_t r = (uint8_t) (color >> 16);
            uint8_t g = (uint8_t) (color >> 8);
            uint8_t b = (uint8_t)  color;

            for (int i = 0; i < total_bytes / 3; i++) {
                buffer[i * 3    ] = r;
                buffer[i * 3 + 1] = g;
                buffer[i * 3 + 2] = b;
            }
        }
    }
    
    void draw_pixel(int16_t x, int16_t y, uint32_t color) {
        if (x < 0 || x >= screen_width || y < 0 || y >= screen_height) return;

        // Трансляция логических координат в физическое пространство фреймбуфера
        int16_t px = x, py = y;
        switch (rotation) {
            case Rotation::DEGREE_90:
                px = phys_width - 1 - y;
                py = x;
                break;
            case Rotation::DEGREE_180:
                px = phys_width - 1 - x;
                py = phys_height - 1 - y;
                break;
            case Rotation::DEGREE_270:
                px = y;
                py = phys_height - 1 - x;
                break;
            default: // Rotation::DEGREE_0
                px = x;
                py = y;
                break;
        }

        if constexpr (BITS_PER_PIXEL == 4) {
            uint32_t idx = (px + (py * phys_width)) / 2;
            uint8_t c = color & 0x0F;
            if (px % 2 == 0) buffer[idx] = (buffer[idx] & 0x0F) | (c << 4);
            else             buffer[idx] = (buffer[idx] & 0xF0) | c;
        } 
        else if constexpr (BITS_PER_PIXEL == 16) {
            ((uint16_t*)buffer)[px + (py * phys_width)] = (uint16_t) color;
        } 
        else if constexpr (BITS_PER_PIXEL >= 18) {
            uint32_t pos = (px + (py * phys_width)) * 3;
            buffer[pos]     = (uint8_t)(color >> 16);
            buffer[pos + 1] = (uint8_t)(color >> 8);
            buffer[pos + 2] = (uint8_t)color;
        }
    }

    void fill_rect(int16_t x, int16_t y, int16_t width, int16_t height, uint32_t color) {
        if (width <= 0 || height <= 0) return;
        int16_t x0 = (x < 0) ? 0 : x;
        int16_t y0 = (y < 0) ? 0 : y;
        int16_t x1 = x + width;
        int16_t y1 = y + height;
        if (x1 > screen_width) x1 = screen_width;
        if (y1 > screen_height) y1 = screen_height;

        // Трансляция границ логического прямоугольника в физический
        int16_t px0 = x0, py0 = y0, px1 = x1, py1 = y1;
        switch (rotation) {
            case Rotation::DEGREE_90:
                px0 = phys_width - y1;   py0 = x0;
                px1 = phys_width - y0;   py1 = x1;
                break;
            case Rotation::DEGREE_180:
                px0 = phys_width - x1;   py0 = phys_height - y1;
                px1 = phys_width - x0;   py1 = phys_height - y0;
                break;
            case Rotation::DEGREE_270:
                px0 = y0;                py0 = phys_height - x1;
                px1 = y1;                py1 = phys_height - x0;
                break;
            default: // Rotation::DEGREE_0
                px0 = x0;  py0 = y0;
                px1 = x1;  py1 = y1;
                break;
        }

        if constexpr (BITS_PER_PIXEL == 4) {
            for (int16_t ypos = y0; ypos < y1; ypos++) {
                for (int16_t xpos = x0; xpos < x1; xpos++) { 
                    draw_pixel(xpos, ypos, color); // 4-bit безопаснее прогнать через пиксели
                }
            }
        } 
        else if constexpr (BITS_PER_PIXEL == 16) {
            uint16_t* buf16 = (uint16_t*)buffer;
            uint16_t c16 = (uint16_t)color;
            // Цикл всегда идет по физическим координатам!
            for (int16_t ypos = py0; ypos < py1; ypos++) {
                uint32_t offset = (uint32_t)ypos * phys_width;
                for (int16_t xpos = px0; xpos < px1; xpos++) { 
                    buf16[xpos + offset] = c16;
                }
            }
        } 
        else if constexpr (BITS_PER_PIXEL >= 18) {
            uint8_t r = (uint8_t)(color >> 16), g = (uint8_t)(color >> 8), b = (uint8_t)color;
            for (int16_t ypos = py0; ypos < py1; ypos++) {
                uint32_t offset = (uint32_t)ypos * phys_width;
                for (int16_t xpos = px0; xpos < px1; xpos++) { 
                    uint32_t pos = (xpos + offset) * 3;
                    buffer[pos] = r; buffer[pos + 1] = g; buffer[pos + 2] = b;
                }
            }
        }
    }

    void draw_image(int16_t x, int16_t y, int16_t width, int16_t height, const uint16_t* bitmap) {
        if constexpr (BITS_PER_PIXEL == 16) {
            uint16_t* buf16 = (uint16_t*)buffer;
            for (int16_t row = 0; row < height; row++) {
                int32_t dy = y + row;
                if (dy < 0 || dy >= screen_height) continue;
                
                const uint16_t* src = bitmap + (size_t)row * width;
                
                for (int16_t col = 0; col < width; col++) {
                    int32_t dx = x + col;
                    if (dx < 0 || dx >= screen_width) continue;
                    uint16_t c = src[col];
            #if defined(LCD_BUFFER_SWAP_BYTES)
                    c = (c >> 8) | (c << 8);
            #endif
                    // Рассчет конечного физического индекса в памяти
                    uint32_t idx;
                    switch (rotation) {
                        case Rotation::DEGREE_90:  
                            idx = (phys_width - 1 - dy) + dx * phys_width; 
                            break;
                        case Rotation::DEGREE_180: 
                            idx = (phys_width - 1 - dx) + (phys_height - 1 - dy) * phys_width; 
                            break;
                        case Rotation::DEGREE_270: 
                            idx = dy + (phys_height - 1 - dx) * phys_width; 
                            break;
                        default:      
                            idx = dx + dy * phys_width; 
                            break;
                    }
                    buf16[idx] = c;
                }
            }
        } else if constexpr (BITS_PER_PIXEL >= 18) {
            uint32_t pixel_idx = 0;
            for (int16_t row = 0; row < height; row++) {
                int32_t dy = y + row;
                for (int16_t col = 0; col < width; col++) {
                    int32_t dx = x + col;
                    uint16_t color = bitmap[pixel_idx++];
                    
                    if (dy < 0 || dy >= screen_height || dx < 0 || dx >= screen_width) continue;
                    
                    uint8_t r = ((color >> 11) & 0x1F) << 3;
                    uint8_t g = ((color >> 5)  & 0x3F) << 2;
                    uint8_t b =  (color & 0x1F) << 3;

                    uint32_t idx;
                    switch (rotation) {
                        case Rotation::DEGREE_90:  
                            idx = ((phys_width - 1 - dy) + dx * phys_width) * 3; 
                            break;
                        case Rotation::DEGREE_180: 
                            idx = ((phys_width - 1 - dx) + (phys_height - 1 - dy) * phys_width) * 3; 
                            break;
                        case Rotation::DEGREE_270: 
                            idx = (dy + (phys_height - 1 - dx) * phys_width) * 3; 
                            break;
                        default:      
                            idx = (dx + dy * phys_width) * 3; 
                            break;
                    }
                    buffer[idx] = r; buffer[idx + 1] = g; buffer[idx + 2] = b;
                }
            }
        }
    }

    void draw_image(int16_t x, int16_t y, int16_t width, int16_t height, const uint8_t* bitmap, uint8_t color = 1) {
        uint32_t pixel_idx = 0;
        if constexpr (BITS_PER_PIXEL == 4) {
            for (uint16_t h = 0; h < height; h++) {
                for (uint16_t w = 0; w < width; w++) {
                    uint8_t byte = bitmap[pixel_idx / 8];
                    if ((byte >> (7 - (pixel_idx % 8))) & 0x01) draw_pixel(x + w, y + h, color);
                    pixel_idx++;
                }
            }
        } else if constexpr (BITS_PER_PIXEL >= 18) {
            for ( uint16_t row = 0; row < height; row++ ) {
                int32_t dy = y + row;
                for ( uint16_t col = 0; col < width; col++ ) {
                    int32_t dx = x + col;
                    
                    uint8_t r = bitmap[ pixel_idx++ ], g = bitmap[ pixel_idx++ ], b = bitmap[ pixel_idx++ ];
                    
                    if (dy < 0 || dy >= screen_height || dx < 0 || dx >= screen_width) continue;
                    
                    uint32_t idx;
                    switch (rotation) {
                        case Rotation::DEGREE_90:  
                            idx = ((phys_width - 1 - dy) + dx * phys_width) * 3; 
                            break;
                        case Rotation::DEGREE_180: 
                            idx = ((phys_width - 1 - dx) + (phys_height - 1 - dy) * phys_width) * 3; 
                            break;
                        case Rotation::DEGREE_270: 
                            idx = (dy + (phys_height - 1 - dx) * phys_width) * 3; 
                            break;
                        default:      
                            idx = (dx + dy * phys_width) * 3; 
                            break;
                    }

                    buffer[idx] = r; buffer[idx + 1] = g; buffer[idx + 2] = b;
                }
            }
        }
    }

    // ПРОИЗВОДНЫЕ МЕТОДЫ (Им не нужен if constexpr, они используют методы выше)

    void fill_screen(uint32_t color) {
        fill_rect(0, 0, screen_width, screen_height, color);
    }

    void draw_rect(int16_t x, int16_t y, int16_t width, int16_t height, uint32_t color, int16_t thickness = 1) {
        fill_rect(x, y, width, thickness, color);
        fill_rect(x, y + height - thickness, width, thickness, color);
        fill_rect(x, y, thickness, height, color);
        fill_rect(x + width - thickness, y, thickness, height, color);
    }

    void draw_vline(int16_t x, int16_t y, int16_t height, uint32_t color, int16_t thickness = 1) {
        fill_rect(x, y, thickness, height, color);
    }

    void draw_hline(int16_t x, int16_t y, int16_t width, uint32_t color, int16_t thickness = 1) {
        fill_rect(x, y, width, thickness, color);
    }

    void draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint32_t color) {
        //if (y0 == y1) { draw_hline(x0, y0, x1 - x0 + 1, color); return; }
        //if (x0 == x1) { draw_vline(x0, y0, y1 - y0 + 1, color); return; }

        if (y0 == y1) { if (x0 > x1) std::swap(x0, x1); draw_hline(x0, y0, x1 - x0 + 1, color); return; }
        if (x0 == x1) { if (y0 > y1) std::swap(y0, y1); draw_vline(x0, y0, y1 - y0 + 1, color); return; }

        int16_t dx = std::abs(x1 - x0);
        int16_t sx = x0 < x1 ? 1 : -1;
        int16_t dy = -std::abs(y1 - y0);
        int16_t sy = y0 < y1 ? 1 : -1;
        int16_t err = dx + dy, e2;

        while (true) {
            draw_pixel(x0, y0, color);
            if (x0 == x1 && y0 == y1) break;
            e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    void fill_ellipse(int16_t x0, int16_t y0, int16_t rx, int16_t ry, uint32_t color) {
        if (rx <= 0 || ry <= 0) return;
        int32_t x = 0, y = ry;
        int32_t rx2 = rx * rx, ry2 = ry * ry;
        int32_t err = ry2 - (2 * ry - 1) * rx2, e2;

        do {
            draw_hline(x0 - x, y0 - y, 2 * x + 1, color);
            if (y != 0) draw_hline(x0 - x, y0 + y, 2 * x + 1, color);
            e2 = 2 * err;
            if (e2 < (2 * x + 1) * ry2) { x++; err += (2 * x + 1) * ry2; }
            if (e2 > -(2 * y - 1) * rx2) { y--; err -= (2 * y - 1) * rx2; }
        } while (y >= 0);
    }

    void draw_ellipse(int16_t x0, int16_t y0, int16_t rx, int16_t ry, uint32_t color, int16_t thickness = 1) {
        if (rx <= 0 || ry <= 0 || thickness == 0) return;
        if (thickness >= rx || thickness >= ry) { fill_ellipse(x0, y0, rx, ry, color); return; }

        int16_t x_out[ry + 1], x_in[ry + 1];
        for (int16_t i = 0; i <= ry; i++) { x_out[i] = 0; x_in[i] = 0; }

        auto compute_bounds = [](int16_t a, int16_t b, int16_t* bounds) {
            if (a <= 0 || b <= 0) return;
            int32_t x = 0, y = b, a2 = a * a, b2 = b * b;
            int32_t err = b2 - (2 * b - 1) * a2, e2;
            do {
                bounds[y] = x;
                e2 = 2 * err;
                if (e2 < (2 * x + 1) * b2) { x++; err += (2 * x + 1) * b2; }
                if (e2 > -(2 * y - 1) * a2) { y--; err -= (2 * y - 1) * a2; }
            } while (y >= 0);
        };

        compute_bounds(rx, ry, x_out);
        compute_bounds(rx - thickness, ry - thickness, x_in);
        int16_t in_ry = ry - thickness;

        for (int16_t y = 0; y <= ry; y++) {
            if (y > in_ry) {
                int16_t w = 2 * x_out[y] + 1;
                int16_t start_x = x0 - x_out[y];
                draw_hline(start_x, y0 - y, w, color, 1);
                if (y != 0) draw_hline(start_x, y0 + y, w, color, 1);
            } else {
                int16_t out_x = x_out[y], in_x = x_in[y];
                int16_t w = out_x - in_x;
                if (w > 0) {
                    draw_hline(x0 - out_x, y0 - y, w, color, 1);
                    draw_hline(x0 + in_x + 1, y0 - y, w, color, 1);
                    if (y != 0) {
                        draw_hline(x0 - out_x, y0 + y, w, color, 1);
                        draw_hline(x0 + in_x + 1, y0 + y, w, color, 1);
                    }
                }
            }
        }
    }

    // *** TEXT AREA *** //

    void set_default_font(const Font* font) {
        default_font = font;
    }
    
    // Перегрузка на случай, если удобнее передавать шрифт по ссылке: tft.set_default_font(my_font);
    void set_default_font(const Font& font) {
        default_font = &font;
    }

    void draw_char(int16_t x, int16_t y, char c, uint32_t color, const Font& font) {
        if (c < font.firstChar || c > font.lastChar) return;
        const Glyph& glyph = font.glyphs[c - font.firstChar];
        if (glyph.width == 0 || glyph.height == 0) return;

        const uint8_t* bitmap = font.bitmap + glyph.bitmapOffset;
        int16_t x0 = x + glyph.xOffset;
        int16_t y0 = y + glyph.yOffset;
        
        uint8_t gl_height = glyph.height;
        uint8_t gl_width = glyph.width;

        uint16_t bit_idx = 0;
        for (uint8_t h = 0; h < gl_height; h++) {
            for (uint8_t w = 0; w < gl_width; w++) {
                uint8_t byte = bitmap[bit_idx / 8];

                if ((byte >> (7 - (bit_idx % 8))) & 0x01) {
                    draw_pixel(x0 + w, y0 + h, color);
                }
                bit_idx++;
            }
        }
    }

    void draw_text(const char* str, int16_t x, int16_t y, uint32_t color, const Font* font = nullptr) {
        // Выбираем переданный шрифт или шрифт по умолчанию
        const Font* active_font = (font != nullptr) ? font : default_font;
        
        // Защита от падения (nullptr dereference), если ни один шрифт не задан
        if (active_font == nullptr || str == nullptr) return;

        int16_t cursor_x = x, cursor_y = y;
        uint8_t f_char = active_font->firstChar;
        uint8_t l_char = active_font->lastChar;
        uint8_t glyph_height = active_font->size + 2;

        while (*str) {
            char c = *str++;
            if (c < f_char || c > l_char) continue;
            uint8_t glyph_width = active_font->glyphs[c - f_char].xAdvance;
            
            if (cursor_x + glyph_width > screen_width) { 
                cursor_x = x; 
                cursor_y += glyph_height; 
            }
            if (cursor_y + glyph_height > screen_height + 4) {
                break;
            }

            // Разыменовываем указатель (*active_font), так как draw_char ждёт ссылку
            draw_char(cursor_x, cursor_y, c, color, *active_font);
            
            if (c >= f_char && c <= l_char) {
                cursor_x += glyph_width;
            }
        }
    }

    void draw_text(const char* str, int16_t x, int16_t y, uint32_t color, const Font& font) {
        draw_text(str, x, y, color, &font);
    }
};