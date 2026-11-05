#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "display/custom_eyes.h"
#include "web_servo_api.h"
#include "mcp_server.h"
// DOC INÍCIO — driver do VL53L0X (ver nota no README: não incluído neste repositório)
#include "vl53l0x_driver.h"
#include <driver/i2c_master.h>
// DOC FIM

#include "esp32_camera.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <driver/spi_master.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include "esp_lcd_st7735.h"

#define TAG "doc-ai-s3cam"

class DocAiS3Cam : public WifiBoard {
private:
    Button boot_button_;
    // DOC INÍCIO — sensores de toque (TTP223) e distância (VL53L0X)
    Button touch_button_;
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    vl53l0x_driver_t* distance_sensor_ = nullptr;
    // DOC FIM
    Display* display_ = nullptr;
    bool servo_api_started_ = false;

    // DOC INÍCIO — Estado da câmera OV3660, aberta somente quando solicitada.
    Camera* camera_ = nullptr;
    std::string camera_vision_url_;
    std::string camera_vision_token_;
    TaskHandle_t camera_preview_task_ = nullptr;
    std::atomic<bool> camera_preview_running_{false};
    std::atomic<bool> camera_preview_stop_requested_{false};

    void InitializeCamera() {
        camera_config_t camera_config = {};
        camera_config.ledc_channel = LEDC_CHANNEL_0;
        camera_config.ledc_timer = LEDC_TIMER_0;
        camera_config.pin_d0 = CAMERA_PIN_D0;
        camera_config.pin_d1 = CAMERA_PIN_D1;
        camera_config.pin_d2 = CAMERA_PIN_D2;
        camera_config.pin_d3 = CAMERA_PIN_D3;
        camera_config.pin_d4 = CAMERA_PIN_D4;
        camera_config.pin_d5 = CAMERA_PIN_D5;
        camera_config.pin_d6 = CAMERA_PIN_D6;
        camera_config.pin_d7 = CAMERA_PIN_D7;
        camera_config.pin_xclk = CAMERA_PIN_XCLK;
        camera_config.pin_pclk = CAMERA_PIN_PCLK;
        camera_config.pin_vsync = CAMERA_PIN_VSYNC;
        camera_config.pin_href = CAMERA_PIN_HREF;
        camera_config.pin_sccb_sda = CAMERA_PIN_SIOD;
        camera_config.pin_sccb_scl = CAMERA_PIN_SIOC;
        camera_config.pin_pwdn = CAMERA_PIN_PWDN;
        camera_config.pin_reset = CAMERA_PIN_RESET;
        camera_config.xclk_freq_hz = XCLK_FREQ_HZ;
        camera_config.pixel_format = PIXFORMAT_RGB565;
        camera_config.frame_size = FRAMESIZE_QQVGA;
        camera_config.jpeg_quality = 12;
        camera_config.fb_count = 2;
        camera_config.fb_location = CAMERA_FB_IN_PSRAM;
        camera_config.grab_mode = CAMERA_GRAB_LATEST;

        camera_ = new Esp32Camera(camera_config);
        camera_->SetExplainUrl(camera_vision_url_, camera_vision_token_);
        camera_->SetVFlip(true);
        ESP_LOGI(TAG, "OV3660 camera initialization requested");
    }

    static void CameraPreviewTask(void* arg) {
        auto* self = static_cast<DocAiS3Cam*>(arg);
        while (!self->camera_preview_stop_requested_.load()) {
            Camera* camera = self->camera_;
            if (camera != nullptr) {
                camera->Capture();
            }
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        self->camera_preview_running_.store(false);
        self->camera_preview_task_ = nullptr;
        vTaskDelete(nullptr);
    }

    void StartCameraPreviewTask() {
        if (camera_ == nullptr || camera_preview_running_.load()) {
            return;
        }

        camera_preview_stop_requested_.store(false);
        camera_preview_running_.store(true);
        BaseType_t result = xTaskCreate(
            CameraPreviewTask, "doc_cam_preview", 4096, this, 2, &camera_preview_task_);
        if (result != pdPASS) {
            camera_preview_running_.store(false);
            camera_preview_task_ = nullptr;
            ESP_LOGE(TAG, "Failed to start camera preview task");
        }
    }

    void StopCameraPreviewTask() {
        if (!camera_preview_running_.load()) {
            camera_preview_task_ = nullptr;
            return;
        }

        camera_preview_stop_requested_.store(true);
        while (camera_preview_running_.load()) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        camera_preview_task_ = nullptr;
        camera_preview_stop_requested_.store(false);
    }
    // DOC FIM — Estado e ciclo de vida da câmera.

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    // DOC INÍCIO — toque TTP223 no GPIO 48: alterna o estado da conversa.
    void InitializeTouchSensor() {
        touch_button_.OnClick([]() {
            Application::GetInstance().ToggleChatState();
        });
        ESP_LOGI(TAG, "TTP223 touch sensor on GPIO %d", TOUCH_BUTTON_GPIO);
    }

    // DOC INÍCIO — barramento I2C0 criado uma única vez pela placa.
    // O PCA9685 (web_servo_api) reutiliza este mesmo barramento.
    bool InitializeI2cBus() {
        i2c_master_bus_config_t bus_config = {};
        bus_config.i2c_port = I2C_NUM_0;
        bus_config.sda_io_num = DOC_I2C_SDA_GPIO;
        bus_config.scl_io_num = DOC_I2C_SCL_GPIO;
        bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
        bus_config.glitch_ignore_cnt = 7;
        bus_config.flags.enable_internal_pullup = true;

        esp_err_t err = i2c_new_master_bus(&bus_config, &i2c_bus_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
            i2c_bus_ = nullptr;
            return false;
        }
        DocWebServoApiSetSharedBus(i2c_bus_);
        return true;
    }

    // Inicializa o VL53L0X no barramento compartilhado.
    void InitializeDistanceSensor() {
        if (i2c_bus_ == nullptr) {
            return;
        }
        esp_err_t err = vl53l0x_driver_create_on_bus(
            i2c_bus_, VL53L0X_I2C_ADDRESS, DOC_I2C_FREQUENCY_HZ, 1000,
            &distance_sensor_);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "VL53L0X init failed: %s", esp_err_to_name(err));
            distance_sensor_ = nullptr;
            return;
        }
        ESP_LOGI(TAG, "VL53L0X ready at 0x%02X", VL53L0X_I2C_ADDRESS);
    }

    // Só registra a ferramenta MCP se o sensor foi inicializado com sucesso.
    void InitializeDistanceMcpTool() {
        if (distance_sensor_ == nullptr) {
            return;
        }
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool(
            "self.doc.distance.read",
            "Read one VL53L0X distance measurement. Returns millimeters.",
            PropertyList(),
            [this](const PropertyList&) -> ToolResult {
                uint16_t distance_mm = 0;
                esp_err_t err =
                    vl53l0x_driver_read_mm(distance_sensor_, &distance_mm);
                if (err != ESP_OK) {
                    return std::unexpected(
                        "VL53L0X measurement failed: " +
                        std::string(esp_err_to_name(err)));
                }
                return std::string("{\"distance_mm\":") +
                       std::to_string(distance_mm) + "}";
            });
    }

    // Leitura em milímetros; ESP_ERR_INVALID_STATE se o sensor não estiver pronto.
    esp_err_t ReadDistanceMillimeters(uint16_t* distance_mm) {
        if (distance_sensor_ == nullptr || distance_mm == nullptr) {
            return ESP_ERR_INVALID_STATE;
        }
        return vl53l0x_driver_read_mm(distance_sensor_, distance_mm);
    }
    // DOC FIM — sensores de toque e distância

    // DOC INÍCIO — API web inicia somente após a rede estar conectada
    void HandleNetworkEvent(NetworkEvent event, const std::string& data) {
        if (event == NetworkEvent::Connected && !servo_api_started_) {
            ESP_LOGI(TAG, "Wi-Fi conectado; iniciando API web de servos");
            DocWebServoApiStart();
            servo_api_started_ = true;
        } else if (event == NetworkEvent::Disconnected && servo_api_started_) {
            // Em desconexão, tenta parar os motores e encerra o servidor.
            DocWebServoApiStop();
            servo_api_started_ = false;
        }
    }
    // DOC FIM

    void InitializeDisplay() {
        spi_bus_config_t bus_config = {};
        bus_config.sclk_io_num = DISPLAY_SPI_CLK_PIN;
        bus_config.mosi_io_num = DISPLAY_SPI_MOSI_PIN;
        bus_config.miso_io_num = GPIO_NUM_NC;
        bus_config.quadwp_io_num = GPIO_NUM_NC;
        bus_config.quadhd_io_num = GPIO_NUM_NC;
        bus_config.max_transfer_sz =
            DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);

        ESP_ERROR_CHECK(
            spi_bus_initialize(SPI3_HOST, &bus_config, SPI_DMA_CH_AUTO));

        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 20 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;

        ESP_ERROR_CHECK(
            esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(
            esp_lcd_new_panel_st7735(panel_io, &panel_config, &panel));

        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
        ESP_ERROR_CHECK(
            esp_lcd_panel_invert_color(panel, DISPLAY_INVERT_COLOR));
        ESP_ERROR_CHECK(
            esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(
            esp_lcd_panel_mirror(
                panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(
            esp_lcd_panel_set_gap(
                panel, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

        display_ = new SpiLcdDisplay(
            panel_io, panel,
            DISPLAY_WIDTH, DISPLAY_HEIGHT,
            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
            DISPLAY_SWAP_XY);

        ESP_LOGI(TAG, "ST7735 display initialized");
    }

public:
    // DOC INÍCIO — preservar o callback da aplicação e tratar eventos da placa
    // Application::Initialize() chama este método depois do construtor.
    // Encadeamos o callback da API web com o callback principal, evitando que
    // o registro posterior da aplicação substitua o tratamento de rede do DOC.
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        WifiBoard::SetNetworkEventCallback(
            [this, callback](NetworkEvent event, const std::string& data) {
                HandleNetworkEvent(event, data);
                if (callback) {
                    callback(event, data);
                }
            });
    }
    // DOC FIM

    DocAiS3Cam() : boot_button_(BOOT_BUTTON_GPIO),
                   touch_button_(TOUCH_BUTTON_GPIO, true) {
        ESP_LOGI(TAG, "DOC AI S3CAM board startup");
        InitializeButtons();
        InitializeTouchSensor();
        if (InitializeI2cBus()) {
            InitializeDistanceSensor();
        }
        InitializeDistanceMcpTool();
        InitializeDisplay();

        // A Application registra seu callback durante Initialize().
        // O encadeamento do callback é implementado em SetNetworkEventCallback().

        ESP_LOGI(TAG,
                 "I2S mic BCLK=%d WS=%d DIN=%d; speaker BCLK=%d WS=%d DOUT=%d",
                 AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS,
                 AUDIO_I2S_MIC_GPIO_DIN,
                 AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
                 AUDIO_I2S_SPK_GPIO_DOUT);
    }

    ~DocAiS3Cam() override {
        CloseCamera();
        if (servo_api_started_) {
            DocWebServoApiStop();
        }
        // DOC — ordem: sensor antes do barramento; o barramento é de fora do módulo servo.
        if (distance_sensor_ != nullptr) {
            vl53l0x_driver_delete(distance_sensor_);
            distance_sensor_ = nullptr;
        }
        if (i2c_bus_ != nullptr) {
            DocWebServoApiSetSharedBus(nullptr);
            i2c_del_master_bus(i2c_bus_);
            i2c_bus_ = nullptr;
        }
    }

    // DOC INÍCIO — API pública da câmera OV3660.
    bool HasCamera() override {
        return true;
    }

    bool SupportsCameraLifecycle() override {
        return true;
    }

    Camera* GetCamera() override {
        return camera_;
    }

    Camera* OpenCamera() override {
        if (camera_ == nullptr) {
            InitializeCamera();
            if (camera_ == nullptr || !camera_->Capture()) {
                ESP_LOGE(TAG, "Failed to initialize or capture from OV3660");
                delete camera_;
                camera_ = nullptr;
                return nullptr;
            }
        }
        camera_->SetExplainUrl(camera_vision_url_, camera_vision_token_);
        // DOC INÍCIO — Com a câmera aberta, olhos na faixa y=0..31 e imagem em y=32..159.
        if (display_ != nullptr) {
            static_cast<LcdDisplay*>(display_)->SetCameraPreviewMode(true);
        }
        CustomEyes_SetCameraMode(true);
        // DOC FIM — Layout da câmera.
        StartCameraPreviewTask();
        return camera_;
    }

    bool CloseCamera() override {
        StopCameraPreviewTask();
        delete camera_;
        camera_ = nullptr;
        if (display_ != nullptr) {
            display_->SetPreviewImage(nullptr);
            // DOC — Restaura a prévia e os olhos ao centro após fechar a câmera.
            static_cast<LcdDisplay*>(display_)->SetCameraPreviewMode(false);
        }
        CustomEyes_SetCameraMode(false);
        ESP_LOGI(TAG, "OV3660 camera closed");
        return true;
    }

    void SetCameraVisionEndpoint(const std::string& url, const std::string& token) override {
        camera_vision_url_ = url;
        camera_vision_token_ = token;
        if (camera_ != nullptr) {
            camera_->SetExplainUrl(camera_vision_url_, camera_vision_token_);
        }
    }

    std::expected<std::string, std::string> CaptureAndExplain(
        const std::string& question) override {
        const bool camera_was_open = camera_ != nullptr;
        if (!camera_was_open && OpenCamera() == nullptr) {
            return std::unexpected("Failed to open OV3660 camera");
        }

        // Pause preview during photo analysis so two tasks do not capture at once.
        StopCameraPreviewTask();
        if (camera_ == nullptr || !camera_->Capture()) {
            if (camera_was_open) {
                StartCameraPreviewTask();
            } else {
                CloseCamera();
            }
            return std::unexpected("Failed to capture photo");
        }

        auto result = camera_->Explain(question);
        if (camera_was_open) {
            StartCameraPreviewTask();
        } else {
            CloseCamera();
        }
        return result;
    }
    // DOC FIM — API pública da câmera OV3660.

    AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK,
            AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS,
            AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(DocAiS3Cam);
