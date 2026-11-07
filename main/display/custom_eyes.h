
/*
 * DOC — Módulo de olhos animados.
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

// DOC — Alterna os olhos entre a faixa superior (câmera aberta) e o centro da tela.
void CustomEyes_SetCameraMode(bool enabled);

// DOC — Mantém os olhos na faixa superior (y=0..31), usada pelo painel de apps.
void CustomEyes_SetTopBand(bool enabled);

// DOC — Solicita uma piscada imediata.
void CustomEyes_BlinkNow(void);

// DOC — Encerra o temporizador e libera os objetos do módulo.
void CustomEyes_Deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_EYES_H */
