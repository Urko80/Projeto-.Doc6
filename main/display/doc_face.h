/*
 * DOC — Rosto "Mochi" (olhos + boca) em C puro, sem dependência do LVGL.
 * Arquivo: main/display/doc_face.h
 *
 * Porta para C do sketch Arduino "DOC AI - Mochi face" (18 expressões).
 * O desenho é feito em um buffer RGB565 de 128x160 (tela ST7735 do DOC).
 * As coordenadas das expressões continuam no espaço virtual 240x280 do
 * sketch original; uma transformação (escala + deslocamento) leva esse
 * espaço para a tela, e é ela que faz os olhos subirem para a faixa
 * superior quando um app (câmera, rádio, cronômetro...) é aberto.
 *
 * Layout (retrato 128x160):
 *
 *   rosto inteiro (sem app)        app aberto
 *   +----------------+             +----------------+  y = 0
 *   |                |             |    O     O     |  faixa 128x32
 *   |    O     O     |             +----------------+  y = 32
 *   |      ___       |             ||              ||
 *   |                |             ||  área do app ||  128x128
 *   +----------------+             ||______________||  (boca = contorno)
 *                                  +----------------+  y = 160
 */

#ifndef DOC_FACE_INCLUDED_H
#define DOC_FACE_INCLUDED_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOC_FACE_W        128
#define DOC_FACE_H        160
#define DOC_FACE_BAND_H    32   /* altura da faixa dos olhos com app aberto */
#define DOC_FACE_APP_Y     32   /* início da área do app */
#define DOC_FACE_APP_H    128   /* altura da área do app (128x128) */

/* DOC — As 18 expressões do sketch Mochi. */
typedef enum {
    DOC_EMO_CUSTOM = 0,
    DOC_EMO_ALIVE,        /* padrão */
    DOC_EMO_HAPPY,
    DOC_EMO_SAD,
    DOC_EMO_ANGRY,
    DOC_EMO_LOVE,
    DOC_EMO_DIZZY,
    DOC_EMO_WINK,
    DOC_EMO_CONFUSED,
    DOC_EMO_SLEEPY,
    DOC_EMO_DEEP_SLEEP,
    DOC_EMO_FURIOUS,
    DOC_EMO_SMILE,
    DOC_EMO_SMUG,
    DOC_EMO_STAR_EYES,
    DOC_EMO_COOL,
    DOC_EMO_CHEEKY,
    DOC_EMO_DERP,
    DOC_EMO_COUNT
} doc_emotion_t;

/* DOC — Retângulo do contorno que nasce da boca (coordenadas de tela). */
typedef struct {
    int x, y, w, h;
    int radius;
    int border;        /* espessura da borda, em pixels */
    uint8_t fill_opa;  /* 255 = preenchido (boca), 0 = só contorno */
} doc_frame_t;

/* Cores em RGB565 nativo do LVGL. Zera o estado e a animação. */
void doc_face_init(uint16_t face_rgb565, uint16_t bg_rgb565);

void doc_face_set_emotion(doc_emotion_t emotion);
doc_emotion_t doc_face_get_emotion(void);

/* Viés fixo do olhar em unidades virtuais (-4..4). Usado em ouvir/pensar. */
void doc_face_set_gaze_bias(bool enabled, float x, float y);

/* Boca abrindo e fechando enquanto o assistente fala. */
void doc_face_set_talking(bool talking);

/* true: olhos sobem para a faixa superior e a boca vira o contorno do app. */
void doc_face_set_app_mode(bool enabled);
bool doc_face_get_app_mode(void);

void doc_face_blink_now(void);

/* Avança a animação. now_ms é um relógio monotônico em milissegundos. */
void doc_face_tick(uint32_t now_ms);

/* 0 = rosto inteiro, 1 = olhos na faixa e contorno no lugar da boca. */
float doc_face_app_progress(void);

/*
 * Desenha o quadro em fb (RGB565, stride_px pixels por linha).
 * Retorna quantas linhas foram desenhadas: 160 no rosto inteiro e durante
 * a transição, 32 quando o app está aberto e a transição terminou (o resto
 * da tela pertence ao app).
 */
int doc_face_render(uint16_t *fb, int stride_px);

/*
 * Retângulo do contorno nesta etapa da animação. Retorna false quando o
 * contorno não deve aparecer (rosto inteiro).
 */
bool doc_face_get_frame(doc_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* DOC_FACE_INCLUDED_H */
