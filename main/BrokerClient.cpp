#include "Config.h"
#include "BrokerClient.h"

#include <cstring>
#include <string>
#include <vector>
#include <cstdio>
#include <algorithm>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "cJSON.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"

static const char* TAG = "BrokerClient";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

BrokerClient::BrokerClient() {}

BrokerClient::~BrokerClient() {
    unsubscribe();
    disconnect();
    cleanupWifi();
}

// ---------------------------------------------------------------------------
// Инициализация WiFi (аналог network.WLAN(STATION))
// ---------------------------------------------------------------------------
bool BrokerClient::initWifi() {
    if (initialized_) return true;

    // 1. NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() != ESP_OK) return false;
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // 2. netif и системный event loop (безопасны при повторном вызове)
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        return false;
    }

    // 3. Создание STA netif (только если ещё не создан)
    if (!station_netif_) {
        station_netif_ = esp_netif_create_default_wifi_sta();
        if (!station_netif_) {
            ESP_LOGE(TAG, "Failed to create default wifi STA netif");
            cleanupWifi();
            return false;
        }
    }

    // 4. Инициализация драйвера Wi-Fi (только если ещё не инициализирован)
    if (!wifi_driver_inited_) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ret = esp_wifi_init(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
            cleanupWifi();
            return false;
        }
        wifi_driver_inited_ = true;
    }

    // 5. Группа событий FreeRTOS
    if (!wifi_event_group_) {
        wifi_event_group_ = xEventGroupCreate();
        if (!wifi_event_group_) {
            ESP_LOGE(TAG, "Failed to create event group");
            cleanupWifi();
            return false;
        }
    }

    // 6. Регистрация обработчиков событий (без дублирования при ретраях)
    if (!wifi_any_id_inst_) {
        ret = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID,
            &BrokerClient::wifiEventHandler, this, &wifi_any_id_inst_);
        if (ret != ESP_OK) {
            cleanupWifi();
            return false;
        }
    }

    if (!ip_got_ip_inst_) {
        ret = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP,
            &BrokerClient::wifiEventHandler, this, &ip_got_ip_inst_);
        if (ret != ESP_OK) {
            cleanupWifi();
            return false;
        }
    }

    // 7. Конфигурация и запуск Wi-Fi
    wifi_config_t wifi_config = {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
                 AP_SSID, sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
                 AP_PASSWORD, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure/start WiFi");
        cleanupWifi();
        return false;
    }

    esp_wifi_set_ps(WIFI_PS_NONE);
    initialized_ = true;
    return true;
}

void BrokerClient::cleanupWifi() {
    if (wifi_any_id_inst_) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_any_id_inst_);
        wifi_any_id_inst_ = nullptr;
    }
    if (ip_got_ip_inst_) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_got_ip_inst_);
        ip_got_ip_inst_ = nullptr;
    }
    if (wifi_driver_inited_) {
        esp_wifi_stop();
        esp_wifi_deinit();
        wifi_driver_inited_ = false;
    }
    if (station_netif_) {
        esp_netif_destroy_default_wifi(station_netif_);
        station_netif_ = nullptr;
    }
    if (wifi_event_group_) {
        vEventGroupDelete(wifi_event_group_);
        wifi_event_group_ = nullptr;
    }
    initialized_ = false;
}

// ---------------------------------------------------------------------------
// Обработчик событий WiFi/IP
// ---------------------------------------------------------------------------
void BrokerClient::wifiEventHandler(void* arg,
                                    esp_event_base_t base,
                                    int32_t id,
                                    void* data) {
    auto* self = static_cast<BrokerClient*>(arg);

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* event = static_cast<wifi_event_sta_disconnected_t*>(data);
        if (DEBUG_MODE) {
            ESP_LOGW(TAG, "[WiFi] Disconnected, reason=%d (%s)",
                     event->reason, statusToStr(event->reason));
        }
        xEventGroupSetBits(self->wifi_event_group_, WIFI_FAIL_BIT);
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        if (DEBUG_MODE) {
            ESP_LOGI(TAG, "[WiFi] Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        }
        xEventGroupSetBits(self->wifi_event_group_, WIFI_CONNECTED_BIT);
    }
}

// ---------------------------------------------------------------------------
// Обновление сохранённых IP (currentIP / serverIP)
// ---------------------------------------------------------------------------
void BrokerClient::updateIpInfo() {
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(station_netif_, &ip_info) != ESP_OK) return;

    char buf[16];

    ip4addr_ntoa_r(reinterpret_cast<ip4_addr_t*>(&ip_info.ip), buf, sizeof(buf));
    current_ip_ = buf;

    // В MicroPython: serverIP = ifconfig()[2] (шлюз).
    // Устанавливаем один раз, как в оригинале: if not self.serverIP.
    if (server_ip_.empty()) {
        ip4addr_ntoa_r(reinterpret_cast<ip4_addr_t*>(&ip_info.gw), buf, sizeof(buf));
        server_ip_ = buf;
    }
}

// ---------------------------------------------------------------------------
// Подключение к AP
// ---------------------------------------------------------------------------
bool BrokerClient::connect(uint32_t timeout_ms) {
    if (!initWifi()) return false;

    // Уже подключены и есть IP?
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(station_netif_, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
        if (DEBUG_MODE) ESP_LOGI(TAG, "[WiFi] Already connected: " IPSTR,
                                 IP2STR(&ip_info.ip));
        updateIpInfo();
        return true;
    }

    xEventGroupClearBits(wifi_event_group_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    if (DEBUG_MODE) ESP_LOGI(TAG, "[WiFi] Connecting to network...");

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        // ESP_ERR_WIFI_CONN означает, что уже идёт подключение — это нормально.
        if (DEBUG_MODE) ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group_,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        updateIpInfo();
        return true;
    }

    if (DEBUG_MODE) ESP_LOGW(TAG, "[WiFi] Connection failed");
    disconnect();
    return false;
}

// ---------------------------------------------------------------------------
// Отключение
// ---------------------------------------------------------------------------
void BrokerClient::disconnect() {
    if (initialized_) {
        esp_wifi_disconnect();
    }
    if (DEBUG_MODE) ESP_LOGI(TAG, "[WiFi] Disconnected");
}

// ---------------------------------------------------------------------------
// Открытие TCP-сокета к серверу
// ---------------------------------------------------------------------------
int BrokerClient::getSocket() {
    if (server_ip_.empty()) {
        if (DEBUG_MODE) ESP_LOGE(TAG, "[Socket] server IP unknown");
        return -1;
    }

    int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        if (DEBUG_MODE) ESP_LOGE(TAG, "[Socket] socket() errno=%d", errno);
        return -1;
    }

    sockaddr_in dest = {};
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(SOCKET_PORT);
    dest.sin_addr.s_addr = inet_addr(server_ip_.c_str());

    if (::connect(sock, reinterpret_cast<sockaddr*>(&dest), sizeof(dest)) != 0) {
        if (DEBUG_MODE) ESP_LOGE(TAG, "[Socket] connect(%s:%d) errno=%d",
                                 server_ip_.c_str(), SOCKET_PORT, errno);
        ::close(sock);
        return -1;
    }
    return sock;
}

// ---------------------------------------------------------------------------
// Отправка сообщения с ретраями (аналог sendMsg)
// ---------------------------------------------------------------------------
// Исправленный sendMsg с гарантированной отправкой и чтением всех TCP-пакетов до закрытия сокета сервером:
int BrokerClient::sendMsg(const std::string& msg,
                          char* out_buf,
                          size_t out_buf_size,
                          int max_retries) {
    if (!out_buf || out_buf_size == 0) return -1;
    out_buf[0] = '\0';

    int attempts = 0;
    while (attempts < max_retries) {
        attempts++;

        if (!connect()) {
            disconnect();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int sock = getSocket();
        if (sock < 0) {
            disconnect();
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        // 1. Отправка всего запроса
        size_t total_sent = 0;
        bool send_ok = true;
        while (total_sent < msg.size()) {
            ssize_t sent = ::send(sock, msg.data() + total_sent, msg.size() - total_sent, 0);
            if (sent <= 0) {
                send_ok = false;
                break;
            }
            total_sent += sent;
        }

        // 2. Чтение всего ответа до EOF (recvd == 0), так как сервер закрывает сокет после ответа
        int result = -1;
        if (send_ok) {
            size_t total_recvd = 0;
            while (total_recvd < out_buf_size - 1) {
                int recvd = ::recv(sock,
                                   out_buf + total_recvd,
                                   (out_buf_size - 1) - total_recvd,
                                   0);
                if (recvd > 0) {
                    total_recvd += recvd;
                } else if (recvd == 0) {
                    // Сервер штатно закрыл соединение — ответ получен полностью
                    out_buf[total_recvd] = '\0';
                    result = static_cast<int>(total_recvd);
                    break;
                } else {
                    // Таймаут или ошибка сокета
                    result = -1;
                    break;
                }
            }
            // Если буфер заполнился целиком, завершаем строку нулём
            if (total_recvd == out_buf_size - 1) {
                out_buf[total_recvd] = '\0';
                result = static_cast<int>(total_recvd);
            }
        }

        ::close(sock);
        if (result >= 0) return result;
    }
    return -1;
}


// ---------------------------------------------------------------------------
// Подписка на обновления сервера (постоянное TCP-соединение)
// ---------------------------------------------------------------------------
bool BrokerClient::sendAll(int sock, const std::string& data) {
    size_t total_sent = 0;
    while (total_sent < data.size()) {
        ssize_t sent = ::send(sock, data.data() + total_sent, data.size() - total_sent, 0);
        if (sent <= 0) return false;
        total_sent += sent;
    }
    return true;
}

void BrokerClient::enableKeepalive(int sock) {
    int keepalive = 1;
    int keepidle  = 15; // Начать проверку через 15 сек тишины
    int keepintvl = 5;  // Интервал между пакетами проверки — 5 сек
    int keepcnt   = 3;  // Закрыть сокет после 3 неудачных проверок (~30 сек)

    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
    setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    // Ожидаем события от сервера бессрочно (пока жив keepalive)
    struct timeval tv_inf = { .tv_sec = 0, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv_inf, sizeof(tv_inf));

    struct timeval tv_snd = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv_snd, sizeof(tv_snd));
}

std::string BrokerClient::normalizeSubTopic(const std::string& topic) {
    std::string clean = topic;
    while (!clean.empty() && (clean.front() == '/' || clean.front() == ' ')) {
        clean.erase(clean.begin());
    }
    while (!clean.empty() && (clean.back() == '/' || clean.back() == ' ' || clean.back() == '\r' || clean.back() == '\n')) {
        clean.pop_back();
    }

    if (clean.empty() || clean == "*" || clean == "*/*") {
        return "sub";
    }
    if (clean == "sub" || clean.rfind("sub/", 0) == 0) {
        return clean;
    }
    return "sub/" + clean;
}

bool BrokerClient::subscribe(const std::string& topic,
                             UpdateCallback callback,
                             uint32_t task_stack_size,
                             UBaseType_t task_priority) {
    std::string norm_topic = normalizeSubTopic(topic);

    {
        std::lock_guard<std::mutex> lock(sub_mutex_);
        if (callback) {
            sub_callback_ = std::move(callback);
        }
        if (std::find(sub_topics_.begin(), sub_topics_.end(), norm_topic) == sub_topics_.end()) {
            sub_topics_.push_back(norm_topic);
        }

        // Если соединение уже активно, сразу досылаем новую подписку в открытый сокет
        if (sub_sock_ >= 0) {
            sendAll(sub_sock_, norm_topic + "\n");
        }
    }

    if (sub_running_.load()) {
        return true;
    }

    sub_running_ = true;
    BaseType_t res = xTaskCreate(
        &BrokerClient::subscribeTask,
        "broker_sub",
        task_stack_size,
        this,
        task_priority,
        &sub_task_handle_);

    if (res != pdPASS) {
        sub_running_ = false;
        sub_task_handle_ = nullptr;
        ESP_LOGE(TAG, "[Sub] Failed to create subscribe task");
        return false;
    }

    return true;
}

void BrokerClient::unsubscribe() {
    sub_running_ = false;
    sub_active_  = false;

    // Закрываем сокет, чтобы немедленно разблокировать recv() в фоновой задаче
    {
        std::lock_guard<std::mutex> lock(sub_mutex_);
        if (sub_sock_ >= 0) {
            ::shutdown(sub_sock_, SHUT_RDWR);
            ::close(sub_sock_);
            sub_sock_ = -1;
        }
        sub_topics_.clear();
    }

    // Ждём штатного завершения задачи (до 1 сек)
    for (int i = 0; i < 20 && sub_task_handle_ != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void BrokerClient::processSubLine(const std::string& line) {
    if (line == "sub:ok") {
        sub_active_ = true;
        if (DEBUG_MODE) ESP_LOGI(TAG, "[Sub] Subscription confirmed");
        return;
    }

    // Ожидаемый формат от сервера: upd/<device>/<sensor>/<value>
    if (line.rfind("upd/", 0) == 0) {
        size_t pos1 = line.find('/', 4);
        if (pos1 == std::string::npos) return;

        size_t pos2 = line.find('/', pos1 + 1);
        if (pos2 == std::string::npos) return;

        std::string device = line.substr(4, pos1 - 4);
        std::string sensor = line.substr(pos1 + 1, pos2 - (pos1 + 1));
        std::string value  = line.substr(pos2 + 1); // Значение может содержать '/', берём всё до конца

        UpdateCallback cb;
        {
            std::lock_guard<std::mutex> lock(sub_mutex_);
            cb = sub_callback_;
        }
        if (cb) {
            cb(device, sensor, value);
        }
    }
}

void BrokerClient::subscribeTask(void* arg) {
    auto* self = static_cast<BrokerClient*>(arg);
    std::string rx_accum;
    char buf[256];

    while (self->sub_running_.load()) {
        if (!self->connect()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        int sock = self->getSocket();
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        enableKeepalive(sock);

        // Формируем пакет со всеми активными топиками подписки
        std::string sub_payload;
        {
            std::lock_guard<std::mutex> lock(self->sub_mutex_);
            self->sub_sock_ = sock;
            for (const auto& t : self->sub_topics_) {
                sub_payload += t + "\n";
            }
        }

        if (!sub_payload.empty() && !sendAll(sock, sub_payload)) {
            std::lock_guard<std::mutex> lock(self->sub_mutex_);
            if (self->sub_sock_ == sock) {
                ::close(sock);
                self->sub_sock_ = -1;
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        rx_accum.clear();

        // Цикл чтения входящих строк до обрыва связи или вызова unsubscribe()
        while (self->sub_running_.load()) {
            ssize_t r = ::recv(sock, buf, sizeof(buf) - 1, 0);
            if (r <= 0) {
                if (self->sub_running_.load() && DEBUG_MODE) {
                    ESP_LOGW(TAG, "[Sub] Connection lost (r=%d, errno=%d), reconnecting...",
                             static_cast<int>(r), errno);
                }
                break;
            }

            rx_accum.append(buf, r);

            size_t pos;
            while ((pos = rx_accum.find('\n')) != std::string::npos) {
                std::string line = rx_accum.substr(0, pos);
                rx_accum.erase(0, pos + 1);

                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                    line.pop_back();
                }
                if (!line.empty()) {
                    self->processSubLine(line);
                }
            }
        }

        self->sub_active_ = false;
        {
            std::lock_guard<std::mutex> lock(self->sub_mutex_);
            if (self->sub_sock_ == sock) {
                ::close(sock);
                self->sub_sock_ = -1;
            }
        }

        if (self->sub_running_.load()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    self->sub_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Маппинг reason-кодов в текст (аналог _status_to_str)
// ---------------------------------------------------------------------------
const char* BrokerClient::statusToStr(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:       return "AUTH EXPIRE";
        case WIFI_REASON_AUTH_FAIL:         return "WRONG PASSWORD / AUTH FAIL";
        case WIFI_REASON_NO_AP_FOUND:       return "NETWORK NOT FOUND";
        case WIFI_REASON_ASSOC_FAIL:        return "ASSOC FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE TIMEOUT";
        case WIFI_REASON_BEACON_TIMEOUT:    return "WIFI BEACON TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL:   return "CONNECT FAIL";
        default:                            return "UNKNOWN";
    }
}

SensorVal BrokerClient::getSensorValue(const char* json,
                                       const char* sensor,
                                       const char* category) {
    SensorVal result;
    if (!json || !sensor || sensor[0] == '\0') {
        return result;
    }

    cJSON* root = cJSON_Parse(json);
    if (!root) {
        return result;
    }

    // Если категория передана и не пустая — ищем вложенный объект, иначе берём корень
    const cJSON* target_obj = root;
    if (category && category[0] != '\0') {
        target_obj = cJSON_GetObjectItemCaseSensitive(root, category);
    }

    if (cJSON_IsObject(target_obj)) {
        const cJSON* arr = cJSON_GetObjectItemCaseSensitive(target_obj, sensor);
        if (cJSON_IsArray(arr) && cJSON_GetArraySize(arr) >= 2) {
            const cJSON* val = cJSON_GetArrayItem(arr, 0);
            const cJSON* ts  = cJSON_GetArrayItem(arr, 1);

            if (cJSON_IsString(val) && cJSON_IsNumber(ts)) {
                result.value     = val->valuestring;
                result.timestamp = static_cast<long long>(ts->valuedouble);
                result.success   = true;
            }
        }
    }

    cJSON_Delete(root);
    return result;
}