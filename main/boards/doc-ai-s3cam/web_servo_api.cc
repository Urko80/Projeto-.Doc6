/**
 * DOC INÍCIO — API HTTP e controle PCA9685
 *
 * Endpoints:
 *   GET  /api/status       Estado atual e limites conhecidos
 *   POST /api/servo        {"channel":0|3,"pulse_us":1250..1750}
 *   POST /api/legs         {"left_us":1250..1750,"right_us":1250..1750}
 *   POST /api/wheels       {"left":"cw|ccw|stop","right":"cw|ccw|stop"}
 *   POST /api/stop         Para ambos os motores de roda
 *
 * Segurança e validação:
 * - Os pulsos são validados no firmware, não apenas na interface.
 * - CH1/CH2 são MG90S contínuos: CW=1665 us, stop=1705 us, CCW=1800 us (valor de teste ainda não validado).
 * - CH0/CH3 são SG90: faixa inicial de 1250 a 1750 us; limites mecânicos finais
 *   dependem da montagem.
 * - CH1/CH2: CW=1665 us, neutro=1705 us e CCW=1800 us foram confirmados nos
 *   testes físicos desta montagem.
 * - O watchdog tenta parar as rodas após 1,5 s sem novo comando; não é uma parada
 *   de emergência certificada.
 * - A API não possui autenticação e permite CORS amplo: use apenas em rede
 *   confiável e não exponha a porta 80 à internet.
 *
 * Limitações:
 * - Não salva calibração em NVS.
 * - A interface chama os endpoints abaixo; velocidade variável não implementada.
 * - Confirme o build do commit atual antes de integrar o PR.
 * DOC FIM
 */

#include "web_servo_api.h"
#include "config.h"

#include <algorithm>
#include <cstring>
#include <cstdint>

// DOC INÍCIO — símbolos gerados pelo ESP-IDF ao embutir web_control.html
// O CMake registra o arquivo boards/doc-ai-s3cam/web_control.html como TXT.
// O ESP-IDF nomeia os símbolos pelo nome-base do arquivo embutido.
extern const uint8_t _binary_web_control_html_start[];
extern const uint8_t _binary_web_control_html_end[];
// DOC FIM — interface HTML dentro da flash do firmware
#include <string>

#include <cJSON.h>
#include <driver/i2c_master.h>
#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace {

static const char* TAG = "doc_web_servo_api";

// DOC INÍCIO — hardware e limites verificados no Projeto .DOC
constexpr gpio_num_t PCA_SDA_GPIO = PCA9685_SDA_GPIO;
constexpr gpio_num_t PCA_SCL_GPIO = PCA9685_SCL_GPIO;
constexpr uint8_t PCA9685_ADDRESS = PCA9685_I2C_ADDRESS;
constexpr uint32_t I2C_FREQUENCY_HZ = PCA9685_I2C_FREQUENCY_HZ;
constexpr uint16_t SERVO_MIN_US = DOC_LEG_SERVO_MIN_US;
constexpr uint16_t SERVO_MAX_US = DOC_LEG_SERVO_MAX_US;
constexpr uint16_t WHEEL_LEFT_CW_US = DOC_WHEEL_LEFT_CW_US;
constexpr uint16_t WHEEL_LEFT_STOP_US = DOC_WHEEL_LEFT_STOP_US;
constexpr uint16_t WHEEL_LEFT_CCW_US = DOC_WHEEL_LEFT_CCW_US;
constexpr uint16_t WHEEL_RIGHT_CW_US = DOC_WHEEL_RIGHT_CW_US;
constexpr uint16_t WHEEL_RIGHT_STOP_US = DOC_WHEEL_RIGHT_STOP_US;
constexpr uint16_t WHEEL_RIGHT_CCW_US = DOC_WHEEL_RIGHT_CCW_US;
constexpr uint32_t WHEEL_WATCHDOG_MS = 1500;
// DOC FIM

constexpr uint8_t REG_MODE1 = 0x00;
constexpr uint8_t REG_PRESCALE = 0xFE;
constexpr uint8_t REG_LED0_ON_L = 0x06;
constexpr uint8_t MODE1_RESTART = 0x80;
constexpr uint8_t MODE1_AI = 0x20;
constexpr uint8_t MODE1_SLEEP = 0x10;

static i2c_master_bus_handle_t s_i2c_bus = nullptr;
// DOC — barramento fornecido pela placa (VL53L0X + PCA9685) e se este módulo o criou.
static i2c_master_bus_handle_t s_shared_bus = nullptr;
static bool s_i2c_bus_owned = false;
static i2c_master_dev_handle_t s_pca = nullptr;
static httpd_handle_t s_http = nullptr;
static SemaphoreHandle_t s_mutex = nullptr;
static TaskHandle_t s_watchdog_task = nullptr;
static bool s_started = false;
// Os pulsos das pernas começam como referências, mas os canais ficam OFF.
static uint16_t s_leg_left_us = 1500;
static uint16_t s_leg_right_us = 1500;
static bool s_leg_left_enabled = false;
static bool s_leg_right_enabled = false;
static uint16_t s_wheel_left_us = WHEEL_LEFT_STOP_US;
static uint16_t s_wheel_right_us = WHEEL_RIGHT_STOP_US;
static TickType_t s_last_wheel_command = 0;

static esp_err_t PcaWrite(uint8_t reg, uint8_t value) {
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_pca, data, sizeof(data), 100);
}

static esp_err_t PcaSetPulse(uint8_t channel, uint16_t pulse_us) {
    if (s_pca == nullptr || channel > 15 || pulse_us < 500 || pulse_us > 2500) {
        return ESP_ERR_INVALID_ARG;
    }

    // PCA9685 em 50 Hz: 20 ms por ciclo, 4096 passos por ciclo.
    const uint16_t ticks = static_cast<uint16_t>(
        (static_cast<uint32_t>(pulse_us) * 4096U + 10000U) / 20000U);
    const uint8_t reg = static_cast<uint8_t>(REG_LED0_ON_L + 4U * channel);
    const uint8_t data[] = {
        reg, 0, 0,
        static_cast<uint8_t>(ticks & 0xFF),
        static_cast<uint8_t>((ticks >> 8) & 0x0F)
    };
    return i2c_master_transmit(s_pca, data, sizeof(data), 100);
}

// DOC INÍCIO — desativa a saída PWM de um canal sem mover o servo
static esp_err_t PcaSetChannelOff(uint8_t channel) {
    if (s_pca == nullptr || channel > 15) return ESP_ERR_INVALID_ARG;
    const uint8_t reg = static_cast<uint8_t>(REG_LED0_ON_L + 4U * channel);
    // Bit FULL_OFF (bit 4 de LEDn_OFF_H) desliga completamente o canal.
    const uint8_t data[] = {reg, 0, 0, 0, 0x10};
    return i2c_master_transmit(s_pca, data, sizeof(data), 100);
}
// DOC FIM

static esp_err_t PcaInitialize() {
    esp_err_t err = ESP_OK;
    if (s_shared_bus != nullptr) {
        s_i2c_bus = s_shared_bus;
        s_i2c_bus_owned = false;
    } else {
        i2c_master_bus_config_t bus_cfg = {};
        bus_cfg.i2c_port = I2C_NUM_0;
        bus_cfg.sda_io_num = PCA_SDA_GPIO;
        bus_cfg.scl_io_num = PCA_SCL_GPIO;
        bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_cfg.glitch_ignore_cnt = 7;
        bus_cfg.flags.enable_internal_pullup = true;

        err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
        if (err != ESP_OK) return err;
        s_i2c_bus_owned = true;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = PCA9685_ADDRESS;
    dev_cfg.scl_speed_hz = I2C_FREQUENCY_HZ;

    err = i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_pca);
    if (err != ESP_OK) return err;

    err = PcaWrite(REG_MODE1, MODE1_SLEEP | MODE1_AI);
    if (err != ESP_OK) return err;

    // Prescale 121 ~= 50 Hz para o oscilador nominal de 25 MHz.
    err = PcaWrite(REG_PRESCALE, 121);
    if (err != ESP_OK) return err;

    err = PcaWrite(REG_MODE1, MODE1_AI);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(5));
    err = PcaWrite(REG_MODE1, MODE1_RESTART | MODE1_AI);
    if (err != ESP_OK) return err;

    // DOC INÍCIO — segurança na inicialização
    // Mantém as pernas sem sinal PWM para evitar movimento inesperado ao conectar.
    // As rodas recebem o pulso neutro calibrado para solicitar parada.
    if ((err = PcaSetChannelOff(0)) != ESP_OK) return err;
    if ((err = PcaSetPulse(1, WHEEL_LEFT_STOP_US)) != ESP_OK) return err;
    if ((err = PcaSetPulse(2, WHEEL_RIGHT_STOP_US)) != ESP_OK) return err;
    if ((err = PcaSetChannelOff(3)) != ESP_OK) return err;
    // DOC FIM
    return ESP_OK;
}

static void SendJson(httpd_req_t* req, const char* json, const char* status = "200 OK") {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_sendstr(req, json);
}

// DOC INÍCIO — servir o painel embutido na raiz do endereço IP
// GET / e GET /index.html devolvem a mesma interface HTML responsiva.
static esp_err_t WebUiHandler(httpd_req_t* req) {
    const size_t html_size = static_cast<size_t>(
        _binary_web_control_html_end -
        _binary_web_control_html_start);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(
        req,
        reinterpret_cast<const char*>(_binary_web_control_html_start),
        html_size);
}
// DOC FIM — servir painel web

static esp_err_t OptionsHandler(httpd_req_t* req) {
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

static bool ReadBody(httpd_req_t* req, char* buffer, size_t capacity) {
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) >= capacity) {
        return false;
    }
    int total = 0;
    while (total < req->content_len) {
        int n = httpd_req_recv(req, buffer + total, req->content_len - total);
        if (n <= 0) return false;
        total += n;
    }
    buffer[total] = '\0';
    return true;
}

static cJSON* ParseBody(httpd_req_t* req, char* buffer, size_t capacity) {
    if (!ReadBody(req, buffer, capacity)) return nullptr;
    return cJSON_Parse(buffer);
}

static esp_err_t StatusHandler(httpd_req_t* req) {
    char json[256];
    snprintf(json, sizeof(json),
        "{\"ok\":true,\"pca9685\":\"0x40\",\"frequency_hz\":50,"
        "\"legs\":{\"left_us\":%u,\"right_us\":%u,\"left_enabled\":%s,\"right_enabled\":%s,\"min_us\":%u,\"max_us\":%u},"
        "\"wheels\":{\"left_us\":%u,\"right_us\":%u,\"stop_us\":%u}}",
        s_leg_left_us, s_leg_right_us,
        s_leg_left_enabled ? "true" : "false", s_leg_right_enabled ? "true" : "false",
        SERVO_MIN_US, SERVO_MAX_US, s_wheel_left_us, s_wheel_right_us, WHEEL_LEFT_STOP_US);
    SendJson(req, json);
    return ESP_OK;
}

static esp_err_t ServoHandler(httpd_req_t* req) {
    char body[128];
    cJSON* root = ParseBody(req, body, sizeof(body));
    if (!root) {
        SendJson(req, "{\"ok\":false,\"error\":\"invalid_json_or_body\"}", "400 Bad Request");
        return ESP_OK;
    }

    const cJSON* channel = cJSON_GetObjectItemCaseSensitive(root, "channel");
    const cJSON* pulse = cJSON_GetObjectItemCaseSensitive(root, "pulse_us");
    if (!cJSON_IsNumber(channel) || !cJSON_IsNumber(pulse) ||
        (channel->valueint != 0 && channel->valueint != 3) ||
        pulse->valueint < SERVO_MIN_US || pulse->valueint > SERVO_MAX_US) {
        cJSON_Delete(root);
        SendJson(req, "{\"ok\":false,\"error\":\"channel_or_pulse_out_of_range\"}", "400 Bad Request");
        return ESP_OK;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        cJSON_Delete(root);
        SendJson(req, "{\"ok\":false,\"error\":\"busy\"}", "503 Service Unavailable");
        return ESP_OK;
    }
    const uint8_t ch = static_cast<uint8_t>(channel->valueint);
    const uint16_t us = static_cast<uint16_t>(pulse->valueint);
    esp_err_t err = PcaSetPulse(ch, us);
    if (err == ESP_OK) {
        if (ch == 0) { s_leg_left_us = us; s_leg_left_enabled = true; }
        else { s_leg_right_us = us; s_leg_right_enabled = true; }
    }
    xSemaphoreGive(s_mutex);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        SendJson(req, "{\"ok\":false,\"error\":\"pca_write_failed\"}", "500 Internal Server Error");
    } else {
        SendJson(req, "{\"ok\":true}");
    }
    return ESP_OK;
}

static esp_err_t LegsHandler(httpd_req_t* req) {
    char body[160];
    cJSON* root = ParseBody(req, body, sizeof(body));
    if (!root) {
        SendJson(req, "{\"ok\":false,\"error\":\"invalid_json_or_body\"}", "400 Bad Request");
        return ESP_OK;
    }

    const cJSON* left = cJSON_GetObjectItemCaseSensitive(root, "left_us");
    const cJSON* right = cJSON_GetObjectItemCaseSensitive(root, "right_us");
    if (!cJSON_IsNumber(left) || !cJSON_IsNumber(right) ||
        left->valueint < SERVO_MIN_US || left->valueint > SERVO_MAX_US ||
        right->valueint < SERVO_MIN_US || right->valueint > SERVO_MAX_US) {
        cJSON_Delete(root);
        SendJson(req, "{\"ok\":false,\"error\":\"pulse_out_of_range\"}", "400 Bad Request");
        return ESP_OK;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        cJSON_Delete(root);
        SendJson(req, "{\"ok\":false,\"error\":\"busy\"}", "503 Service Unavailable");
        return ESP_OK;
    }
    const uint16_t l = static_cast<uint16_t>(left->valueint);
    const uint16_t r = static_cast<uint16_t>(right->valueint);
    esp_err_t err = PcaSetPulse(0, l);
    if (err == ESP_OK) {
        s_leg_left_us = l;
        s_leg_left_enabled = true;
        err = PcaSetPulse(3, r);
        if (err == ESP_OK) {
            s_leg_right_us = r;
            s_leg_right_enabled = true;
        }
    }
    xSemaphoreGive(s_mutex);
    cJSON_Delete(root);

    if (err != ESP_OK) {
        SendJson(req, "{\"ok\":false,\"error\":\"pca_write_failed\"}", "500 Internal Server Error");
    } else {
        SendJson(req, "{\"ok\":true}");
    }
    return ESP_OK;
}

static esp_err_t StopWheelsLocked() {
    esp_err_t err = PcaSetPulse(1, WHEEL_LEFT_STOP_US);
    if (err == ESP_OK) err = PcaSetPulse(2, WHEEL_RIGHT_STOP_US);
    if (err == ESP_OK) {
        s_wheel_left_us = WHEEL_LEFT_STOP_US;
        s_wheel_right_us = WHEEL_RIGHT_STOP_US;
    }
    return err;
}

static esp_err_t StopHandler(httpd_req_t* req) {
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        SendJson(req, "{\"ok\":false,\"error\":\"busy\"}", "503 Service Unavailable");
        return ESP_OK;
    }
    esp_err_t err = StopWheelsLocked();
    s_last_wheel_command = xTaskGetTickCount();
    xSemaphoreGive(s_mutex);
    if (err != ESP_OK) SendJson(req, "{\"ok\":false,\"error\":\"pca_write_failed\"}", "500 Internal Server Error");
    else SendJson(req, "{\"ok\":true,\"wheels\":\"stopped\"}");
    return ESP_OK;
}

static bool ParseWheel(const cJSON* root, const char* key, bool is_left, uint16_t* pulse) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsString(value) || value->valuestring == nullptr) return false;
    if (strcmp(value->valuestring, "cw") == 0) {
        *pulse = is_left ? WHEEL_LEFT_CW_US : WHEEL_RIGHT_CW_US;
        return true;
    }
    if (strcmp(value->valuestring, "ccw") == 0) {
        *pulse = is_left ? WHEEL_LEFT_CCW_US : WHEEL_RIGHT_CCW_US;
        return true;
    }
    if (strcmp(value->valuestring, "stop") == 0) {
        *pulse = is_left ? WHEEL_LEFT_STOP_US : WHEEL_RIGHT_STOP_US;
        return true;
    }
    return false;
}

static esp_err_t WheelsHandler(httpd_req_t* req) {
    char body[160];
    cJSON* root = ParseBody(req, body, sizeof(body));
    if (!root) {
        SendJson(req, "{\"ok\":false,\"error\":\"invalid_json_or_body\"}", "400 Bad Request");
        return ESP_OK;
    }
    uint16_t left = 0, right = 0;
    if (!ParseWheel(root, "left", true, &left) || !ParseWheel(root, "right", false, &right)) {
        cJSON_Delete(root);
        SendJson(req, "{\"ok\":false,\"error\":\"wheel_direction_must_be_cw_ccw_or_stop\"}", "400 Bad Request");
        return ESP_OK;
    }

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        cJSON_Delete(root);
        SendJson(req, "{\"ok\":false,\"error\":\"busy\"}", "503 Service Unavailable");
        return ESP_OK;
    }
    esp_err_t err = PcaSetPulse(1, left);
    if (err == ESP_OK) err = PcaSetPulse(2, right);
    if (err == ESP_OK) {
        s_wheel_left_us = left;
        s_wheel_right_us = right;
        s_last_wheel_command = xTaskGetTickCount();
    } else {
        // Falha parcial: tenta parar ambos os motores antes de liberar o mutex.
        StopWheelsLocked();
    }
    xSemaphoreGive(s_mutex);
    cJSON_Delete(root);

    if (err != ESP_OK) SendJson(req, "{\"ok\":false,\"error\":\"pca_write_failed_wheels_stopped\"}", "500 Internal Server Error");
    else SendJson(req, "{\"ok\":true}");
    return ESP_OK;
}

static void WheelWatchdogTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (s_mutex == nullptr || xSemaphoreTake(s_mutex, 0) != pdTRUE) continue;
        const TickType_t now = xTaskGetTickCount();
        const TickType_t timeout = pdMS_TO_TICKS(WHEEL_WATCHDOG_MS);
        const bool moving = s_wheel_left_us != WHEEL_LEFT_STOP_US ||
                            s_wheel_right_us != WHEEL_RIGHT_STOP_US;
        if (moving && (now - s_last_wheel_command) >= timeout) {
            ESP_LOGW(TAG, "Watchdog: parando rodas após ausência de comando");
            StopWheelsLocked();
        }
        xSemaphoreGive(s_mutex);
    }
}

static esp_err_t RegisterUri(httpd_handle_t server, const char* uri, httpd_method_t method,
                             esp_err_t (*handler)(httpd_req_t*)) {
    httpd_uri_t route = {};
    route.uri = uri;
    route.method = method;
    route.handler = handler;
    route.user_ctx = nullptr;
    return httpd_register_uri_handler(server, &route);
}

} // namespace

extern "C" void DocWebServoApiSetSharedBus(i2c_master_bus_handle_t bus) {
    s_shared_bus = bus;
}

void DocWebServoApiStart(void) {
    if (s_started) return;

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == nullptr) {
        ESP_LOGE(TAG, "Falha ao criar mutex");
        return;
    }

    esp_err_t err = PcaInitialize();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao inicializar PCA9685: %s", esp_err_to_name(err));
        return;
    }

    s_last_wheel_command = xTaskGetTickCount();
    if (xTaskCreate(WheelWatchdogTask, "doc_wheel_wd", 2048, nullptr, 4, &s_watchdog_task) != pdPASS) {
        ESP_LOGE(TAG, "Falha ao criar watchdog das rodas");
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    err = httpd_start(&s_http, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao iniciar servidor HTTP: %s", esp_err_to_name(err));
        return;
    }

    // DOC INÍCIO — página inicial para acesso direto pelo celular
    RegisterUri(s_http, "/", HTTP_GET, WebUiHandler);
    RegisterUri(s_http, "/index.html", HTTP_GET, WebUiHandler);
    // DOC FIM — página inicial

    RegisterUri(s_http, "/api/status", HTTP_GET, StatusHandler);
    RegisterUri(s_http, "/api/servo", HTTP_POST, ServoHandler);
    RegisterUri(s_http, "/api/legs", HTTP_POST, LegsHandler);
    RegisterUri(s_http, "/api/wheels", HTTP_POST, WheelsHandler);
    RegisterUri(s_http, "/api/stop", HTTP_POST, StopHandler);
    RegisterUri(s_http, "/api/status", HTTP_OPTIONS, OptionsHandler);
    RegisterUri(s_http, "/api/servo", HTTP_OPTIONS, OptionsHandler);
    RegisterUri(s_http, "/api/legs", HTTP_OPTIONS, OptionsHandler);
    RegisterUri(s_http, "/api/wheels", HTTP_OPTIONS, OptionsHandler);
    RegisterUri(s_http, "/api/stop", HTTP_OPTIONS, OptionsHandler);

    s_started = true;
    ESP_LOGI(TAG, "Painel web: http://<IP-DO-DOC>/ | API: /api/status");
}

extern "C" void DocWebServoApiStop(void) {
    // Bloqueia novos comandos HTTP antes de parar os motores.
    if (s_http != nullptr) {
        httpd_stop(s_http);
        s_http = nullptr;
    }

    if (s_mutex != nullptr && s_pca != nullptr &&
        xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        StopWheelsLocked();
        xSemaphoreGive(s_mutex);
    }

    if (s_watchdog_task != nullptr) {
        vTaskDelete(s_watchdog_task);
        s_watchdog_task = nullptr;
    }
    if (s_pca != nullptr && s_i2c_bus != nullptr) {
        i2c_master_bus_rm_device(s_pca);
        s_pca = nullptr;
    }
    if (s_i2c_bus != nullptr && s_i2c_bus_owned) {
        i2c_del_master_bus(s_i2c_bus);
    }
    s_i2c_bus = nullptr;
    s_i2c_bus_owned = false;
    if (s_mutex != nullptr) {
        vSemaphoreDelete(s_mutex);
        s_mutex = nullptr;
    }
    s_started = false;
}
