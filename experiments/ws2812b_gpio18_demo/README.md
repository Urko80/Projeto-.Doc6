# Projeto .DOC — teste experimental WS2812B no GPIO18

## Objetivo

Firmware ESP-IDF independente para testar a barra WS2812B de 8 LEDs usando GPIO18 da ESP32-S3-CAM N16R8.

**Esta experiência não altera nem integra o firmware principal do DOC.** O diretório está fora dos componentes do build principal e só deve ser compilado como projeto ESP-IDF separado.

## Justificativa do GPIO

No mapa atual da placa:

- GPIO18 = `CAMERA_PIN_D5`.
- A câmera OV3660 utiliza GPIO18 como linha de dados D5 quando inicializada.
- A aplicação principal abre a câmera sob demanda, por meio de `OpenCamera()`.

Neste firmware de demonstração, a câmera não é inicializada. Portanto, o GPIO18 fica reservado para a saída de dados da barra.

**Não utilizar a câmera e a barra WS2812B simultaneamente no GPIO18.** A integração futura exigirá controle explícito de exclusão: desligar/liberar a câmera antes de habilitar a barra e vice-versa.

## Ligações

| Barra WS2812B | ESP32-S3-CAM / alimentação |
|---|---|
| DIN | GPIO18, preferencialmente através de buffer 74AHCT125 |
| GND | GND comum |
| +5V | Fonte regulada externa de 5 V, dimensionada para a barra |

Recomendações elétricas:

1. Não alimentar a barra pelo pino 3V3 da placa.
2. Unir o GND da fonte de 5 V ao GND da ESP32-S3.
3. Usar buffer lógico 74AHCT125 para converter o sinal de 3,3 V para 5 V.
4. Colocar resistor em série de aproximadamente 330 Ω na linha de dados, próximo ao DIN.
5. Dimensionar a fonte considerando que 8 LEDs podem exigir até aproximadamente 480 mA em branco e brilho máximo; o firmware deste teste limita o brilho.

Faça as conexões com a alimentação desligada e confira a identificação de DIN, +5V e GND na barra.

## Compilação isolada

Com o ambiente ESP-IDF configurado:

```bash
cd experiments/ws2812b_gpio18_demo
idf.py set-target esp32s3
idf.py build
```

Para gravar e monitorar, conectar a placa pela USB-C UART e usar a porta serial correspondente:

```bash
idf.py -p PORTA_SERIAL flash monitor
```

Substitua `PORTA_SERIAL` pela porta efetivamente identificada no computador.

## Resultado esperado

- Sequência de vermelho, verde, azul e apagado.
- Varredura laranja de um LED por vez, do primeiro ao oitavo.
- Repetição contínua da sequência.

## Pontos de ajuste no código

Arquivo: `main/main.c`

- `LED_GPIO`: GPIO de dados (GPIO18).
- `LED_COUNT`: quantidade de LEDs.
- `LED_BRIGHTNESS`: brilho de teste.
- `WS_T0H_TICKS`, `WS_T0L_TICKS`, `WS_T1H_TICKS`, `WS_T1L_TICKS`: temporização do protocolo.
- `app_main()`: sequência de demonstração.

## Escopo e validação

- Não altera `main/application.cc`, `config.h` ou `doc_ai_s3cam.cc` da aplicação principal.
- Não modifica a pinagem definitiva do projeto.
- Não integra o driver WS2812B à inicialização da placa.
- Não foi validado em hardware nesta etapa.
- A PR deve permanecer em Draft até a compilação e o teste físico serem concluídos.
