# Declara o componente principal do firmware experimental.
# Dependências explícitas:
# - esp_driver_rmt: API RMT nova (TX e encoder de bytes).
# - esp_driver_gpio: tipos e definições de GPIO usados na configuração do canal.
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_rmt esp_driver_gpio
)
