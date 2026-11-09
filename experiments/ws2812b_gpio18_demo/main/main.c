/*
 * PROJETO .DOC — DEMONSTRAÇÃO EXPERIMENTAL WS2812B
 *
 * Objetivo:
 *   Testar uma barra WS2812B de 8 LEDs usando GPIO18 (CAMERA_PIN_D5).
 *
 * IMPORTANTE:
 *   - Este é um firmware ESP-IDF independente, não integrado à aplicação DOC.
 *   - A câmera OV3660 não é inicializada neste projeto de demonstração.
 *   - Não usar a câmera enquanto a barra estiver conectada ao GPIO18.
 *   - A alimentação de 5 V da barra deve vir de uma fonte apropriada,
 *     com GND comum ao ESP32-S3. Nunca alimentar a barra pelo GPIO.
 *   - Para maior confiabilidade do sinal, usar buffer lógico 74AHCT125
 *     entre o GPIO de 3,3 V e a entrada DIN de 5 V da barra.
 *
 * PONTOS DE AJUSTE:
 *   LED_GPIO       -> pino de dados da barra (atualmente GPIO18).
 *   LED_COUNT      -> quantidade de LEDs (atualmente 8).
 *   RMT_CHANNEL    -> canal RMT usado pelo teste.
 *   show_color()   -> ordem de cores e temporização do protocolo.
 *   app_main()     -> sequência visual de demonstração.
 */

#include <stdint.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/rmt.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ===== PONTOS DE AJUSTE DO HARDWARE ===== */
#define LED_GPIO       GPIO_NUM_18  /* GPIO18 = CAMERA_PIN_D5 na placa DOC */
#define LED_COUNT      8            /* Barra WS2812B-8 */
#define RMT_CHANNEL    RMT_CHANNEL_0
#define RMT_CLK_DIV    8            /* 80 MHz / 8 = 10 MHz (0,1 us por tick) */

#define LED_BRIGHTNESS 24           /* Brilho de teste: 0 a 255 */

/* Temporização aproximada WS2812B em ticks de 0,1 us.
 * Bit 0: HIGH 0,4 us / LOW 0,9 us.
 * Bit 1: HIGH 0,8 us / LOW 0,5 us.
 * Total aproximado: 1,3 us por bit.
 */
#define WS_T0H_TICKS 4
#define WS_T0L_TICKS 9
#define WS_T1H_TICKS 8
#define WS_T1L_TICKS 5
#define WS_RESET_MS  1

static const char *TAG = "DOC_WS2812_DEMO";

/* Buffer de 24 bits por LED; WS2812B recebe GRB, do bit mais significativo. */
static rmt_item32_t led_items[LED_COUNT * 24];

/* Reduz o brilho sem alterar a sequência RGB informada. */
static uint8_t scale_color(uint8_t value)
{
    return (uint8_t)(((uint16_t)value * LED_BRIGHTNESS) / 255U);
}

/* Converte um bit em um símbolo RMT conforme o protocolo WS2812B. */
static void encode_bit(rmt_item32_t *item, bool bit_value)
{
    item->level0 = 1;
    item->level1 = 0;

    if (bit_value) {
        item->duration0 = WS_T1H_TICKS;
        item->duration1 = WS_T1L_TICKS;
    } else {
        item->duration0 = WS_T0H_TICKS;
        item->duration1 = WS_T0L_TICKS;
    }
}

/* Monta os 24 bits de um LED na ordem GRB exigida pelo WS2812B. */
static void encode_led(uint32_t led_index, uint8_t red, uint8_t green, uint8_t blue)
{
    const uint8_t grb[3] = {
        scale_color(green),
        scale_color(red),
        scale_color(blue)
    };

    uint32_t item_index = led_index * 24U;

    for (uint32_t color_index = 0; color_index < 3; color_index++) {
        for (int bit = 7; bit >= 0; bit--) {
            encode_bit(&led_items[item_index++],
                       (grb[color_index] & (1U << bit)) != 0);
        }
    }
}

/* Envia a cor para todos os LEDs e aguarda o tempo de reset do protocolo. */
static void show_color(uint8_t red, uint8_t green, uint8_t blue)
{
    for (uint32_t i = 0; i < LED_COUNT; i++) {
        encode_led(i, red, green, blue);
    }

    ESP_ERROR_CHECK(rmt_write_items(
        RMT_CHANNEL,
        led_items,
        LED_COUNT * 24,
        true
    ));

    /* Linha permanece em nível baixo entre quadros; 1 ms excede o reset. */
    vTaskDelay(pdMS_TO_TICKS(WS_RESET_MS));
}

/* Acende somente um LED, permitindo verificar a sequência física da barra. */
static void show_single_led(uint32_t selected, uint8_t red, uint8_t green, uint8_t blue)
{
    for (uint32_t i = 0; i < LED_COUNT; i++) {
        if (i == selected) {
            encode_led(i, red, green, blue);
        } else {
            encode_led(i, 0, 0, 0);
        }
    }

    ESP_ERROR_CHECK(rmt_write_items(
        RMT_CHANNEL,
        led_items,
        LED_COUNT * 24,
        true
    ));

    vTaskDelay(pdMS_TO_TICKS(WS_RESET_MS));
}

/* Configura o transmissor RMT para o GPIO definido em LED_GPIO. */
static void initialize_ws2812(void)
{
    rmt_config_t config = {
        .rmt_mode = RMT_MODE_TX,
        .channel = RMT_CHANNEL,
        .gpio_num = LED_GPIO,
        .clk_div = RMT_CLK_DIV,
        .mem_block_num = 1,
        .tx_config = {
            .loop_en = false,
            .carrier_en = false,
            .idle_output_en = true,
            .idle_level = RMT_IDLE_LEVEL_LOW,
            .carrier_duty_percent = 50,
            .carrier_freq_hz = 38000,
            .carrier_level = RMT_CARRIER_LEVEL_HIGH
        }
    };

    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(RMT_CHANNEL, 0, 0));

    ESP_LOGI(TAG, "WS2812B pronto: GPIO=%d, LEDs=%d", LED_GPIO, LED_COUNT);
}

/* Firmware de teste: cores primárias e varredura LED por LED. */
void app_main(void)
{
    initialize_ws2812();

    while (true) {
        /* Teste de cores: vermelho, verde, azul e apagado. */
        show_color(255, 0, 0);
        show_color(0, 255, 0);
        show_color(0, 0, 255);
        show_color(0, 0, 0);

        /* Varredura para confirmar a ordem física dos oito LEDs. */
        for (uint32_t i = 0; i < LED_COUNT; i++) {
            show_single_led(i, 255, 120, 0);
            show_color(0, 0, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
