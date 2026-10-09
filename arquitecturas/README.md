| Supported Targets | ESP32-S2 |
| ----------------- | -------- |

# arquitecturas — las cuatro arquitecturas de firmware (Clase 4)

La **misma aplicación** escrita en las **cuatro arquitecturas** de la Sección 4, seleccionables al compilar. El programa **mide su propia latencia de respuesta**, así que la comparación entre arquitecturas no es una opinión: es un número que sale por el monitor serie.

```
   idf.py -DARQ=1 build flash monitor      Round-Robin puro
   idf.py -DARQ=2 build flash monitor      Round-Robin con interrupciones
   idf.py -DARQ=3 build flash monitor      Encolado de funciones
   idf.py -DARQ=4 build flash monitor      RTOS (FreeRTOS)
```

## El trabajo a repartir, idéntico en las cuatro

| | Trabajo | Exigencia |
|---|---|---|
| **A** | Parpadear el LED RGB cada **500 ms** | Periódico, tolera retrasos |
| **B** | Un **cálculo de ~200 ms** | Ninguna: solo tiene que avanzar |
| **C** | **Responder al botón BOOT** | Urgente — **es lo que se mide** |

El trabajo B es el villano de la historia. En cuanto hay una tarea larga que no se puede interrumpir, la arquitectura deja de ser un detalle de estilo y pasa a decidir si la aplicación cumple o no cumple.

## Hardware

- **Botón BOOT** en **GPIO0**, ya soldado en la placa: a masa, con *pull-up* interno, activo en **BAJO**.
- **LED RGB direccionable** en **GPIO45**. ⚠️ **Coloca el jumper RGB**: sin él, el LED no recibe señal.
- **No necesita ninguna placa de extensión.** Basta la Kaluga-1 y el cable del puerto UART.
- Target ESP-IDF: **`esp32s2`**.

## Estructura

```
arquitecturas/
├── CMakeLists.txt
├── sdkconfig.defaults          Desactiva el WDT sobre la tarea IDLE (ver abajo)
├── main/
│   ├── CMakeLists.txt          Traduce -DARQ=n al #define ARQ del fuente
│   ├── idf_component.yml       Declara la dependencia espressif/led_strip
│   └── arquitecturas_main.c    Las cuatro arquitecturas, en bloques #if
└── README.md   (este archivo)
```

Las cuatro versiones viven en **un solo fichero**, separadas por `#if ARQ == …`, y **comparten** el LED, la medida de latencia y los tres trabajos. Todo lo que cambia entre una y otra es **quién decide cuándo se ejecuta cada cosa** — que es exactamente lo que queremos comparar.

## Cómo compilar y ejecutar

```bash
idf.py set-target esp32s2          # una sola vez
idf.py -DARQ=1 build flash monitor
```

Para cambiar de arquitectura basta con volver a lanzar `idf.py` con otro `-DARQ=n`: CMake reconfigura y recompila solo lo necesario.

```bash
idf.py -DARQ=4 build flash monitor
```

- Sustituye el puerto con `-p /dev/cu.usbserial-XXXX` si tienes más de una placa (`ls /dev/cu.*` en macOS).
- Conecta el cable al puerto **UART** de la placa.
- Salir del monitor: `Ctrl + ]`.

> Si te olvidas del `-DARQ`, CMake **conserva el último valor** que guardó en `build/CMakeCache.txt`. La línea `arquitecturas: compilando con ARQ=n` que imprime la configuración te dice cuál se está compilando de verdad.

## Qué hay que mirar

Pulsa **BOOT** una docena de veces en cada arquitectura y anota la **latencia MAXIMA**, no la media: en tiempo real lo que importa es el peor caso.

```
I (1234) arquitecturas: BOTON #7  latencia  198431 us  (min 412  media 96204  MAX 198431)  rebotes 3
```

| | Latencia máxima esperada | Por qué |
|---|---|---|
| **1. Round-Robin** | ~**200 ms** | Hay que esperar a que termine la vuelta entera |
| **2. RR + ISR** | ~**200 ms** | La ISR anota al instante, pero la respuesta sigue esperando turno |
| **3. Cola de funciones** | ~**10 ms** | Solo hay que esperar al paso troceado en curso |
| **4. RTOS** | **decenas de µs** | El planificador desaloja el cálculo en mitad de la operación |

Las arquitecturas 1 y 2 dan **la misma** latencia de respuesta. Esa coincidencia es la lección: la interrupción mejora la **captura** del evento (precisión de microsegundos, ninguna pulsación perdida) pero **no mejora la respuesta** mientras el trabajo siga saliendo del mismo bucle.

## Salida esperada

```
I (301) arquitecturas: ==========================================================
I (302) arquitecturas:  Arquitectura 4. RTOS (FreeRTOS: tareas y prioridades)
I (303) arquitecturas: ==========================================================
I (304) arquitecturas: Trabajo A: parpadeo cada 500 ms
I (305) arquitecturas: Trabajo B: calculo de 200 ms, de un tiron
I (306) arquitecturas: Trabajo C: responder al boton BOOT (GPIO0)  <-- se mide
I (512) arquitecturas: Calibracion: 20134 iteraciones = 1 ms  (la muestra de 200000 tardo 9933 us)
I (513) arquitecturas: Pulsa BOOT unas cuantas veces y mira la latencia MAXIMA.
I (3120) arquitecturas: BOTON #1  latencia      47 us  (min 47  media 47  MAX 47)  rebotes 0
I (4802) arquitecturas: BOTON #2  latencia      52 us  (min 47  media 49  MAX 52)  rebotes 2
```

Los números concretos dependen de la placa y de la frecuencia de CPU; el **orden de magnitud** y la **relación entre arquitecturas** no.

## Por qué `sdkconfig.defaults` desactiva el watchdog sobre IDLE

En la ESP32-S2, ESP-IDF arranca **siempre** con FreeRTOS corriendo debajo: `app_main()` es una tarea. Las arquitecturas 1, 2 y 3 lo **ignoran a propósito** —un solo bucle que nunca se bloquea, tal y como se escribiría un firmware sin sistema operativo— y ese bucle no deja ejecutarse nunca a la tarea IDLE. El *task watchdog* lo detecta y la da por colgada:

```
E (5321) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (5321) task_wdt:  - IDLE (CPU 0)
```

Desactivar esa comprobación es lo que nos permite enseñar el bucle sin RTOS tal cual es. **En un proyecto con RTOS esto no se toca**: que la tarea IDLE no llegue a ejecutarse es un síntoma —memoria de tareas borradas sin liberar, mantenimiento sin hacer—, no una molestia que silenciar.

Fíjate en el contraste: en la arquitectura 4, `tarea_calculo()` hace un `vTaskDelay(1)` por lote **precisamente para no caer en eso**, aun siendo la tarea de prioridad más baja del sistema.

## El coste del RTOS, medido

`idf.py size` sobre las mismas fuentes, cambiando solo `ARQ`:

| | DIRAM (bytes) | Imagen total |
|---|---:|---:|
| **ARQ=1** Round-Robin | 55 260 | 159 690 |
| **ARQ=3** Cola de funciones | 55 796 | 161 326 |
| **ARQ=4** RTOS | 55 728 | 161 506 |

Prácticamente lo mismo — y eso **no** significa que el RTOS sea gratis. Significa que en ESP-IDF **el kernel ya está enlazado en los tres casos**: `app_main()` es una tarea de FreeRTOS aunque tu código no lo use. El coste real del RTOS aquí no está en la imagen: está en el ***heap***, donde vive **una pila por tarea**:

```c
xTaskCreate(tarea_boton,    "boton",    3072, NULL, 10, NULL);
xTaskCreate(tarea_parpadeo, "parpadeo", 2560, NULL,  5, NULL);
xTaskCreate(tarea_calculo,  "calculo",  2560, NULL,  1, NULL);
```

8 KB de RAM que en las arquitecturas 1–3 no se gastan, porque hay **una sola pila**. En un MCU con 320 KB no duele; en uno con 8 KB, decide el diseño.

## Cosas para probar

| Experimento | Qué observar |
|---|---|
| Subir `CALCULO_MS` a 1000 en ARQ=1 | La latencia máxima lo sigue: **es** la duración de la vuelta |
| Subir `CALCULO_MS` a 1000 en ARQ=4 | La latencia **no se mueve**: el desalojo no depende del trabajo de fondo |
| Bajar `PASO_MS` a 1 en ARQ=3 | La latencia baja, pero el troceado cuesta más CPU: el compromiso de la arquitectura 3 |
| Subir `PASO_MS` a 100 en ARQ=3 | La arquitectura 3 se degrada hasta parecerse a la 1 |
| Quitar `portYIELD_FROM_ISR()` en ARQ=4 | La latencia salta a milisegundos: se espera al siguiente *tick* |
| Poner `tarea_boton` a prioridad 1 en ARQ=4 | Deja de desalojar al cálculo: la prioridad **es** el diseño |
| Quitar `volatile` de `s_hay_evento` en ARQ=2 | Con `-O2`, el bucle puede no ver nunca el cambio |
| Comentar la sección crítica en ARQ=3 | Aparecen pulsaciones perdidas, raras y difíciles de reproducir |

## Solución de problemas

| Síntoma | Solución |
|---|---|
| Compila siempre la misma arquitectura | Pasa `-DARQ=n`; mira la línea `compilando con ARQ=n` |
| `ARQ tiene que valer 1…4` | El `-DARQ` recibió algo que no es 1, 2, 3 ni 4 |
| El LED no enciende | Colocar el **jumper RGB**; verificar GPIO45 |
| `Task watchdog got triggered` sobre IDLE | Falta `CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=n`; borra `sdkconfig` y reconstruye |
| Muchos rebotes contados | Normal: el pulsador rebota. Subir `REBOTE_US` |
| Una pulsación no se registra | En ARQ=1 y ARQ=2 es **esperable**: es parte de lo que se demuestra |
| `Failed to connect` al flashear | Secuencia **BOOT + RST** (modo bootloader) |

## Referencias

- [../../detalles/arquitecturas-de-firmware.md](../../detalles/arquitecturas-de-firmware.md)
- [../../detalles/freertos-en-esp-idf.md](../../detalles/freertos-en-esp-idf.md)
- [../../detalle-latencia-y-peor-caso.md](../../detalle-latencia-y-peor-caso.md)
- [../../../clase-2/detalles/interrupciones-y-datos-compartidos.md](../../../clase-2/detalles/interrupciones-y-datos-compartidos.md)
- Barry, R. *Mastering the FreeRTOS Real-Time Kernel*, cap. 3–4.
- Fan, X. (2015). *Real-Time Embedded Systems*, cap. 2–4.
- Guía de conexión e instalación: [../../../bibliografia/Guia-Kaluga-1-macOS-ESP-IDF-VSCode.md](../../../bibliografia/Guia-Kaluga-1-macOS-ESP-IDF-VSCode.md)
