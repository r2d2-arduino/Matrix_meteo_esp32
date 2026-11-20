#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "hub75/hub75_dma.h"
#include "fonts/tahomabd_10pt.h"
#include "fonts/LibreBodoni_Bold_20pt.h"
#include "fonts/icons16.h"

#include "BrokerClient.h"

static const char* TAG = "APP";

static constexpr uint32_t SCREEN_STEP_MS        = 4000;
static constexpr uint32_t RECONNECT_INTERVAL_MS = 30000; // Интервал повтора при ошибке (30 сек)

static constexpr Hub75Pins PINS = {
    .r1 = 25, .g1 = 26, .b1 = 27,
    .r2 = 21, .g2 = 22, .b2 = 23,
    .a  = 12, .b  = 16, .c  = 17, .d = 18, .e = -1,
    .clk = 15, .lat = 19, .oe = 5
};

enum class FetchStatus {
    OK,
    WIFI_ERROR,
    SERVER_ERROR,
    DATA_ERROR
};

struct BrokerData {
    FetchStatus status    = FetchStatus::WIFI_ERROR;
    std::string temp      = "?";
    std::string hum       = "?";
    std::string co2       = "?";
    std::string pres      = "?";
    std::string tempOut   = "?";

    std::string sunrise   = "?";
    std::string sunset    = "?";

    std::string date      = "?";
    long long   timestamp = 0;

    bool isOk() const { return status == FetchStatus::OK; }
};

// Безопасный парсинг целого/дробного числа без исключений C++
static bool tryParseFloat(const std::string& str, float& out_val) {
    if (str.empty() || str == "?") return false;
    char* end = nullptr;
    errno = 0;
    float val = std::strtof(str.c_str(), &end);
    if (errno != 0 || end == str.c_str()) return false;
    out_val = val;
    return true;
}

static bool tryParseInt(const std::string& str, int& out_val) {
    float fval = 0.0f;
    if (!tryParseFloat(str, fval)) return false;
    out_val = static_cast<int>(fval);
    return true;
}

BrokerData getBrokdata(BrokerClient& client) {
    BrokerData broda;

    if (!client.connect(10000)) {
        ESP_LOGE(TAG, "WiFi connect failed");
        broda.status = FetchStatus::WIFI_ERROR;
        return broda;
    }

    ESP_LOGI(TAG, "WiFi OK, my IP=%s, server IP=%s",
             client.currentIP().c_str(),
             client.serverIP().c_str());

    char c_reply[2048];
    // 3 попытки отправки запроса перед переходом в режим ожидания 30 сек
    int res = client.sendMsg("ret", c_reply, sizeof(c_reply), 3);

    if (res <= 0) {
        ESP_LOGE(TAG, "No response from broker server");
        client.disconnect(); // Сбрасываем соединение для чистого реконнекта через 30 сек
        broda.status = FetchStatus::SERVER_ERROR;
        return broda;
    }

    if (static_cast<size_t>(res) >= sizeof(c_reply) - 1) {
        ESP_LOGW(TAG, "Broker response may be truncated (%d bytes)", res);
    }

    ESP_LOGD(TAG, "Response: %s", c_reply);

    int valid_fields = 0;

    SensorVal temp = BrokerClient::getSensorValue(c_reply, "temp", "meteo");
    if (temp.success) {
        broda.temp = temp.value;
        valid_fields++;
    }

    SensorVal hum = BrokerClient::getSensorValue(c_reply, "hum", "meteo");
    if (hum.success) {
        broda.hum = hum.value;
        valid_fields++;
    }

    SensorVal pres = BrokerClient::getSensorValue(c_reply, "pres", "meteo");
    if (pres.success) {
        broda.pres = pres.value;
        valid_fields++;
    }

    SensorVal co2 = BrokerClient::getSensorValue(c_reply, "co2", "meteo");
    if (co2.success) {
        broda.co2 = co2.value;
        valid_fields++;
    }

    SensorVal tempOut = BrokerClient::getSensorValue(c_reply, "temp", "ds18b20");
    if (tempOut.success) {
        broda.tempOut = tempOut.value;
        valid_fields++;
    }

    SensorVal sunrise = BrokerClient::getSensorValue(c_reply, "sunrise", "broker");
    if (sunrise.success) {
        broda.sunrise = sunrise.value;
        valid_fields++;
    }

    SensorVal sunset = BrokerClient::getSensorValue(c_reply, "sunset", "broker");
    if (sunset.success) {
        broda.sunset = sunset.value;
        valid_fields++;
    }

    SensorVal date = BrokerClient::getSensorValue(c_reply, "date", "broker");
    if (date.success) {
        broda.date = date.value;
        broda.timestamp = date.timestamp;
        valid_fields++;
    }

    // Если JSON битый или ни одно поле не прочиталось — считаем это ошибкой данных
    if (valid_fields == 0) {
        ESP_LOGE(TAG, "Invalid or empty JSON payload: %s", c_reply);
        broda.status = FetchStatus::DATA_ERROR;
        return broda;
    }

    broda.status = FetchStatus::OK;
    return broda;
}

void screenStart(Hub75& rgb) {
    auto C = [](Colors c) { return GFX_FB<16>::get_color(c); };

    rgb.fill_screen(C(Colors::BLACK));
    rgb.draw_text("Starting...", 0, 0, C(Colors::RED));

    rgb.draw_char( 4, 12, (char)Icon16::SUN,     C(Colors::GREEN), icons16);
    rgb.draw_char(24, 12, (char)Icon16::SNOWMAN, C(Colors::CYAN),  icons16);
    rgb.draw_char(44, 12, (char)Icon16::RAIN,    C(Colors::BLUE),  icons16);

    rgb.show();
}

void screenFail(Hub75& rgb, FetchStatus status) {
    auto C = [](Colors c) { return GFX_FB<16>::get_color(c); };

    const char* errText = "Reloading...";
    switch (status) {
        case FetchStatus::WIFI_ERROR:   errText = "WiFi Err..."; break;
        case FetchStatus::SERVER_ERROR: errText = "No Server";   break;
        case FetchStatus::DATA_ERROR:   errText = "Data Err";    break;
        default: break;
    }

    rgb.fill_screen(C(Colors::BLACK));
    rgb.draw_text(errText, 0, 0, C(Colors::RED));
    rgb.draw_text("Wait 30s", 0, 18, C(Colors::CYAN));
    rgb.draw_char(46, 12, (char)Icon16::ALERT, C(Colors::YELLOW), icons16);

    rgb.show();
}

void screenMeteo(Hub75& rgb, const BrokerData& BData) {
    auto C = [](Colors c) { return GFX_FB<16>::get_color(c); };

    constexpr int16_t row0 = 0, row1 = 8, row2 = 16, row3 = 24;
    constexpr int16_t col0 = 0, col1 = 24, col2 = 47;

    rgb.fill_screen(C(Colors::BLACK));

    rgb.draw_text("Tem",  col0, row0, C(Colors::GREEN));
    rgb.draw_text("Hum",  col0, row1, C(Colors::RED));
    rgb.draw_text("Pres", col0, row2, C(Colors::BLUE));
    rgb.draw_text("CO2",  col0, row3, C(Colors::CYAN));

    rgb.draw_text(BData.temp.c_str(), col1, row0, C(Colors::YELLOW));
    rgb.draw_text(BData.hum.c_str(),  col1, row1, C(Colors::BLUE));
    rgb.draw_text(BData.pres.c_str(), col1, row2, C(Colors::MAGENTA));
    rgb.draw_text(BData.co2.c_str(),  col1, row3, C(Colors::WHITE));

    Icon16 meteoIcon   = Icon16::ALERT;
    Icon16 weatherIcon = Icon16::ALERT;

    int co2  = 0;
    int hum  = 0;
    int pres = 0;
    float temp = -99.0f;

    bool has_co2  = tryParseInt(BData.co2, co2);
    bool has_hum  = tryParseInt(BData.hum, hum);
    bool has_pres = tryParseInt(BData.pres, pres);
    bool has_temp = tryParseFloat(BData.temp, temp);

    if (has_co2 && co2 > 1200) {
        meteoIcon = Icon16::WINDOW;
    } else if (has_hum && hum > 0 && hum < 35) {
        meteoIcon = Icon16::FONTAIN;
    } else if (has_hum && hum > 75) {
        meteoIcon = Icon16::DROP;
    } else if (has_temp && temp < 18.0f) {
        meteoIcon = Icon16::SNOWMAN;
    } else if (has_temp || has_hum || has_co2) {
        meteoIcon = Icon16::TERMOMETER;
    }

    if (has_pres && pres > 0) {
        if (pres > 756) {
            weatherIcon = Icon16::SUN;
        } else if (pres > 748) {
            weatherIcon = Icon16::SUNCLOUD;
        } else if (pres > 740) {
            weatherIcon = Icon16::CLOUD;
        } else if (pres > 732) {
            weatherIcon = Icon16::RAIN;
        } else {
            weatherIcon = Icon16::RAINLIGHT;
        }
    }

    rgb.draw_char(col2, row0, (char)weatherIcon, C(Colors::CYAN),    icons16);
    rgb.draw_char(col2, row2, (char)meteoIcon,   C(Colors::MAGENTA), icons16);

    rgb.show();
}

void screenDate(Hub75& rgb, const BrokerData& BData) {
    auto C = [](Colors c) { return GFX_FB<16>::get_color(c); };

    constexpr int16_t row0 = -1, row1 = 7, row2 = 23;
    constexpr int16_t col0 = 0, col1 = 40;

    static const char* daysOfWeek[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
    };

    char timeStr[16] = "--:--";
    char dateStr[16] = "--.--";
    const char* dayName = "---";

    if (BData.timestamp > 0) {
        time_t ts = static_cast<time_t>(BData.timestamp);
        struct tm timeinfo{};
        if (gmtime_r(&ts, &timeinfo) != nullptr) {
            int day    = timeinfo.tm_mday;
            int month  = timeinfo.tm_mon + 1; // tm_mon в диапазоне 0..11
            int hour   = timeinfo.tm_hour;
            int minute = timeinfo.tm_min;
            int wday   = timeinfo.tm_wday;

            std::snprintf(timeStr, sizeof(timeStr), "%02d:%02d", hour, minute);
            std::snprintf(dateStr, sizeof(dateStr), "%02d.%02d", day, month);

            if (wday >= 0 && wday <= 6) {
                dayName = daysOfWeek[wday];
            }
        }
    }

    rgb.fill_screen(C(Colors::BLACK));

    rgb.draw_text(dateStr,              col0, row0, C(Colors::MAGENTA));
    rgb.draw_text(BData.tempOut.c_str(), col1, row0, C(Colors::BLUE));
    rgb.draw_text(timeStr,              col0 + 1, row1, C(Colors::GREEN), LibreBodoni_Bold_20pt);
    rgb.draw_text(dayName,              col0, row2, C(Colors::YELLOW));

    rgb.show();
}

void screenSun(Hub75& rgb, const BrokerData& BData) {
    auto C = [](Colors c) { return GFX_FB<16>::get_color(c); };

    constexpr int16_t row0 = -1, row1 = 7, row2 = 23;
    constexpr int16_t col0 = 0, col1 = 32;

    char timeStr[8] = "--:--";
    if (BData.timestamp > 0) {
        time_t ts = static_cast<time_t>(BData.timestamp);
        struct tm timeinfo{};
        if (gmtime_r(&ts, &timeinfo) != nullptr) {
            std::snprintf(timeStr, sizeof(timeStr), "%02d:%02d",
                          timeinfo.tm_hour, timeinfo.tm_min);
        }
    }

    rgb.fill_screen(C(Colors::BLACK));

    rgb.draw_text("Rise",   col0, row0, C(Colors::CYAN));
    rgb.draw_text("Sunset", col0, row2, C(Colors::BLUE));

    rgb.draw_text(BData.sunrise.c_str(), col1, row0, C(Colors::YELLOW));
    rgb.draw_text(timeStr,               col0 + 1, row1, C(Colors::GREEN), LibreBodoni_Bold_20pt);
    rgb.draw_text(BData.sunset.c_str(),  col1, row2, C(Colors::RED));

    rgb.show();
}

extern "C" void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    BrokerClient client;
    BrokerData brok_data;

    Hub75Config cfg;
    cfg.pins           = PINS;
    cfg.width          = 64;
    cfg.height         = 32;
    cfg.color_depth    = 4;
    cfg.base_oe_words  = 24;
    cfg.double_buffer  = true;
    cfg.brightness     = 120;
    cfg.latch_blanking = 2;
    cfg.clk_div        = 4;
    cfg.pixel_shift    = 0;
    cfg.pre_blank      = 32;

    static Hub75 panel(cfg);
    if (!panel.begin()) {
        ESP_LOGE(TAG, "Hub75 panel init failed");
        return;
    }
    panel.set_default_font(tahomabd_10pt);

    screenStart(panel);

    while (true) {
        brok_data = getBrokdata(client);

        if (brok_data.isOk()) {
            // 4 круга × (3 экрана × 4 сек) = 48 секунд до следующего опроса
            for (uint8_t i = 0; i < 4; i++) {
                screenMeteo(panel, brok_data);
                vTaskDelay(pdMS_TO_TICKS(SCREEN_STEP_MS * 2));

                screenDate(panel, brok_data);
                vTaskDelay(pdMS_TO_TICKS(SCREEN_STEP_MS));

                screenSun(panel, brok_data);
                vTaskDelay(pdMS_TO_TICKS(SCREEN_STEP_MS));
            }
        } else {
            screenFail(panel, brok_data.status);
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_INTERVAL_MS));
        }
    }
}