
/*
 * DOC — Motor de olhos animados para LVGL.
 * Arquivo: main/display/custom_eyes.c
 *
 * DOC — Objetivos:
 *   - Aproximar o visual de playfultechnology/esp32-eyes.
 *   - Usar formas arredondadas em vez de círculos sobrepostos.
 *   - Preservar a API usada pelo firmware DOC.
 *   - Manter piscadas, movimento do olhar e expressões recebidas por texto.
 *
 * DOC — Ajustes visuais principais:
 *   SCREEN_W / SCREEN_H          : resolução lógica.
 *   EYE_DISTANCE                 : distância entre os olhos.
 *   EYE_CENTER_Y_OFFSET          : posição vertical.
 *   COLOR_EYE                    : cor principal.
 *   HIGHLIGHT_SIZE               : tamanho do brilho.
 *   apply_expression_targets()   : formato de cada expressão.
 */

#include "custom_eyes.h"

#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>

/* DOC — Identificação das mensagens no log do ESP-IDF. */
static const char *TAG = "CustomEyes";

/* ============================================================
 * DOC — 1. CONFIGURAÇÕES FÁCEIS DE ALTERAR
 * ============================================================ */

/* DOC — Resolução lógica da tela ST7735 do DOC. */
#define SCREEN_W                    128
#define SCREEN_H                    160

/* DOC — Altura da caixa superior reservada aos olhos com a câmera aberta. */
#define CAMERA_INFO_HEIGHT           32

/* DOC — Distância horizontal entre os centros dos olhos. */
#define EYE_DISTANCE                52

/* DOC — Valor negativo sobe os olhos; positivo desce os olhos. */
#define EYE_CENTER_Y_OFFSET         (0)  /* DOC — centralização vertical */

/* DOC — Dimensões iniciais dos olhos, em pixels. */
#define EYE_WIDTH_BASE              36
#define EYE_HEIGHT_BASE             36

/* DOC — Cores RGB em hexadecimal. */
#define COLOR_EYE                   0x43D9F5
#define COLOR_EYE_HIGHLIGHT         0xFFFFFF

/* DOC — Tamanho e posição do brilho. */
#define HIGHLIGHT_SIZE              5
#define HIGHLIGHT_OFFSET_X          (-7)
#define HIGHLIGHT_OFFSET_Y          (-7)

/* DOC — Velocidade das transições. */
#define LOOK_SPEED                  0.12f
#define SHAPE_SPEED                 0.16f

/* DOC — Intervalo entre piscadas automáticas. */
#define BLINK_MIN_INTERVAL_MS       2200
#define BLINK_MAX_INTERVAL_MS       5000
#define BLINK_FRAME_STEP            0.22f

/* DOC — Movimento do olhar durante o estado neutro. */
#define IDLE_LOOK_INTERVAL_MS       2400
#define IDLE_LOOK_RANGE_X           0.32f
#define IDLE_LOOK_RANGE_Y           0.18f

/* DOC — Deslocamento máximo do olhar em pixels. */
#define LOOK_OFFSET_X_MAX           5
#define LOOK_OFFSET_Y_MAX           4

/* ============================================================
 * DOC — 2. ESTRUTURA INTERNA DE CADA OLHO
 * ============================================================ */

/*
 * DOC — Cada olho tem uma forma arredondada e um brilho.
 * Os valores atuais são interpolados até os valores-alvo.
 */
typedef struct {
    lv_obj_t *shape;
    lv_obj_t *highlight;

    float width;
    float height;
    float open;
    float look_x;
    float look_y;
    float offset_x;
    float offset_y;

    float target_width;
    float target_height;
    float target_open;
    float target_look_x;
    float target_look_y;
    float target_offset_x;
    float target_offset_y;
} eye_t;

/* DOC — Expressões reconhecidas pelo módulo. */
typedef enum {
    DOC_EXPR_NEUTRAL = 0,
    DOC_EXPR_LISTENING,
    DOC_EXPR_SPEAKING,
    DOC_EXPR_THINKING,
    DOC_EXPR_HAPPY,
    DOC_EXPR_SAD,
    DOC_EXPR_ANGRY,
    DOC_EXPR_SURPRISED,
    DOC_EXPR_SLEEPY,
    DOC_EXPR_CONFUSED,
    /* DOC — Novas expressões portadas do sketch anexo, renderizadas como presets LVGL. */
    DOC_EXPR_LOVE,
    DOC_EXPR_DIZZY,
    DOC_EXPR_WINK,
    DOC_EXPR_FURIOUS,
    DOC_EXPR_SMILE,
    DOC_EXPR_SMUG,
    DOC_EXPR_STAR_EYES,
    DOC_EXPR_COOL,
    DOC_EXPR_CHEEKY,
    DOC_EXPR_DERP
} doc_expression_t;

/* ============================================================
 * DOC — 3. VARIÁVEIS DO MOTOR
 * ============================================================ */

static eye_t left_eye;
static eye_t right_eye;

static lv_timer_t *anim_timer = NULL;
static lv_obj_t *parent_ref = NULL;

/* DOC — Controla se os olhos ocupam a faixa superior ou o centro da tela. */
static bool camera_mode_active = false;

static eyes_state_t current_state = EYES_STATE_IDLE;
static doc_expression_t current_expression = DOC_EXPR_NEUTRAL;

static float blink_progress = 0.0f;
static bool is_blinking = false;

static uint32_t next_blink_time = 0;
static uint32_t last_look_change = 0;

/* DOC — Objetos decorativos da segunda etapa. Todos são criados uma vez
 * e apenas reposicionados/ocultados no timer para evitar alocações contínuas.
 */
static lv_obj_t *heart_left = NULL;
static lv_obj_t *heart_right = NULL;
static lv_obj_t *tear_left = NULL;
static lv_obj_t *tear_right = NULL;
static lv_obj_t *spiral_left = NULL;
static lv_obj_t *spiral_right = NULL;
static lv_obj_t *anger_mark = NULL;
static lv_obj_t *star_left = NULL;
static lv_obj_t *star_right = NULL;
static lv_point_t spiral_left_points[13];
static lv_point_t spiral_right_points[13];
static lv_point_t star_left_points[11];
static lv_point_t star_right_points[11];
static lv_point_t anger_points[5];
static float effect_phase = 0.0f;

/* ============================================================
 * DOC — 4. CRIAÇÃO DOS OBJETOS LVGL
 * ============================================================ */

/* DOC — Cria a forma principal arredondada de um olho. */
static lv_obj_t *create_eye_shape(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_set_size(obj, EYE_WIDTH_BASE, EYE_HEIGHT_BASE);

    /* DOC — O raio circular cria o contorno arredondado. */
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(
        obj, lv_color_hex(COLOR_EYE), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);

    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    return obj;
}

/* DOC — Cria o brilho branco decorativo do olho. */
static lv_obj_t *create_eye_highlight(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_set_size(obj, HIGHLIGHT_SIZE, HIGHLIGHT_SIZE);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(
        obj, lv_color_hex(COLOR_EYE_HIGHLIGHT), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);

    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);

    return obj;
}

/* DOC — Interpolação linear para suavizar as mudanças. */
static float lerp(float current, float target, float speed)
{
    float diff = target - current;

    if (fabsf(diff) < 0.02f) {
        return target;
    }

    return current + diff * speed;
}

/* DOC — Limita um valor a um intervalo seguro. */
static float clampf_local(float value, float minimum, float maximum)
{
    if (value < minimum) return minimum;
    if (value > maximum) return maximum;

    return value;
}

/* DOC — Inicializa os objetos e os valores de um olho. */
static void init_one_eye(eye_t *eye, lv_obj_t *parent)
{
    memset(eye, 0, sizeof(*eye));

    eye->shape = create_eye_shape(parent);
    eye->highlight = create_eye_highlight(parent);

    eye->width = (float)EYE_WIDTH_BASE;
    eye->height = (float)EYE_HEIGHT_BASE;
    eye->open = 1.0f;

    eye->target_width = (float)EYE_WIDTH_BASE;
    eye->target_height = (float)EYE_HEIGHT_BASE;
    eye->target_open = 1.0f;
}

/* ============================================================
 * DOC — 4B. EFEITOS DECORATIVOS LVGL
 * DOC — Objetos são criados uma única vez e reaproveitados pelo timer.
 * ============================================================ */

static lv_obj_t *create_effect_label(lv_obj_t *parent, const char *symbol,
                                     uint32_t color, uint16_t size)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, symbol);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_font(obj, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);
    lv_obj_set_style_text_line_space(obj, 0, 0);
    lv_obj_set_style_transform_zoom(obj, size, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *create_effect_dot(lv_obj_t *parent, uint32_t color, int size)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, size, size);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static lv_obj_t *create_effect_line(lv_obj_t *parent, uint32_t color, int width)
{
    lv_obj_t *obj = lv_line_create(parent);
    lv_obj_set_style_line_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_line_width(obj, width, 0);
    lv_obj_set_style_line_rounded(obj, true, 0);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void create_expression_effects(lv_obj_t *parent)
{
    /* DOC — Símbolo de coração fornecido pela fonte de símbolos do LVGL. */
    heart_left = create_effect_label(parent, LV_SYMBOL_HEART, 0xFF6FAE, 384);
    heart_right = create_effect_label(parent, LV_SYMBOL_HEART, 0xFF6FAE, 2);

    /* DOC — Lágrimas simples; o deslocamento vertical simula a queda. */
    tear_left = create_effect_dot(parent, 0x55BFFF, 5);
    tear_right = create_effect_dot(parent, 0x55BFFF, 5);

    /* DOC — Linhas para espirais, marca de raiva e estrelas. */
    spiral_left = create_effect_line(parent, 0xF5C451, 2);
    spiral_right = create_effect_line(parent, 0xF5C451, 2);
    anger_mark = create_effect_line(parent, 0xFF5A36, 2);
    star_left = create_effect_line(parent, 0xFFD34E, 2);
    star_right = create_effect_line(parent, 0xFFD34E, 2);
}

static void hide_expression_effects(void)
{
    lv_obj_t *items[] = {heart_left, heart_right, tear_left, tear_right,
                         spiral_left, spiral_right, anger_mark,
                         star_left, star_right};
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        if (items[i] != NULL) lv_obj_add_flag(items[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* DOC — Atualiza uma estrela de cinco pontas como polilinha LVGL. */
static void update_star_points(lv_point_t *points, int cx, int cy, int radius,
                               float rotation)
{
    for (int i = 0; i < 10; i++) {
        float angle = rotation - 1.5707963f + (float)i * 0.6283185f;
        float r = (i % 2 == 0) ? (float)radius : (float)radius * 0.43f;
        points[i].x = (lv_coord_t)(cx + cosf(angle) * r);
        points[i].y = (lv_coord_t)(cy + sinf(angle) * r);
    }
    points[10] = points[0];
}

/* DOC — Atualiza os efeitos sem criar objetos durante a animação. */
static void update_expression_effects(uint32_t now, int center_y,
                                      int left_cx, int right_cx)
{
    if (heart_left == NULL) return;

    hide_expression_effects();
    effect_phase += 0.10f;

    if (current_expression == DOC_EXPR_LOVE) {
        int float_y = (int)(sinf(effect_phase) * 4.0f);
        lv_obj_clear_flag(heart_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(heart_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(heart_left, left_cx - 8, center_y + 28 - float_y);
        lv_obj_set_pos(heart_right, right_cx + 2, center_y + 20 + float_y);
    } else if (current_expression == DOC_EXPR_SAD) {
        int fall = (int)((now / 55U) % 13U);
        lv_obj_clear_flag(tear_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(tear_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(tear_left, left_cx + 8, center_y + 13 + fall);
        lv_obj_set_pos(tear_right, right_cx + 8, center_y + 13 + ((fall + 6) % 13));
    } else if (current_expression == DOC_EXPR_DIZZY) {
        /* DOC — Espirais formadas por 13 pontos; rotação contínua e suave. */
        for (int i = 0; i < 13; i++) {
            float t = (float)i / 12.0f;
            float a = effect_phase * 1.8f + t * 6.2831853f;
            float r = 2.0f + t * 10.0f;
            spiral_left_points[i].x = (lv_coord_t)(left_cx + cosf(a) * r);
            spiral_left_points[i].y = (lv_coord_t)(center_y + sinf(a) * r);
            spiral_right_points[i].x = (lv_coord_t)(right_cx + cosf(-a) * r);
            spiral_right_points[i].y = (lv_coord_t)(center_y + sinf(-a) * r);
        }
        lv_line_set_points(spiral_left, spiral_left_points, 13);
        lv_line_set_points(spiral_right, spiral_right_points, 13);
        lv_obj_clear_flag(spiral_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(spiral_right, LV_OBJ_FLAG_HIDDEN);
    } else if (current_expression == DOC_EXPR_ANGRY ||
               current_expression == DOC_EXPR_FURIOUS) {
        /* DOC — Marca de raiva em zigue-zague no canto superior direito. */
        anger_points[0] = (lv_point_t){right_cx + 10, center_y - 20};
        anger_points[1] = (lv_point_t){right_cx + 17, center_y - 14};
        anger_points[2] = (lv_point_t){right_cx + 12, center_y - 8};
        anger_points[3] = (lv_point_t){right_cx + 20, center_y - 3};
        anger_points[4] = (lv_point_t){right_cx + 14, center_y + 2};
        lv_line_set_points(anger_mark, anger_points, 5);
        lv_obj_clear_flag(anger_mark, LV_OBJ_FLAG_HIDDEN);
    } else if (current_expression == DOC_EXPR_STAR_EYES) {
        /* DOC — Duas estrelas desenhadas em polilinha, sincronizadas com o olhar. */
        update_star_points(star_left_points, left_cx, center_y, 13, effect_phase * 0.4f);
        update_star_points(star_right_points, right_cx, center_y, 13, -effect_phase * 0.4f);
        lv_line_set_points(star_left, star_left_points, 11);
        lv_line_set_points(star_right, star_right_points, 11);
        lv_obj_clear_flag(star_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(star_right, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ============================================================
 * DOC — 5. PRESETS VISUAIS DAS EXPRESSÕES
 *
 * DOC — Esta é a seção principal para alterar a aparência.
 * width/height controlam o formato.
 * look_x/look_y controlam a direção do olhar.
 * offset_y desloca um olho independentemente do outro.
 * ============================================================ */

static void apply_expression_targets(void)
{
    float width = (float)EYE_WIDTH_BASE;
    float height = (float)EYE_HEIGHT_BASE;
    float open = 1.0f;

    float left_look_x = 0.0f;
    float right_look_x = 0.0f;
    float look_y = 0.0f;

    float left_offset_y = 0.0f;
    float right_offset_y = 0.0f;

    /*
     * DOC — Presets inspirados na abordagem de olhos configuráveis
     * do projeto esp32-eyes, adaptados à tela vertical do DOC.
     */
    switch (current_expression) {

        case DOC_EXPR_NEUTRAL:
            /* DOC — Expressão padrão, olhos equilibrados. */
            width = 36.0f;
            height = 36.0f;
            open = 1.0f;
            break;

        case DOC_EXPR_LISTENING:
            /* DOC — Olhos altos e atentos, olhando levemente para cima. */
            width = 34.0f;
            height = 42.0f;
            open = 1.0f;
            look_y = -0.20f;
            break;

        case DOC_EXPR_SPEAKING:
            /* DOC — Olhos um pouco mais baixos durante a fala. */
            width = 38.0f;
            height = 29.0f;
            open = 0.92f;
            break;

        case DOC_EXPR_THINKING:
            /* DOC — Olhos semicerrados e olhar desviado. */
            width = 33.0f;
            height = 25.0f;
            open = 0.82f;

            left_look_x = 0.30f;
            right_look_x = 0.38f;
            look_y = -0.35f;

            left_offset_y = -2.0f;
            right_offset_y = 1.0f;
            break;

        case DOC_EXPR_HAPPY:
            /* DOC — Olhos mais largos e semicerrados. */
            width = 40.0f;
            height = 25.0f;
            open = 0.86f;

            left_offset_y = 1.0f;
            right_offset_y = 1.0f;
            break;

        case DOC_EXPR_SAD:
            /* DOC — Olhos menores, mais baixos e olhando para baixo. */
            width = 34.0f;
            height = 29.0f;
            open = 0.78f;
            look_y = 0.28f;

            left_offset_y = 3.0f;
            right_offset_y = 3.0f;
            break;

        case DOC_EXPR_ANGRY:
            /* DOC — Olhos estreitos para uma expressão mais firme. */
            width = 38.0f;
            height = 22.0f;
            open = 0.76f;

            left_offset_y = 1.0f;
            right_offset_y = 1.0f;
            break;

        case DOC_EXPR_SURPRISED:
            /* DOC — Olhos estreitos na largura e muito altos. */
            width = 31.0f;
            height = 45.0f;
            open = 1.0f;
            break;

        case DOC_EXPR_SLEEPY:
            /* DOC — Olhos muito semicerrados. */
            width = 38.0f;
            height = 18.0f;
            open = 0.72f;

            left_offset_y = 3.0f;
            right_offset_y = 3.0f;
            break;

        case DOC_EXPR_CONFUSED:
            /* DOC — Assimetria sutil para sugerir dúvida. */
            width = 35.0f;
            height = 31.0f;
            open = 0.88f;

            left_offset_y = -3.0f;
            right_offset_y = 2.0f;

            left_look_x = -0.18f;
            right_look_x = 0.24f;
            look_y = -0.12f;
            break;

        case DOC_EXPR_LOVE:
            /* DOC — Olhos semicerrados com tonalidade rosa (efeito de carinho). */
            width = 39.0f; height = 24.0f; open = 0.88f;
            break;

        case DOC_EXPR_DIZZY:
            /* DOC — Olhos circulares e olhar desalinhado; espirais animadas ficam para fase 2. */
            width = 30.0f; height = 30.0f; open = 1.0f;
            left_look_x = -0.12f; right_look_x = 0.12f;
            break;

        case DOC_EXPR_WINK:
            /* DOC — Base normal; abaixo será fechado apenas o olho esquerdo. */
            width = 36.0f; height = 34.0f; open = 1.0f;
            break;

        case DOC_EXPR_FURIOUS:
            /* DOC — Olhos compactos e estreitos para expressão intensa. */
            width = 39.0f; height = 20.0f; open = 0.72f;
            left_offset_y = 1.0f; right_offset_y = 1.0f;
            break;

        case DOC_EXPR_SMILE:
            /* DOC — Olhos largos e baixos, estilo sorriso. */
            width = 40.0f; height = 22.0f; open = 0.82f;
            left_offset_y = 2.0f; right_offset_y = 2.0f;
            break;

        case DOC_EXPR_SMUG:
            /* DOC — Olhar lateral assimétrico, expressão convencida. */
            width = 35.0f; height = 27.0f; open = 0.84f;
            left_look_x = 0.28f; right_look_x = 0.42f;
            left_offset_y = 1.0f; right_offset_y = -2.0f;
            break;

        case DOC_EXPR_STAR_EYES:
            /* DOC — Olhos mais abertos; estrelas decorativas não são desenhadas nesta fase. */
            width = 30.0f; height = 42.0f; open = 1.0f;
            break;

        case DOC_EXPR_COOL:
            /* DOC — Olhos horizontais e estreitos, referência visual de óculos escuros. */
            width = 40.0f; height = 19.0f; open = 0.78f;
            break;

        case DOC_EXPR_CHEEKY:
            /* DOC — Um olho mais fechado e olhar deslocado, expressão brincalhona. */
            width = 37.0f; height = 27.0f; open = 0.90f;
            left_look_x = -0.22f; right_look_x = 0.24f;
            break;

        case DOC_EXPR_DERP:
            /* DOC — Olhos desencontrados e olhar divergente. */
            width = 31.0f; height = 32.0f; open = 1.0f;
            left_look_x = -0.48f; right_look_x = 0.48f;
            left_offset_y = -2.0f; right_offset_y = 2.0f;
            break;

        default:
            break;
    }

    /* DOC — Aplica o formato calculado aos dois olhos. */
    left_eye.target_width = width;
    right_eye.target_width = width;

    left_eye.target_height = height;
    right_eye.target_height = height;

    left_eye.target_open = open;
    right_eye.target_open = open;

    left_eye.target_look_x = left_look_x;
    right_eye.target_look_x = right_look_x;

    left_eye.target_look_y = look_y;
    right_eye.target_look_y = look_y;

    left_eye.target_offset_x = 0.0f;
    right_eye.target_offset_x = 0.0f;

    left_eye.target_offset_y = left_offset_y;
    right_eye.target_offset_y = right_offset_y;

    /* DOC — Preset assimétrico: piscadela fecha só o olho esquerdo. */
    if (current_expression == DOC_EXPR_WINK) {
        left_eye.target_height = 4.0f;
        left_eye.target_open = 1.0f;
    } else if (current_expression == DOC_EXPR_CHEEKY) {
        left_eye.target_height = 17.0f;
        left_eye.target_open = 0.72f;
    }
}

/* DOC — Converte os estados antigos em expressões visuais. */
static void apply_state_as_expression(eyes_state_t state)
{
    switch (state) {
        case EYES_STATE_IDLE:
            current_expression = DOC_EXPR_NEUTRAL;
            break;

        case EYES_STATE_LISTENING:
            current_expression = DOC_EXPR_LISTENING;
            break;

        case EYES_STATE_SPEAKING:
            current_expression = DOC_EXPR_SPEAKING;
            break;

        case EYES_STATE_THINKING:
            current_expression = DOC_EXPR_THINKING;
            break;

        default:
            current_expression = DOC_EXPR_NEUTRAL;
            break;
    }

    apply_expression_targets();
}

/* ============================================================
 * DOC — 6. ATUALIZAÇÃO VISUAL DE CADA OLHO
 * ============================================================ */

/* DOC — Paleta emocional: fácil de ajustar sem tocar na lógica da animação. */
static uint32_t expression_eye_color(void)
{
    switch (current_expression) {
        case DOC_EXPR_LOVE:      return 0xFF6FAE; /* rosa */
        case DOC_EXPR_SAD:       return 0x58A6FF; /* azul */
        case DOC_EXPR_ANGRY:
        case DOC_EXPR_FURIOUS:   return 0xFF5A36; /* vermelho/laranja */
        case DOC_EXPR_CONFUSED:  return 0xF5C451; /* âmbar */
        case DOC_EXPR_HAPPY:
        case DOC_EXPR_SMILE:     return 0x55D6A7; /* verde menta */
        case DOC_EXPR_STAR_EYES: return 0xFFD34E; /* dourado */
        case DOC_EXPR_SLEEPY:    return 0x9A9FEF; /* lilás */
        case DOC_EXPR_COOL:      return 0x9CE7E7; /* ciano claro */
        default:                 return COLOR_EYE;
    }
}

static void update_eye_visual(eye_t *eye, int center_x, int center_y)
{
    float blink_scale = 1.0f;

    /* DOC — A piscada reduz a altura e depois reabre o olho. */
    if (is_blinking) {
        float t = blink_progress;

        if (t < 0.5f) {
            blink_scale = 1.0f - (t * 2.0f);
        } else {
            blink_scale = (t - 0.5f) * 2.0f;
        }

        blink_scale = clampf_local(blink_scale, 0.08f, 1.0f);
    }

    /* DOC — Reduz o tamanho renderizado para recuperar a escala funcional anterior. */
    const float EYE_RENDER_SCALE = 0.80f;
    float width = eye->width * EYE_RENDER_SCALE;
    float height = eye->height * eye->open * blink_scale * EYE_RENDER_SCALE;

    int draw_w = (int)(width + 0.5f);
    int draw_h = (int)(height + 0.5f);

    if (draw_w < 4) draw_w = 4;
    if (draw_h < 3) draw_h = 3;

    /* DOC — Converte o olhar de -1..1 em deslocamento de pixels. */
    int gaze_x = (int)(eye->look_x * LOOK_OFFSET_X_MAX);
    int gaze_y = (int)(eye->look_y * LOOK_OFFSET_Y_MAX);

    int x = center_x - (draw_w / 2)
          + gaze_x + (int)eye->offset_x;

    int y = center_y - (draw_h / 2)
          + gaze_y + (int)eye->offset_y;

    lv_obj_set_size(eye->shape, draw_w, draw_h);
    lv_obj_set_pos(eye->shape, x, y);

    /* DOC — Aplica a cor correspondente à expressão ativa. */
    lv_obj_set_style_bg_color(eye->shape, lv_color_hex(expression_eye_color()), 0);

    /* DOC — Mantém as bordas arredondadas ao mudar o tamanho. */
    lv_obj_set_style_radius(eye->shape, LV_RADIUS_CIRCLE, 0);

    /* DOC — O brilho acompanha o olho. */
    int highlight_x = x + (draw_w / 2) + HIGHLIGHT_OFFSET_X;
    int highlight_y = y + (draw_h / 2) + HIGHLIGHT_OFFSET_Y;

    lv_obj_set_pos(eye->highlight, highlight_x, highlight_y);

    /* DOC — Esconde o brilho quando o olho estiver quase fechado. */
    if (draw_h < 10 || blink_scale < 0.25f) {
        lv_obj_add_flag(eye->highlight, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_clear_flag(eye->highlight, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ============================================================
 * DOC — 7. TIMER DE ANIMAÇÃO
 * ============================================================ */

static void anim_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    uint32_t now = lv_tick_get();

    /* DOC — Inicia uma piscada quando chega a hora programada. */
    if (!is_blinking && (int32_t)(now - next_blink_time) >= 0) {
        is_blinking = true;
        blink_progress = 0.0f;
    }

    /* DOC — Avança a piscada e agenda a próxima. */
    if (is_blinking) {
        blink_progress += BLINK_FRAME_STEP;

        if (blink_progress >= 1.0f) {
            is_blinking = false;
            blink_progress = 0.0f;

            uint32_t span =
                BLINK_MAX_INTERVAL_MS - BLINK_MIN_INTERVAL_MS;

            uint32_t interval = BLINK_MIN_INTERVAL_MS;

            if (span > 0) {
                interval +=
                    (uint32_t)(rand() % (int)(span + 1));
            }

            next_blink_time = now + interval;
        }
    }

    /* DOC — Olhar aleatório suave somente no repouso neutro. */
    if (current_state == EYES_STATE_IDLE &&
        current_expression == DOC_EXPR_NEUTRAL &&
        (uint32_t)(now - last_look_change) >= IDLE_LOOK_INTERVAL_MS) {

        last_look_change = now;

        float random_x =
            ((float)(rand() % 201) - 100.0f) / 100.0f;

        float random_y =
            ((float)(rand() % 201) - 100.0f) / 100.0f;

        left_eye.target_look_x = random_x * IDLE_LOOK_RANGE_X;
        right_eye.target_look_x =
            left_eye.target_look_x * 0.92f;

        left_eye.target_look_y = random_y * IDLE_LOOK_RANGE_Y;
        right_eye.target_look_y = left_eye.target_look_y;
    }

    /* DOC — Interpola formato, abertura e direção do olhar. */
    left_eye.width =
        lerp(left_eye.width, left_eye.target_width, SHAPE_SPEED);

    right_eye.width =
        lerp(right_eye.width, right_eye.target_width, SHAPE_SPEED);

    left_eye.height =
        lerp(left_eye.height, left_eye.target_height, SHAPE_SPEED);

    right_eye.height =
        lerp(right_eye.height, right_eye.target_height, SHAPE_SPEED);

    left_eye.open =
        lerp(left_eye.open, left_eye.target_open, SHAPE_SPEED);

    right_eye.open =
        lerp(right_eye.open, right_eye.target_open, SHAPE_SPEED);

    left_eye.look_x =
        lerp(left_eye.look_x, left_eye.target_look_x, LOOK_SPEED);

    right_eye.look_x =
        lerp(right_eye.look_x, right_eye.target_look_x, LOOK_SPEED);

    left_eye.look_y =
        lerp(left_eye.look_y, left_eye.target_look_y, LOOK_SPEED);

    right_eye.look_y =
        lerp(right_eye.look_y, right_eye.target_look_y, LOOK_SPEED);

    left_eye.offset_x =
        lerp(left_eye.offset_x, left_eye.target_offset_x, SHAPE_SPEED);

    right_eye.offset_x =
        lerp(right_eye.offset_x, right_eye.target_offset_x, SHAPE_SPEED);

    left_eye.offset_y =
        lerp(left_eye.offset_y, left_eye.target_offset_y, SHAPE_SPEED);

    right_eye.offset_y =
        lerp(right_eye.offset_y, right_eye.target_offset_y, SHAPE_SPEED);

    /* DOC — Calcula os centros para a tela vertical 128x160. */
    /* DOC — Câmera aberta: centro da faixa y=0..31; fechada: centro y=80. */
    int center_y = (camera_mode_active ? (CAMERA_INFO_HEIGHT / 2) : (SCREEN_H / 2))
                 + EYE_CENTER_Y_OFFSET;

    int left_cx =
        (SCREEN_W / 2) - (EYE_DISTANCE / 2);

    int right_cx =
        (SCREEN_W / 2) + (EYE_DISTANCE / 2);

    update_eye_visual(&left_eye, left_cx, center_y);
    update_eye_visual(&right_eye, right_cx, center_y);

    /* DOC — Efeitos decorativos sincronizados com a expressão atual. */
    update_expression_effects(now, center_y, left_cx, right_cx);
}

/* ============================================================
 * DOC — 8. API PÚBLICA DO MÓDULO
 * ============================================================ */

/* DOC — Inicializa os olhos dentro do objeto LVGL recebido. */
void CustomEyes_Init(lv_obj_t *parent)
{
    if (parent == NULL) {
        ESP_LOGE(TAG, "CustomEyes_Init: parent é NULL");
        return;
    }

    /* DOC — Evita criar objetos e timer duplicados. */
    if (anim_timer != NULL) {
        ESP_LOGW(TAG, "CustomEyes_Init chamado novamente; ignorando");
        return;
    }

    parent_ref = parent;

    /*
     * DOC — Não alteramos o fundo do parent.
     * O tema do Xiaozhi continua responsável pelo fundo da tela.
     */

    init_one_eye(&left_eye, parent);
    init_one_eye(&right_eye, parent);

    /* DOC — Cria uma única vez os elementos gráficos da segunda etapa. */
    create_expression_effects(parent);

    current_state = EYES_STATE_IDLE;
    current_expression = DOC_EXPR_NEUTRAL;
    apply_expression_targets();

    /* DOC — Posiciona os olhos antes do primeiro ciclo do timer. */
    /* DOC — Câmera aberta: centro da faixa y=0..31; fechada: centro y=80. */
    int center_y = (camera_mode_active ? (CAMERA_INFO_HEIGHT / 2) : (SCREEN_H / 2))
                 + EYE_CENTER_Y_OFFSET;

    int left_cx =
        (SCREEN_W / 2) - (EYE_DISTANCE / 2);

    int right_cx =
        (SCREEN_W / 2) + (EYE_DISTANCE / 2);

    update_eye_visual(&left_eye, left_cx, center_y);
    update_eye_visual(&right_eye, right_cx, center_y);

    last_look_change = lv_tick_get();
    next_blink_time = lv_tick_get() + 2000;

    is_blinking = false;
    blink_progress = 0.0f;

    /* DOC — Timer de aproximadamente 30 atualizações por segundo. */
    anim_timer = lv_timer_create(anim_timer_cb, 33, NULL);

    if (anim_timer == NULL) {
        ESP_LOGE(TAG, "Não foi possível criar o timer dos olhos");
        return;
    }

    ESP_LOGI(
        TAG,
        "Motor de olhos DOC inicializado (%dx%d)",
        SCREEN_W,
        SCREEN_H
    );
}

/* DOC — Mantém compatibilidade com os estados de atividade antigos. */
void CustomEyes_SetState(eyes_state_t state)
{
    if (state >= EYES_STATE_COUNT) {
        ESP_LOGW(TAG, "Estado inválido: %d", (int)state);
        return;
    }

    current_state = state;
    apply_state_as_expression(state);

    ESP_LOGD(TAG, "Estado dos olhos atualizado: %d", (int)state);
}

/*
 * DOC — Recebe expressões textuais do firmware.
 * Mantém os nomes usados anteriormente pelo DOC.
 */
void CustomEyes_SetExpression(const char *expression)
{
    if (expression == NULL || expression[0] == '\0') {
        return;
    }

    /* DOC — Normaliza para minúsculas. */
    char normalized[32];
    size_t i = 0;

    while (expression[i] != '\0' && i < sizeof(normalized) - 1) {
        normalized[i] =
            (char)tolower((unsigned char)expression[i]);
        i++;
    }

    normalized[i] = '\0';

    /* DOC — Mapeamento dos nomes recebidos para os presets visuais. */
    if (strcmp(normalized, "idle") == 0 ||
        strcmp(normalized, "neutral") == 0 ||
        strcmp(normalized, "normal") == 0 ||
        strcmp(normalized, "robot_2") == 0 ||
        strcmp(normalized, "robot-2") == 0) {

        current_state = EYES_STATE_IDLE;
        current_expression = DOC_EXPR_NEUTRAL;

    } else if (strcmp(normalized, "listening") == 0 ||
               strcmp(normalized, "listen") == 0) {

        current_state = EYES_STATE_LISTENING;
        current_expression = DOC_EXPR_LISTENING;

    } else if (strcmp(normalized, "speaking") == 0 ||
               strcmp(normalized, "talking") == 0) {

        current_state = EYES_STATE_SPEAKING;
        current_expression = DOC_EXPR_SPEAKING;

    } else if (strcmp(normalized, "thinking") == 0 ||
               strcmp(normalized, "think") == 0) {

        current_state = EYES_STATE_THINKING;
        current_expression = DOC_EXPR_THINKING;

    } else if (strcmp(normalized, "happy") == 0 ||
               strcmp(normalized, "excited") == 0 ||
               strcmp(normalized, "glee") == 0) {

        current_expression = DOC_EXPR_HAPPY;

    } else if (strcmp(normalized, "love") == 0 ||
               strcmp(normalized, "heart") == 0 ||
               strcmp(normalized, "adoring") == 0) {

        current_expression = DOC_EXPR_LOVE;

    } else if (strcmp(normalized, "dizzy") == 0 ||
               strcmp(normalized, "spiral") == 0) {

        current_expression = DOC_EXPR_DIZZY;

    } else if (strcmp(normalized, "wink") == 0 ||
               strcmp(normalized, "winking") == 0) {

        current_expression = DOC_EXPR_WINK;

    } else if (strcmp(normalized, "furious") == 0 ||
               strcmp(normalized, "rage") == 0) {

        current_expression = DOC_EXPR_FURIOUS;

    } else if (strcmp(normalized, "smile") == 0 ||
               strcmp(normalized, "smiling") == 0) {

        current_expression = DOC_EXPR_SMILE;

    } else if (strcmp(normalized, "smug") == 0) {

        current_expression = DOC_EXPR_SMUG;

    } else if (strcmp(normalized, "star_eyes") == 0 ||
               strcmp(normalized, "star-eyes") == 0 ||
               strcmp(normalized, "star_struck") == 0) {

        current_expression = DOC_EXPR_STAR_EYES;

    } else if (strcmp(normalized, "cool") == 0) {

        current_expression = DOC_EXPR_COOL;

    } else if (strcmp(normalized, "cheeky") == 0 ||
               strcmp(normalized, "playful") == 0) {

        current_expression = DOC_EXPR_CHEEKY;

    } else if (strcmp(normalized, "derp") == 0 ||
               strcmp(normalized, "goofy") == 0) {

        current_expression = DOC_EXPR_DERP;

    } else if (strcmp(normalized, "sad") == 0 ||
               strcmp(normalized, "cry") == 0 ||
               strcmp(normalized, "worried") == 0) {

        current_expression = DOC_EXPR_SAD;

    } else if (strcmp(normalized, "angry") == 0 ||
               strcmp(normalized, "anger") == 0) {

        current_expression = DOC_EXPR_ANGRY;

    } else if (strcmp(normalized, "surprised") == 0 ||
               strcmp(normalized, "shocked") == 0 ||
               strcmp(normalized, "surprise") == 0) {

        current_expression = DOC_EXPR_SURPRISED;

    } else if (strcmp(normalized, "sleepy") == 0 ||
               strcmp(normalized, "sleeping") == 0 ||
               strcmp(normalized, "sleep") == 0) {

        current_expression = DOC_EXPR_SLEEPY;

    } else if (strcmp(normalized, "confused") == 0 ||
               strcmp(normalized, "confusion") == 0 ||
               strcmp(normalized, "uncertain") == 0) {

        current_expression = DOC_EXPR_CONFUSED;

    } else {
        /* DOC — Expressão desconhecida: retorna ao visual neutro. */
        ESP_LOGW(
            TAG,
            "Expressão não reconhecida: %s; usando neutral",
            normalized
        );

        current_expression = DOC_EXPR_NEUTRAL;
    }

    apply_expression_targets();

    ESP_LOGI(TAG, "Expressão visual: %s", normalized);
}

/* DOC INÍCIO — Alternar a posição dos olhos conforme o estado da câmera. */
void CustomEyes_SetCameraMode(bool enabled)
{
    camera_mode_active = enabled;
    /* O timer reposiciona os olhos em até 33 ms, sem recriar objetos LVGL. */
}
/* DOC FIM — Modo de posição dos olhos para a câmera. */

/* DOC — Solicita uma piscada manual. */
void CustomEyes_BlinkNow(void)
{
    if (!is_blinking) {
        is_blinking = true;
        blink_progress = 0.0f;
    }
}

/*
 * DOC — Encerra o timer e remove os objetos criados pelo módulo.
 * Chame antes de destruir o parent que contém os olhos.
 */
void CustomEyes_Deinit(void)
{
    if (anim_timer != NULL) {
        lv_timer_del(anim_timer);
        anim_timer = NULL;
    }

    if (left_eye.shape != NULL) {
        lv_obj_del(left_eye.shape);
    }

    if (left_eye.highlight != NULL) {
        lv_obj_del(left_eye.highlight);
    }

    if (right_eye.shape != NULL) {
        lv_obj_del(right_eye.shape);
    }

    if (right_eye.highlight != NULL) {
        lv_obj_del(right_eye.highlight);
    }

    /* DOC — Libera também os objetos decorativos criados pelo módulo. */
    lv_obj_t *effects[] = {heart_left, heart_right, tear_left, tear_right,
                           spiral_left, spiral_right, anger_mark,
                           star_left, star_right};
    for (size_t i = 0; i < sizeof(effects) / sizeof(effects[0]); i++) {
        if (effects[i] != NULL) lv_obj_del(effects[i]);
    }

    heart_left = heart_right = NULL;
    tear_left = tear_right = NULL;
    spiral_left = spiral_right = NULL;
    anger_mark = star_left = star_right = NULL;

    memset(&left_eye, 0, sizeof(left_eye));
    memset(&right_eye, 0, sizeof(right_eye));

    parent_ref = NULL;
    is_blinking = false;
    blink_progress = 0.0f;

    ESP_LOGI(TAG, "Motor de olhos desinicializado");
}
