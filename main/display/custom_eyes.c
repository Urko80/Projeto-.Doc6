
/*
 * DOC — Olhos e boca "Mochi" para LVGL.
 * Arquivo: main/display/custom_eyes.c
 *
 * O desenho das 18 expressões fica em doc_face.c (C puro). Este arquivo faz
 * a ligação com o LVGL:
 *   - um lv_canvas RGB565 128x160 recebe o rosto a cada 33 ms;
 *   - um lv_obj arredondado ("frame") nasce da boca e vira o contorno da
 *     área 128x128 onde o app (câmera, rádio, cronômetro...) aparece;
 *   - a API pública continua a mesma usada pelo firmware DOC.
 *
 * DOC — Ajustes visuais principais (ver também doc_face.c):
 *   DOC_FACE_COLOR_HEX    : cor dos olhos e da boca.
 *   DOC_PANEL_RB_SWAPPED  : 1 se a tela mostra vermelho e azul trocados.
 *   expression_from_name(): nomes de emoção -> expressão Mochi.
 */

#include "custom_eyes.h"
#include "doc_face.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>

static const char *TAG = "CustomEyes";

/* ============================================================
 * DOC — 1. CONFIGURAÇÕES FÁCEIS DE ALTERAR
 * ============================================================ */

/* DOC — Cor dos olhos e da boca, em RGB (#22C55E = verde do sketch Mochi). */
#define DOC_FACE_COLOR_HEX      0x22C55E

/*
 * DOC — A tela ST7735 do DOC mostrava o azul-ciano 0x43D9F5 dos olhos antigos
 * como amarelo, ou seja, com vermelho e azul trocados. Com 1, a cor acima é
 * trocada antes de ir para a tela e aparece como definida. Se a cor sair
 * errada, troque para 0.
 */
#define DOC_PANEL_RB_SWAPPED    1


/* DOC — Intervalo entre quadros (ms): 33 = cerca de 30 quadros por segundo. */
#define DOC_FACE_FRAME_MS       33

/* ============================================================
 * DOC — 2. VARIÁVEIS DO MÓDULO
 * ============================================================ */

static lv_obj_t *face_canvas = NULL;
static lv_obj_t *app_frame = NULL;
static lv_draw_buf_t *face_buf = NULL;
static lv_timer_t *anim_timer = NULL;

static doc_frame_t last_frame;
static bool last_frame_valid = false;
static int last_rows = DOC_FACE_H;

/* ============================================================
 * DOC — 3. UTILIDADES
 * ============================================================ */

static lv_color_t face_color(void)
{
    uint8_t r = (DOC_FACE_COLOR_HEX >> 16) & 0xFF;
    uint8_t g = (DOC_FACE_COLOR_HEX >> 8) & 0xFF;
    uint8_t b = DOC_FACE_COLOR_HEX & 0xFF;

#if DOC_PANEL_RB_SWAPPED
    return lv_color_make(b, g, r);
#else
    return lv_color_make(r, g, b);
#endif
}

/* DOC — Cria o contorno arredondado que representa a boca/área do app. */
static lv_obj_t *create_app_frame(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);

    lv_obj_set_style_radius(obj, 6, 0);
    lv_obj_set_style_bg_color(obj, face_color(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, face_color(), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);

    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);

    return obj;
}

/* DOC — Atualiza o contorno somente quando algum valor muda. */
static void update_app_frame(void)
{
    doc_frame_t f;

    if (app_frame == NULL) return;

    if (!doc_face_get_frame(&f)) {
        if (last_frame_valid) {
            lv_obj_add_flag(app_frame, LV_OBJ_FLAG_HIDDEN);
            last_frame_valid = false;
        }
        return;
    }

    if (!last_frame_valid) {
        lv_obj_clear_flag(app_frame, LV_OBJ_FLAG_HIDDEN);
    }

    if (!last_frame_valid ||
        f.x != last_frame.x || f.y != last_frame.y ||
        f.w != last_frame.w || f.h != last_frame.h) {
        lv_obj_set_pos(app_frame, f.x, f.y);
        lv_obj_set_size(app_frame, f.w, f.h);
    }
    if (!last_frame_valid || f.radius != last_frame.radius) {
        lv_obj_set_style_radius(app_frame, f.radius, 0);
    }
    if (!last_frame_valid || f.border != last_frame.border) {
        lv_obj_set_style_border_width(app_frame, f.border, 0);
    }
    if (!last_frame_valid || f.fill_opa != last_frame.fill_opa) {
        lv_obj_set_style_bg_opa(app_frame, f.fill_opa, 0);
    }

    last_frame = f;
    last_frame_valid = true;
}

/* ============================================================
 * DOC — 4. TIMER DE ANIMAÇÃO
 * ============================================================ */

static void anim_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (face_canvas == NULL || face_buf == NULL) return;

    doc_face_tick(lv_tick_get());

    int stride_px = (int)(face_buf->header.stride / sizeof(uint16_t));
    int rows = doc_face_render((uint16_t *)face_buf->data, stride_px);

    /* DOC — Só invalida as linhas que mudaram: 32 com app aberto. */
    int invalid_rows = rows > last_rows ? rows : last_rows;
    last_rows = rows;

    lv_area_t area;
    lv_area_set(&area, 0, 0, DOC_FACE_W - 1, invalid_rows - 1);
    lv_obj_invalidate_area(face_canvas, &area);

    update_app_frame();
}

/* ============================================================
 * DOC — 5. NOMES DE EMOÇÃO -> EXPRESSÕES MOCHI
 * ============================================================ */

typedef struct {
    const char *name;
    doc_emotion_t emotion;
} expression_entry_t;

static const expression_entry_t kExpressions[] = {
    /* padrão */
    {"idle", DOC_EMO_ALIVE}, {"neutral", DOC_EMO_ALIVE}, {"normal", DOC_EMO_ALIVE},
    {"robot_2", DOC_EMO_ALIVE}, {"robot-2", DOC_EMO_ALIVE}, {"staticstate", DOC_EMO_ALIVE},
    /* alegria */
    {"happy", DOC_EMO_HAPPY}, {"excited", DOC_EMO_HAPPY}, {"glee", DOC_EMO_HAPPY},
    {"laughing", DOC_EMO_HAPPY}, {"funny", DOC_EMO_HAPPY}, {"joy", DOC_EMO_HAPPY},
    /* carinho */
    {"love", DOC_EMO_LOVE}, {"loving", DOC_EMO_LOVE}, {"kissy", DOC_EMO_LOVE},
    /* tristeza */
    {"sad", DOC_EMO_SAD}, {"cry", DOC_EMO_SAD}, {"crying", DOC_EMO_SAD}, {"worried", DOC_EMO_SAD},
    /* raiva */
    {"angry", DOC_EMO_ANGRY}, {"anger", DOC_EMO_ANGRY},
    {"furious", DOC_EMO_FURIOUS}, {"rage", DOC_EMO_FURIOUS},
    /* surpresa */
    {"surprised", DOC_EMO_STAR_EYES}, {"surprise", DOC_EMO_STAR_EYES}, {"shocked", DOC_EMO_STAR_EYES},
    /* sono */
    {"sleepy", DOC_EMO_SLEEPY}, {"sleeping", DOC_EMO_DEEP_SLEEP}, {"sleep", DOC_EMO_DEEP_SLEEP},
    /* dúvida */
    {"confused", DOC_EMO_CONFUSED}, {"confusion", DOC_EMO_CONFUSED}, {"uncertain", DOC_EMO_CONFUSED},
    {"embarrassed", DOC_EMO_CONFUSED}, {"warning", DOC_EMO_CONFUSED},
    /* outras */
    {"winking", DOC_EMO_WINK}, {"wink", DOC_EMO_WINK},
    {"smile", DOC_EMO_SMILE}, {"relaxed", DOC_EMO_SMILE}, {"delicious", DOC_EMO_SMILE},
    {"smug", DOC_EMO_SMUG}, {"confident", DOC_EMO_SMUG},
    {"cool", DOC_EMO_COOL},
    {"cheeky", DOC_EMO_CHEEKY}, {"playful", DOC_EMO_CHEEKY},
    {"silly", DOC_EMO_DERP}, {"crazy", DOC_EMO_DERP}, {"derp", DOC_EMO_DERP},
    {"dizzy", DOC_EMO_DIZZY},
};

static bool expression_from_name(const char *name, doc_emotion_t *out)
{
    for (size_t i = 0; i < sizeof(kExpressions) / sizeof(kExpressions[0]); i++) {
        if (strcmp(name, kExpressions[i].name) == 0) {
            *out = kExpressions[i].emotion;
            return true;
        }
    }
    return false;
}

/* ============================================================
 * DOC — 6. API PÚBLICA DO MÓDULO
 * ============================================================ */

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

    face_buf = lv_draw_buf_create(DOC_FACE_W, DOC_FACE_H, LV_COLOR_FORMAT_RGB565, 0);
    if (face_buf == NULL) {
        ESP_LOGE(TAG, "Sem memória para o buffer do rosto (%dx%d)", DOC_FACE_W, DOC_FACE_H);
        return;
    }

    /* DOC — O fundo do rosto acompanha o fundo da tela definido pelo tema. */
    lv_color_t bg = lv_obj_get_style_bg_color(parent, LV_PART_MAIN);
    doc_face_init(lv_color_to_u16(face_color()), lv_color_to_u16(bg));

    face_canvas = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(face_canvas, face_buf);
    lv_obj_set_pos(face_canvas, 0, 0);
    lv_obj_clear_flag(face_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(face_canvas, LV_OBJ_FLAG_CLICKABLE);

    /*
     * DOC — O rosto fica atrás de tudo (exceto o fundo): a prévia da câmera,
     * o texto do chat e os apps aparecem por cima dele.
     */
    if (lv_obj_get_child_count(parent) > 2) {
        lv_obj_move_to_index(face_canvas, 1);
    }

    /* DOC — O contorno da área do app fica acima da prévia da câmera. */
    app_frame = create_app_frame(parent);
    last_frame_valid = false;
    last_rows = DOC_FACE_H;

    /* DOC — Desenha o primeiro quadro antes do primeiro ciclo do timer. */
    doc_face_tick(lv_tick_get());
    (void)doc_face_render((uint16_t *)face_buf->data,
                          (int)(face_buf->header.stride / sizeof(uint16_t)));

    anim_timer = lv_timer_create(anim_timer_cb, DOC_FACE_FRAME_MS, NULL);
    if (anim_timer == NULL) {
        ESP_LOGE(TAG, "Não foi possível criar o timer do rosto");
        return;
    }

    ESP_LOGI(TAG, "Rosto Mochi DOC inicializado (%dx%d, 18 expressões)", DOC_FACE_W, DOC_FACE_H);
}

/* DOC — Mantém compatibilidade com os estados de atividade antigos. */
void CustomEyes_SetState(eyes_state_t state)
{
    if (state >= EYES_STATE_COUNT) {
        ESP_LOGW(TAG, "Estado inválido: %d", (int)state);
        return;
    }

    doc_face_set_emotion(DOC_EMO_ALIVE);

    switch (state) {
        case EYES_STATE_LISTENING:
            /* DOC — Olhos atentos, olhando um pouco para cima. */
            doc_face_set_gaze_bias(true, 0.0f, -4.0f);
            doc_face_set_talking(false);
            break;

        case EYES_STATE_SPEAKING:
            /* DOC — A boca abre e fecha enquanto o DOC fala. */
            doc_face_set_gaze_bias(false, 0.0f, 0.0f);
            doc_face_set_talking(true);
            break;

        case EYES_STATE_THINKING:
            /* DOC — Olhar desviado para cima e para o lado. */
            doc_face_set_gaze_bias(true, 4.0f, -4.0f);
            doc_face_set_talking(false);
            break;

        case EYES_STATE_IDLE:
        default:
            doc_face_set_gaze_bias(false, 0.0f, 0.0f);
            doc_face_set_talking(false);
            break;
    }

    ESP_LOGD(TAG, "Estado do rosto atualizado: %d", (int)state);
}

/*
 * DOC — Recebe expressões textuais do firmware.
 * Mantém os nomes usados antes e acrescenta os das 18 expressões Mochi.
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
        normalized[i] = (char)tolower((unsigned char)expression[i]);
        i++;
    }
    normalized[i] = '\0';

    /* DOC — Estados de atividade. */
    if (strcmp(normalized, "listening") == 0 || strcmp(normalized, "listen") == 0) {
        CustomEyes_SetState(EYES_STATE_LISTENING);
        return;
    }
    if (strcmp(normalized, "speaking") == 0 || strcmp(normalized, "talking") == 0) {
        CustomEyes_SetState(EYES_STATE_SPEAKING);
        return;
    }
    if (strcmp(normalized, "thinking") == 0 || strcmp(normalized, "think") == 0) {
        CustomEyes_SetState(EYES_STATE_THINKING);
        return;
    }

    /* DOC — Emoções: voltam o olhar ao normal e fecham a boca. */
    doc_emotion_t emotion = DOC_EMO_ALIVE;

    if (!expression_from_name(normalized, &emotion)) {
        ESP_LOGW(TAG, "Expressão não reconhecida: %s; usando neutral", normalized);
        emotion = DOC_EMO_ALIVE;
    }

    doc_face_set_gaze_bias(false, 0.0f, 0.0f);
    doc_face_set_talking(false);
    doc_face_set_emotion(emotion);

    ESP_LOGI(TAG, "Expressão visual: %s", normalized);
}

/*
 * DOC INÍCIO — Modo "app": qualquer função que use a área 128x128 (câmera,
 * rádio, cronômetro, CubeWorld...) chama CustomEyes_SetAppMode(true). Os
 * olhos sobem para a faixa y=0..31 e a boca vira o contorno da área do app.
 * Ao fechar o app, chame CustomEyes_SetAppMode(false).
 */
void CustomEyes_SetAppMode(bool enabled)
{
    doc_face_set_app_mode(enabled);
}

/* DOC — Compatibilidade: a câmera usa o mesmo modo "app". */
void CustomEyes_SetCameraMode(bool enabled)
{
    CustomEyes_SetAppMode(enabled);
}
/* DOC FIM — Modo app. */

/* DOC — Solicita uma piscada manual. */
void CustomEyes_BlinkNow(void)
{
    doc_face_blink_now();
}

/*
 * DOC — Encerra o timer e remove os objetos criados pelo módulo.
 * Chame antes de destruir o parent que contém o rosto.
 */
void CustomEyes_Deinit(void)
{
    if (anim_timer != NULL) {
        lv_timer_del(anim_timer);
        anim_timer = NULL;
    }

    if (app_frame != NULL) {
        lv_obj_del(app_frame);
        app_frame = NULL;
    }

    if (face_canvas != NULL) {
        lv_obj_del(face_canvas);
        face_canvas = NULL;
    }

    if (face_buf != NULL) {
        lv_draw_buf_destroy(face_buf);
        face_buf = NULL;
    }

    last_frame_valid = false;

    ESP_LOGI(TAG, "Rosto Mochi desinicializado");
}
