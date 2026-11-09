/*
 * PROJETO .DOC — DEMONSTRAÇÃO EXPERIMENTAL WS2812B
 *
 * Objetivo:
 *   Testar uma barra WS2812B de 8 LEDs usando GPIO18 (CAMERA_PIN_D5).
 *
 * IMPORTANTE:
 *   - Firmware ESP-IDF independente; não integra a aplicação principal DOC.
 *   - A câmera OV3660 não é inicializada neste projeto de demonstração.
 *   - Não usar a câmera enquanto a barra estiver conectada ao GPIO18.
 *   - Alimentar a barra com fonte regulada de 5 V; manter GND comum.
 *   - Para adequar o nível lógico, recomenda-se buffer 74AHCT125.
 *
 * PONTOS DE AJUSTE:
 *   LED_GPIO       -> pino de dados da barra (atualmente GPIO18).
 *   LED_COUNT      -> quantidade de LEDs (atualmente 8).
 *   LED_BRIGHTNESS -> brilho aplicado aos canais RGB (0 a 255).
 *   WS_*_NS        -> temporização dos bits do protocolo WS2812B.
 *   app_main()     -> sequência visual de demonstração.
 *
 * COMPATIBILIDADE:
 *   Usa a API RMT nova (driver/rmt_tx.h e driver/rmt_encoder.h),
 *   compatível com versões atuais do ESP-IDF que removeram o RMT legado.
 */

#include <stdint.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ===== PONTOS DE AJUSTE DO HARDWARE ===== */
#define LED_GPIO          GPIO_NUM_18 /* GPIO18 = CAMERA_PIN_D5 na placa DOC */
#define LED_COUNT         8           /* Barra WS2812B-8 */
#define LED_BRIGHTNESS    24          /* Brilho de teste: 0 a 255 */

/* ===== TEMPORIZAÇÃO DO PROTOCOLO WS2812B =====
 * A resolução do RMT é 10 MHz: cada tick equivale a 100 ns.
 * Valores em nanossegundos são convertidos para ticks abaixo.
 */
#define RMT_RESOLUTION_HZ 10000000UL
#define WS_T0H_NS         400
#define WS_T0L_NS         900
#define WS_T1H_NS         800
#define WS_T1L_NS         500
#define WS_RESET_MS       1

#define NS_TO_RMT_TICKS(ns) ((uint16_t)(((uint64_t)(ns) * RMT_RESOLUTION_HZ) / 1000000000ULL))

static const char *TAG = "DOC_WS2812_DEMO";

/* Handles mantidos durante toda a execução do firmware. */
static rmt_channel_handle_t led_tx_channel = NULL;
static rmt_encoder_handle_t led_bytes_encoder = NULL;

/* Buffer de 3 bytes por LED, na ordem GRB exigida pelo WS2812B. */
static uint8_t led_data[LED_COUNT * 3U];

/* Reduz o brilho sem alterar a sequência RGB informada. */
static uint8_t scale_color(uint8_t value)
{
    return (uint8_t)(((uint16_t)value * LED_BRIGHTNESS) / 255U);
}

/* Preenche o buffer de todos os LEDs com a mesma cor, em ordem GRB. */
static void fill_all_leds(uint8_t red, uint8_t green, uint8_t blue)
{
    const uint8_t green_scaled = scale_color(green);
    const uint8_t red_scaled = scale_color(red);
    const uint8_t blue_scaled = scale_color(blue);

    for (uint32_t i = 0; i < LED_COUNT; i++) {
        const uint32_t offset = i * 3U;
        led_data[offset + 0U] = green_scaled;
        led_data[offset + 1U] = red_scaled;
        led_data[offset + 2U] = blue_scaled;
    }
}

/* Preenche o buffer para acender apenas o LED selecionado. */
static void fill_single_led(uint32_t selected, uint8_t red, uint8_t green, uint8_t blue)
{
    for (uint32_t i = 0; i < LED_COUNT; i++) {
        const uint32_t offset = i * 3U;

        if (i == selected) {
            led_data[offset + 0U] = scale_color(green);
            led_data[offset + 1U] = scale_color(red);
            led_data[offset + 2U] = scale_color(blue);
        } else {
            led_data[offset + 0U] = 0;
            led_data[offset + 1U] = 0;
            led_data[offset + 2U] = 0;
        }
    }
}

/* Transmite o quadro GRB e aguarda o término antes de reutilizar o buffer. */
static void transmit_led_frame(void)
{
    const rmt_transmit_config_t transmit_config = {
        .loop_count = 0,
        .flags = {
            .eot_level = 0,
            .queue_nonblocking = 0
        }
    };

    ESP_ERROR_CHECK(rmt_transmit(
        led_tx_channel,
        led_bytes_encoder,
        led_data,
        sizeof(led_data),
        &transmit_config
    ));

    ESP_ERROR_CHECK(rmt_tx_wait_all_done(led_tx_channel, -1));

    /* O nível baixo por 1 ms excede o tempo de reset do WS2812B. */
    vTaskDelay(pdMS_TO_TICKS(WS_RESET_MS));
}

/* Envia a mesma cor para todos os LEDs. */
static void show_color(uint8_t red, uint8_t green, uint8_t blue)
{
    fill_all_leds(red, green, blue);
    transmit_led_frame();
}

/* Acende somente um LED, para verificar a ordem física da barra. */
static void show_single_led(uint32_t selected, uint8_t red, uint8_t green, uint8_t blue)
{
    fill_single_led(selected, red, green, blue);
    transmit_led_frame();
}

/* Configura o canal transmissor RMT e o encoder de bytes da API nova. */
static void initialize_ws2812(void)
{
    const rmt_tx_channel_config_t tx_channel_config = {
        .gpio_num = LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
        .flags.invert_out = false,
        .flags.with_dma = false
    };

    const rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {
            .level0 = 1,
            .duration0 = NS_TO_RMT_TICKS(WS_T0H_NS),
            .level1 = 0,
            .duration1 = NS_TO_RMT_TICKS(WS_T0L_NS)
        },
        .bit1 = {
            .level0 = 1,
            .duration0 = NS_TO_RMT_TICKS(WS_T1H_NS),
            .level1 = 0,
            .duration1 = NS_TO_RMT_TICKS(WS_T1L_NS)
        },
        .flags.msb_first = true
    };

    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_channel_config, &led_tx_channel));
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&encoder_config, &led_bytes_encoder));
    ESP_ERROR_CHECK(rmt_enable(led_tx_channel));

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

        /* Varredura laranja para confirmar a ordem dos oito LEDs. */
        for (uint32_t i = 0; i < LED_COUNT; i++) {
            show_single_led(i, 255, 120, 0);
            show_color(0, 0, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
