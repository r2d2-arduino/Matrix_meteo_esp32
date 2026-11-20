#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <mutex>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

struct SensorVal {
    bool        success   = false;
    std::string value;
    long long   timestamp = 0;
};

class BrokerClient {
public:
    static constexpr int DEBUG_MODE = 0;

    // Коллбэк для входящих обновлений: (device, sensor, value)
    using UpdateCallback = std::function<void(const std::string& device,
                                              const std::string& sensor,
                                              const std::string& value)>;

    BrokerClient();
    ~BrokerClient();

    // Подключиться к WiFi. Возвращает true при успехе.
    bool connect(uint32_t timeout_ms = 10000);

    // Отключиться / выключить станцию.
    void disconnect();

    // Открыть TCP-сокет к серверу. Возвращает fd >= 0 при успехе, -1 — иначе.
    int getSocket();

    // Отправить одноразовый запрос с ретраями (pub / ret).
    int sendMsg(const std::string& msg,
                char* out_buf,
                size_t out_buf_size,
                int max_retries = 5);

    // Подписаться на обновления сервера в фоновой задаче FreeRTOS.
    // Примеры topic: "sub" (всё), "sub/meteo" (устройство), "sub/meteo/temp" (датчик),
    // либо просто "meteo" / "meteo/temp" (префикс "sub/" добавится автоматически).
    bool subscribe(const std::string& topic,
                   UpdateCallback callback,
                   uint32_t task_stack_size = 4096,
                   UBaseType_t task_priority = 5);

    // Остановить подписку и закрыть постоянный сокет.
    void unsubscribe();

    bool isSubscribed() const { return sub_active_.load(); }

    const std::string& currentIP() const { return current_ip_; }
    const std::string& serverIP()  const { return server_ip_;  }

    static SensorVal getSensorValue(const char* json,
                                    const char* sensor,
                                    const char* category = nullptr);

private:
    bool initWifi();
    void cleanupWifi();
    void updateIpInfo();

    static void        wifiEventHandler(void* arg,
                                        esp_event_base_t base,
                                        int32_t id,
                                        void* data);
    static const char* statusToStr(uint8_t reason);

    // Вспомогательные методы для подписки
    static void        subscribeTask(void* arg);
    static void        enableKeepalive(int sock);
    static bool        sendAll(int sock, const std::string& data);
    static std::string normalizeSubTopic(const std::string& topic);
    void               processSubLine(const std::string& line);

    esp_netif_t*                 station_netif_      = nullptr;
    EventGroupHandle_t           wifi_event_group_   = nullptr;
    esp_event_handler_instance_t wifi_any_id_inst_   = nullptr;
    esp_event_handler_instance_t ip_got_ip_inst_     = nullptr;
    bool                         wifi_driver_inited_ = false;
    bool                         initialized_        = false;

    std::string current_ip_;
    std::string server_ip_;

    // Поля для постоянной подписки
    std::atomic<bool>        sub_running_{false};
    std::atomic<bool>        sub_active_{false};
    int                      sub_sock_        = -1;
    TaskHandle_t             sub_task_handle_ = nullptr;
    std::mutex               sub_mutex_;
    std::vector<std::string> sub_topics_;
    UpdateCallback           sub_callback_;
};