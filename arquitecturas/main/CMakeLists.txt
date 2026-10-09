idf_component_register(SRCS "arquitecturas_main.c"
                       INCLUDE_DIRS ""
                       REQUIRES esp_driver_gpio esp_timer)

# Arquitectura seleccionada al compilar:  idf.py -DARQ=4 build
#   1 round-robin puro · 2 round-robin con interrupciones
#   3 encolado de funciones · 4 RTOS
if(NOT DEFINED ARQ)
    set(ARQ 1)
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE ARQ=${ARQ})
message(STATUS "arquitecturas: compilando con ARQ=${ARQ}")
