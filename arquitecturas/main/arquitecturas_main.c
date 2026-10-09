/*
 * arquitecturas — Clase 4 (Sistemas Embebidos, UCU 2026)
 *
 * La MISMA aplicación escrita en las CUATRO arquitecturas de firmware de la
 * Sección 4, seleccionables al compilar. El programa mide su propia latencia
 * de respuesta, así que la comparación no es de opinión: es un número.
 *
 *      ARQ=1  Round-Robin puro       bucle único, todo por sondeo
 *      ARQ=2  Round-Robin + ISR      bucle único + interrupción que anota
 *      ARQ=3  Encolado de funciones  la ISR encola punteros a función
 *      ARQ=4  RTOS                   tareas de FreeRTOS con prioridades
 *
 * El trabajo a repartir es siempre el mismo:
 *
 *      A) parpadeo del LED RGB cada 500 ms          (periódico, poco urgente)
 *      B) un cálculo largo de ~200 ms               (el que estropea todo)
 *      C) atender el botón BOOT lo antes posible    (urgente: se mide)
 *
 * Lo que hay que ver: la latencia MAXIMA de C es de ~200 ms en la
 * arquitectura 1 y de decenas de µs en la 4, con exactamente el mismo trabajo
 * por delante. La diferencia no está en el código de C: está en quién decide
 * cuándo se ejecuta.
 *
 * Cómo elegir la arquitectura, sin tocar el fuente:
 *
 *      idf.py -DARQ=1 build flash monitor
 *
 * Hardware:
 *   - Botón BOOT en GPIO0, a masa, con pull-up interno: activo en BAJO.
 *   - LED RGB direccionable en GPIO45 (requiere el JUMPER RGB colocado).
 *   - No necesita ninguna placa de extensión.
 *   - Target: esp32s2.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"

/* --- Selección de arquitectura ------------------------------------------ */

#define ARQ_ROUND_ROBIN         1
#define ARQ_RR_INTERRUPCIONES   2
#define ARQ_COLA_FUNCIONES      3
#define ARQ_RTOS                4

/* Lo define main/CMakeLists.txt a partir de -DARQ=n; si no, vale la 1. */
#ifndef ARQ
#define ARQ  ARQ_ROUND_ROBIN
#endif

#if ARQ < ARQ_ROUND_ROBIN || ARQ > ARQ_RTOS
#error "ARQ tiene que valer 1 (round-robin), 2 (RR+ISR), 3 (cola) o 4 (RTOS)"
#endif

/* --- Parámetros comunes a las cuatro ------------------------------------ */

#define BOTON_GPIO        GPIO_NUM_0
#define RGB_LED_GPIO      GPIO_NUM_45
#define RGB_LED_COUNT     1

/* Trabajo A: periodo del parpadeo. */
#define PARPADEO_MS       500
/* Trabajo B: duración del cálculo largo. Es el número que manda: en
 * round-robin, la peor latencia del trabajo C se parece mucho a este. */
#define CALCULO_MS        200
/* En la arquitectura 3 ese cálculo se TROCEA en pasos de esta duración, que
 * es lo que permite que la cola avance entre paso y paso. */
#define PASO_MS           10
/* Ventana de guarda del antirrebote. Un pulsador mecánico rebota 1-20 ms. */
#define REBOTE_US         50000
#define COLA_LARGO        8

static const char *TAG = "arquitecturas";

static const char *NOMBRE_ARQ[] = {
    [ARQ_ROUND_ROBIN]       = "1. Round-Robin puro (sondeo, sin interrupciones)",
    [ARQ_RR_INTERRUPCIONES] = "2. Round-Robin con interrupciones",
    [ARQ_COLA_FUNCIONES]    = "3. Encolado de funciones (function-queue)",
    [ARQ_RTOS]              = "4. RTOS (FreeRTOS: tareas y prioridades)",
};

/* --- El LED ------------------------------------------------------------- */

static led_strip_handle_t s_led;

static void led(uint8_t r, uint8_t g, uint8_t b)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(s_led, 0, r, g, b));
    ESP_ERROR_CHECK(led_strip_refresh(s_led));
}

static void configurar_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = RGB_LED_COUNT,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, /* 10 MHz */
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led));
    ESP_ERROR_CHECK(led_strip_clear(s_led));
}

/* --- Lo que la ISR comparte con el resto del programa -------------------- */

/* En las arquitecturas 2, 3 y 4 la ISR es parte del diseño, y todo lo que
 * sigue es el precio de haberla introducido. En la 1 también hay una ISR,
 * pero solo como INSTRUMENTO DE MEDIDA: anota cuándo ocurrió el flanco y el
 * programa no la usa para decidir nada (ver isr_cronometro). */

/* Instante exacto del flanco, tomado dentro de la ISR. Un escritor (la ISR),
 * un lector (el bucle o la tarea): en Xtensa la lectura de un int64 NO es
 * atómica, pero aquí el lector solo la mira cuando la bandera ya está puesta,
 * y la bandera se pone después de escribir la marca. */
static volatile int64_t  s_marca_evento_us;
static volatile uint32_t s_rebotes;

/* Antirrebote hecho DENTRO de la ISR: los rebotes ni siquiera llegan al resto
 * del programa. Es trabajo en contexto de interrupción, pero son cuatro
 * instrucciones y evita ráfagas de eventos falsos. */
static bool IRAM_ATTR flanco_valido(int64_t ahora)
{
    static int64_t ultimo_us;

    if (ahora - ultimo_us < REBOTE_US) {
        s_rebotes++;
        return false;
    }
    ultimo_us = ahora;
    return true;
}

/* --- Estadística de latencia -------------------------------------------- */

/* Latencia = desde que el evento OCURRE hasta que se ATIENDE. El instante del
 * evento lo toma siempre una ISR en el flanco: la del propio diseño en las
 * arquitecturas 2, 3 y 4, y el cronómetro en la 1. El de atención, justo
 * antes de responder. */
typedef struct {
    uint32_t n;
    int64_t  suma_us;
    int64_t  min_us;
    int64_t  max_us;
} latencia_t;

static latencia_t s_lat = { .min_us = INT64_MAX };

static void informar_latencia(int64_t us)
{
    s_lat.n++;
    s_lat.suma_us += us;
    if (us < s_lat.min_us) { s_lat.min_us = us; }
    if (us > s_lat.max_us) { s_lat.max_us = us; }

    const int64_t media = s_lat.suma_us / s_lat.n;

    ESP_LOGI(TAG, "BOTON #%" PRIu32 "  latencia %7" PRId64 " us  "
                  "(min %" PRId64 "  media %" PRId64 "  MAX %" PRId64 ")  "
                  "rebotes %" PRIu32,
             s_lat.n, us, s_lat.min_us, media, s_lat.max_us, s_rebotes);
}

/* --- Los tres trabajos, idénticos en las cuatro arquitecturas ------------ */

/* Trabajo A: alterna el color del LED. Barato, pero tiene que ocurrir cada
 * PARPADEO_MS o el parpadeo se ve irregular. */
static void trabajo_parpadeo(void)
{
    static bool encendido;

    encendido = !encendido;
    led(0, encendido ? 24 : 0, 0);
}

/* Sumidero del cálculo. Si el resultado del bucle no se usara, el compilador
 * lo eliminaría entero con optimización (-O2 = "Optimize for performance" en
 * menuconfig): no cambia nada observable. Ser `volatile` obliga a:
 *  - escribirlo al salir -> hay que calcular `acc`, luego el bucle se ejecuta;
 *  - leerlo al entrar    -> el valor inicial es desconocido al compilar, así
 *    que el resultado no se puede precalcular.
 * El `volatile` va aquí y no en `acc` para que el bucle siga en registros. */

static volatile uint32_t s_sumidero = 1;

/* Trabajo B: el cálculo largo. Un bucle de aritmética entera dimensionado
 * para tardar del orden de CALCULO_MS. No se trata de optimizarlo: el
 * problema de este ejemplo es, precisamente, que hay trabajo que tarda. */
static void trabajo_calculo(uint32_t iteraciones)
{
    uint32_t acc = s_sumidero;

    for (uint32_t i = 0; i < iteraciones; i++) {
        acc = acc * 1664525u + 1013904223u;   /* congruencial lineal */
        acc ^= acc >> 13;
    }
    s_sumidero = acc | 1u;
}

/* Cuántas iteraciones equivalen a 1 ms en ESTA placa. Se calibra al arrancar
 * en vez de suponerlo: depende de la frecuencia de CPU y de si el código se
 * está ejecutando desde flash (con caché) o desde IRAM. */
static uint32_t s_iter_por_ms;

static void calibrar_calculo(void)
{
    const uint32_t muestra = 200000;
    const int64_t t0 = esp_timer_get_time();

    trabajo_calculo(muestra);

    const int64_t us = esp_timer_get_time() - t0;
    s_iter_por_ms = (us > 0) ? (uint32_t)((int64_t)muestra * 1000 / us) : muestra;

    ESP_LOGI(TAG, "Calibracion: %" PRIu32 " iteraciones = 1 ms  "
                  "(la muestra de %" PRIu32 " tardo %" PRId64 " us)",
             s_iter_por_ms, muestra, us);
}

/* Trabajo C: la respuesta al botón. Lo único que hace es medir e informar;
 * lo interesante no es qué hace, sino CUANDO consigue ejecutarse. */
static void trabajo_boton(int64_t marca_evento_us)
{
    const int64_t latencia = esp_timer_get_time() - marca_evento_us;

    led(0, 0, 40);                     /* destello azul: "te he oido" */
    informar_latencia(latencia);
}

/* --- Configuración del botón -------------------------------------------- */

static void configurar_boton(gpio_int_type_t interrupcion)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOTON_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* nivel alto en reposo */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = interrupcion,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static void instalar_isr(gpio_isr_t isr)
{
    configurar_boton(GPIO_INTR_NEGEDGE);
    /* La ISR global del driver se instala una sola vez en todo el programa;
     * después se registra un manejador por pin. */
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BOTON_GPIO, isr, NULL));
}

/* ======================================================================== */
#if ARQ == ARQ_ROUND_ROBIN
/* ------------------------------------------------------------------------
 * 1. ROUND-ROBIN PURO
 *
 * Un solo bucle que mira todo por turno. Sin interrupciones, sin prioridades
 * y sin concurrencia: por eso no hay ni un dato compartido que proteger, y
 * por eso es la arquitectura más fácil de razonar y de certificar.
 *
 * El precio: el botón solo se descubre cuando el bucle vuelve a mirarlo, y
 * para volver a mirarlo tiene que haber terminado antes el cálculo largo. La
 * peor latencia es LA DURACION DE UNA VUELTA COMPLETA, ~CALCULO_MS.
 *
 * La única interrupción de esta versión es el cronómetro que mide esa
 * latencia. No es parte de la arquitectura: quitándola, el programa hace
 * exactamente lo mismo, solo que ya no sabe medirse.
 * ------------------------------------------------------------------------ */

/* El cronómetro: anota el instante del flanco y nada más. Es el equivalente
 * a enganchar un osciloscopio al pin. Hace falta porque el sondeo sabe QUE se
 * pulsó, pero no CUANDO: si la marca se tomase al descubrir la pulsación, la
 * "latencia" sería lo que tardan dos lecturas seguidas del reloj, ~1 µs, y la
 * espera de la vuelta, que es justo lo que se quiere ver, quedaría fuera. */
static void IRAM_ATTR isr_cronometro(void *arg)
{
    const int64_t ahora = esp_timer_get_time();

    if (flanco_valido(ahora)) {
        s_marca_evento_us = ahora;
    }
}

/* Detección por SONDEO: hay pulsación cuando el nivel pasa de alto a bajo.
 * Solo la ve quien la mira, y solo cuando la mira. */
static bool boton_pulsado_por_sondeo(void)
{
    static int nivel_anterior = 1;
    static int64_t ultimo_us;

    const int nivel = gpio_get_level(BOTON_GPIO);
    const bool flanco = (nivel_anterior == 1) && (nivel == 0);
    nivel_anterior = nivel;

    if (!flanco) {
        return false;
    }
    const int64_t ahora = esp_timer_get_time();
    if (ahora - ultimo_us < REBOTE_US) {
        return false;                  /* rebote */
    }
    ultimo_us = ahora;
    return true;
}

static void bucle_principal(void)
{
    int64_t proximo_parpadeo_us = esp_timer_get_time();

    instalar_isr(isr_cronometro);      /* solo para medir: ver arriba */

    while (1) {
        /* --- turno 1: el botón ---------------------------------------- */
        if (boton_pulsado_por_sondeo()) {
            /* El sondeo descubre la pulsación; cuándo ocurrió lo dice el
             * cronómetro. La lectura del int64 no es atómica, pero mientras
             * el botón sigue pulsado no llegan flancos que la pisen. */
            trabajo_boton(s_marca_evento_us);
        }

        /* --- turno 2: el parpadeo ------------------------------------- */
        const int64_t ahora = esp_timer_get_time();
        if (ahora >= proximo_parpadeo_us) {
            proximo_parpadeo_us = ahora + (int64_t)PARPADEO_MS * 1000;
            trabajo_parpadeo();
        }

        /* --- turno 3: el cálculo largo -------------------------------- */
        trabajo_calculo(s_iter_por_ms * CALCULO_MS);
    }
}

/* ======================================================================== */
#elif ARQ == ARQ_RR_INTERRUPCIONES
/* ------------------------------------------------------------------------
 * 2. ROUND-ROBIN CON INTERRUPCIONES
 *
 * La ISR no hace el trabajo: solo ANOTA que ocurrió y CUANDO ocurrió. El
 * bucle sigue siendo el mismo de antes.
 *
 * Lo que se gana: el instante del evento se captura con precisión de
 * microsegundos y ninguna pulsación se pierde aunque el bucle esté ocupado.
 * Lo que NO se gana: la RESPUESTA sigue esperando su turno, así que la
 * latencia máxima sigue siendo ~CALCULO_MS. Es la distinción clave entre
 * LATENCIA DE INTERRUPCION y LATENCIA DE RESPUESTA.
 *
 * Y aparece el problema que no existía en la arquitectura 1: s_hay_evento lo
 * escribe la ISR y lo lee el bucle. Es un DATO COMPARTIDO.
 * ------------------------------------------------------------------------ */

static volatile bool s_hay_evento;

static void IRAM_ATTR isr_boton(void *arg)
{
    const int64_t ahora = esp_timer_get_time();

    if (!flanco_valido(ahora)) {
        return;
    }
    s_marca_evento_us = ahora;
    s_hay_evento = true;               /* se escribe DESPUES de la marca */
}

static void bucle_principal(void)
{
    int64_t proximo_parpadeo_us = esp_timer_get_time();

    instalar_isr(isr_boton);

    while (1) {
        /* --- turno 1: ¿anotó algo la ISR? ----------------------------- */
        if (s_hay_evento) {
            const int64_t marca = s_marca_evento_us;
            s_hay_evento = false;      /* se borra DESPUES de leer la marca */
            trabajo_boton(marca);
            /* Cuidado: si llega un flanco entre la lectura y el borrado, ese
             * evento se pierde. Con una bandera de un bit no hay forma de
             * arreglarlo del todo; hace falta un contador o una cola. */
        }

        /* --- turno 2: el parpadeo ------------------------------------- */
        const int64_t ahora = esp_timer_get_time();
        if (ahora >= proximo_parpadeo_us) {
            proximo_parpadeo_us = ahora + (int64_t)PARPADEO_MS * 1000;
            trabajo_parpadeo();
        }

        /* --- turno 3: el cálculo largo -------------------------------- */
        trabajo_calculo(s_iter_por_ms * CALCULO_MS);
    }
}

/* ======================================================================== */
#elif ARQ == ARQ_COLA_FUNCIONES
/* ------------------------------------------------------------------------
 * 3. ENCOLADO DE FUNCIONES (function-queue scheduling)
 *
 * La ISR encola un PUNTERO A FUNCION en vez de levantar una bandera. El
 * bucle principal desencola y ejecuta, pero ya no en orden fijo: la agenda
 * está ORDENADA POR PRIORIDAD, así que el trabajo urgente adelanta al resto.
 *
 * Es medio RTOS hecho a mano: hay prioridades, pero NO hay desalojo. Una
 * función que empieza, termina. Por eso el cálculo largo hay que TROCEARLO:
 * cada paso se reencola a sí mismo hasta acabar el lote.
 *
 * La latencia máxima deja de ser la vuelta entera y pasa a ser LA FUNCION MAS
 * LARGA DE LA AGENDA, ~PASO_MS. Ese troceado es el coste de diseño de esta
 * arquitectura, y es un coste real: hay que partir el algoritmo a mano y
 * sacar su estado de la pila, porque entre paso y paso la función retorna.
 * ------------------------------------------------------------------------ */

typedef void (*accion_t)(void);

typedef enum {
    PRIO_ALTA = 0,      /* el botón */
    PRIO_MEDIA,         /* el parpadeo */
    PRIO_BAJA,          /* el cálculo troceado */
    PRIO_N
} prioridad_t;

/* Una entrada por nivel: aquí basta con saber si hay trabajo pendiente de
 * cada clase, que es el caso habitual de esta arquitectura. Con varias
 * funciones por nivel sería una cola circular por nivel. */
typedef struct {
    accion_t fn;
    bool     pendiente;
} entrada_t;

static entrada_t s_agenda[PRIO_N];

/* La agenda la escriben DOS flujos: la ISR y el bucle. Hay que protegerla.
 * portMUX_TYPE es el candado de sección crítica de ESP-IDF: en el ESP32-S2,
 * monoprocesador, se reduce a deshabilitar las interrupciones. */
static portMUX_TYPE s_agenda_mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR encolar(prioridad_t p, accion_t fn)
{
    s_agenda[p].fn = fn;
    s_agenda[p].pendiente = true;
}

/* Saca la pendiente más prioritaria y la marca como tomada, todo dentro de la
 * sección crítica. Si se leyera fuera, un flanco que llegase entre la lectura
 * y el borrado dejaría un evento sin atender. */
static accion_t tomar_siguiente(void)
{
    accion_t fn = NULL;

    portENTER_CRITICAL(&s_agenda_mux);
    for (int p = 0; p < PRIO_N; p++) {
        if (s_agenda[p].pendiente) {
            s_agenda[p].pendiente = false;
            fn = s_agenda[p].fn;
            break;
        }
    }
    portEXIT_CRITICAL(&s_agenda_mux);
    return fn;
}

/* Estado del cálculo troceado. Como la función termina y retorna en cada
 * paso, su progreso NO puede vivir en una variable local. */
static uint32_t s_iter_restantes;

static void accion_boton(void)
{
    trabajo_boton(s_marca_evento_us);
}

static void accion_parpadeo(void)
{
    trabajo_parpadeo();
}

/* Un paso del cálculo largo: hace su trozo, se REENCOLA a sí mismo y
 * devuelve el control. Entre paso y paso el despachador puede colar algo más
 * urgente; ese hueco es toda la diferencia con la arquitectura 1. */
static void accion_calculo_paso(void)
{
    const uint32_t paso = s_iter_por_ms * PASO_MS;
    const uint32_t hacer = (s_iter_restantes < paso) ? s_iter_restantes : paso;

    trabajo_calculo(hacer);
    s_iter_restantes -= hacer;

    if (s_iter_restantes == 0) {
        s_iter_restantes = s_iter_por_ms * CALCULO_MS;   /* siguiente lote */
    }
    portENTER_CRITICAL(&s_agenda_mux);
    encolar(PRIO_BAJA, accion_calculo_paso);
    portEXIT_CRITICAL(&s_agenda_mux);
}

static void IRAM_ATTR isr_boton(void *arg)
{
    const int64_t ahora = esp_timer_get_time();

    if (!flanco_valido(ahora)) {
        return;
    }
    s_marca_evento_us = ahora;
    /* Dentro de una ISR las interrupciones ya están inhibidas: no hace falta
     * tomar el candado, y tomarlo aquí sería un error de diseño. */
    encolar(PRIO_ALTA, accion_boton);
}

static void bucle_principal(void)
{
    int64_t proximo_parpadeo_us = esp_timer_get_time();

    s_iter_restantes = s_iter_por_ms * CALCULO_MS;
    encolar(PRIO_BAJA, accion_calculo_paso);

    instalar_isr(isr_boton);

    while (1) {
        /* El parpadeo es periódico: lo encola el propio bucle al vencer el
         * plazo, no una interrupción. */
        const int64_t ahora = esp_timer_get_time();
        if (ahora >= proximo_parpadeo_us) {
            proximo_parpadeo_us = ahora + (int64_t)PARPADEO_MS * 1000;
            portENTER_CRITICAL(&s_agenda_mux);
            encolar(PRIO_MEDIA, accion_parpadeo);
            portEXIT_CRITICAL(&s_agenda_mux);
        }

        /* Despacho: siempre la pendiente de mayor prioridad. */
        const accion_t fn = tomar_siguiente();
        if (fn != NULL) {
            fn();
        }
    }
}

/* ======================================================================== */
#else /* ARQ == ARQ_RTOS */
/* ------------------------------------------------------------------------
 * 4. RTOS
 *
 * Tres tareas, tres prioridades. Cada una se escribe como si fuera el único
 * programa de la placa: su propio bucle infinito y una llamada bloqueante
 * donde espera lo suyo.
 *
 * Lo que cambia respecto de la 3: el planificador DESALOJA. La tarea del
 * botón no espera a que el cálculo termine ni a que llegue a un punto de
 * troceado; entra en cuanto la cola tiene un dato, en mitad de lo que
 * estuviera haciendo la tarea de baja prioridad.
 *
 * Por eso el cálculo largo vuelve a escribirse de un tirón, sin trocear: el
 * troceado lo hace el planificador, y lo hace a golpe de interrupción de
 * reloj. Ese es el trato del RTOS: unos KB de RAM y un cambio de contexto a
 * cambio de no tener que pensar el troceado nunca más.
 * ------------------------------------------------------------------------ */

static QueueHandle_t s_cola;

static void IRAM_ATTR isr_boton(void *arg)
{
    const int64_t ahora = esp_timer_get_time();

    if (!flanco_valido(ahora)) {
        return;
    }
    BaseType_t hp_task_woken = pdFALSE;
    xQueueSendFromISR(s_cola, &ahora, &hp_task_woken);
    if (hp_task_woken == pdTRUE) {
        /* Cede la CPU al salir de la ISR: la tarea del botón entra YA, sin
         * esperar al siguiente tick. Sin esto se perderían hasta 10 ms. */
        portYIELD_FROM_ISR();
    }
}

/* Prioridad 10: la urgente. Se pasa la vida BLOQUEADA en la cola, que es
 * como una tarea dice "no me des CPU hasta que haya algo para mí". */
static void tarea_boton(void *arg)
{
    int64_t marca;

    while (xQueueReceive(s_cola, &marca, portMAX_DELAY) == pdTRUE) {
        trabajo_boton(marca);
    }
}

/* Prioridad 5: periódica. xTaskDelayUntil cuenta el periodo desde el
 * despertar ANTERIOR, no desde ahora: así el parpadeo no se va retrasando
 * acumulativamente por lo que tarde el cuerpo del bucle. */
static void tarea_parpadeo(void *arg)
{
    TickType_t anterior = xTaskGetTickCount();

    while (1) {
        trabajo_parpadeo();
        xTaskDelayUntil(&anterior, pdMS_TO_TICKS(PARPADEO_MS));
    }
}

/* Prioridad 1: la de relleno. Calcula sin parar y sin trocear nada; el
 * planificador la desaloja en cuanto alguien más importante despierta. */
static void tarea_calculo(void *arg)
{
    while (1) {
        trabajo_calculo(s_iter_por_ms * CALCULO_MS);

        /* Un respiro de un tick para que la tarea IDLE llegue a ejecutarse:
         * es quien libera la memoria de las tareas borradas y quien alimenta
         * al watchdog. Una tarea que nunca cede deja al sistema sin
         * mantenimiento, por baja que sea su prioridad. */
        vTaskDelay(1);
    }
}

static void bucle_principal(void)
{
    s_cola = xQueueCreate(COLA_LARGO, sizeof(int64_t));
    ESP_ERROR_CHECK(s_cola != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    /* La cola tiene que existir antes de habilitar la interrupción que
     * escribe en ella. */
    instalar_isr(isr_boton);

    xTaskCreate(tarea_boton,    "boton",    3072, NULL, 10, NULL);
    xTaskCreate(tarea_parpadeo, "parpadeo", 2560, NULL,  5, NULL);
    xTaskCreate(tarea_calculo,  "calculo",  2560, NULL,  1, NULL);

    /* Y aquí se acaba app_main. No hay bucle principal: el planificador se
     * ocupa. Al retornar, ESP-IDF borra la tarea de app_main y las otras tres
     * siguen corriendo. */
}

#endif /* ARQ */
/* ======================================================================== */

void app_main(void)
{
    ESP_LOGI(TAG, "==========================================================");
    ESP_LOGI(TAG, " Arquitectura %s", NOMBRE_ARQ[ARQ]);
    ESP_LOGI(TAG, "==========================================================");
    ESP_LOGI(TAG, "Trabajo A: parpadeo cada %d ms", PARPADEO_MS);
#if ARQ == ARQ_COLA_FUNCIONES
    ESP_LOGI(TAG, "Trabajo B: calculo de %d ms, troceado en pasos de %d ms",
             CALCULO_MS, PASO_MS);
#else
    ESP_LOGI(TAG, "Trabajo B: calculo de %d ms, de un tiron", CALCULO_MS);
#endif
    ESP_LOGI(TAG, "Trabajo C: responder al boton BOOT (GPIO%d)  <-- se mide",
             BOTON_GPIO);

    configurar_led();
    calibrar_calculo();

    ESP_LOGI(TAG, "Pulsa BOOT unas cuantas veces y mira la latencia MAXIMA.");

    bucle_principal();
}
