
/*
 * DOC — Módulo de olhos e boca animados (rosto Mochi, 18 expressões).
 * Arquivo: main/display/custom_eyes.h
 * Função: declarar os estados e as funções públicas do módulo.
 */

#ifndef CUSTOM_EYES_H
#define CUSTOM_EYES_H

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// DOC — Estados disponíveis para a animação.
typedef enum {
    EYES_STATE_IDLE = 0,
    EYES_STATE_LISTENING,
    EYES_STATE_SPEAKING,
    EYES_STATE_THINKING,
    EYES_STATE_COUNT
} eyes_state_t;

// DOC — Cria os olhos como objetos filhos do objeto LVGL informado.
void CustomEyes_Init(lv_obj_t *parent);

// DOC — Troca o comportamento da animação.
void CustomEyes_SetState(eyes_state_t state);

// DOC — Recebe uma expressão textual, como "happy", "sad" ou "neutral".
void CustomEyes_SetExpression(const char *expression);

// DOC — Modo "app": qualquer função que use a área 128x128 (câmera, rádio,
// cronômetro, CubeWorld...) chama CustomEyes_SetAppMode(true). Os olhos sobem
// para a faixa y=0..31 e a boca vira o contorno da área do app (y=32..159).
// Ao fechar o app, chame CustomEyes_SetAppMode(false).
void CustomEyes_SetAppMode(bool enabled);

// DOC — Compatibilidade: a câmera usa o mesmo modo "app".
void CustomEyes_SetCameraMode(bool enabled);

// DOC — Área livre para os apps com o modo app ligado (tela 128x160).
#define CUSTOM_EYES_APP_AREA_X 0
#define CUSTOM_EYES_APP_AREA_Y 32
#define CUSTOM_EYES_APP_AREA_W 128
#define CUSTOM_EYES_APP_AREA_H 128

// DOC — Solicita uma piscada imediata.
void CustomEyes_BlinkNow(void);

// DOC — Encerra o temporizador e libera os objetos do módulo.
void CustomEyes_Deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_EYES_H */
