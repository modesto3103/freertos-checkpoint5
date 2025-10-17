#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"

/* ==========================
 *  CONFIGURAÇÕES GERAIS
 * ========================== */

/* Identificação obrigatória em TODOS os prints */
#define STUDENT_PREFIX "{Pedro Modesto Mesquita-RM:87880} "
#define PRINTF(fmt, ...) printf(STUDENT_PREFIX fmt, ##__VA_ARGS__)

/* Prioridades (maior número = maior prioridade) */
#define GEN_TASK_PRIO      6   // Módulo 1 – Geração de Dados
#define RX_TASK_PRIO       5   // Módulo 2 – Recepção/Transmissão
#define SUP_TASK_PRIO      4   // Módulo 3 – Supervisão
#define LOG_TASK_PRIO      2   // Extra – Log periódico (opcional)

/* Tamanhos de pilha (em words) */
#define GEN_STACK_WORDS    4096
#define RX_STACK_WORDS     4096
#define SUP_STACK_WORDS    4096
#define LOG_STACK_WORDS    3072

/* Fila */
#define QUEUE_LEN          10
#define QUEUE_ITEM_SIZE    sizeof(int)

/* Temporizações */
#define GEN_PERIOD_MS            150
#define RX_TIMEOUT_MS            1000
#define SUP_PERIOD_MS            1500
#define STALL_TICKS(ms)          pdMS_TO_TICKS(ms)

/* Escalonamento de reações na RX */
#define RX_WARN_THRESHOLD        2   // n° de timeouts para aviso leve
#define RX_RECOVER_SOFT          3   // tentativa leve (limpeza de estado)
#define RX_RECOVER_RESET_Q       4   // reset da fila
#define RX_FAIL_THRESHOLD        5   // encerra tarefa para o supervisor recriar

/* Watchdog (Task WDT) */
#define WDT_TIMEOUT_SECONDS      5

/* ==========================
 *  ESTADO GLOBAL
 * ========================== */
static QueueHandle_t g_queue = NULL;
static TaskHandle_t g_task_gen = NULL;
static TaskHandle_t g_task_rx  = NULL;
static TaskHandle_t g_task_sup = NULL;
static TaskHandle_t g_task_log = NULL; // opcional

/* Heartbeats e flags de saúde */
static volatile TickType_t g_hb_gen = 0;
static volatile TickType_t g_hb_rx  = 0;
static volatile TickType_t g_hb_sup = 0;

static volatile bool g_flag_gen_ok = false;
static volatile bool g_flag_rx_ok  = false;

/* ==========================
 *  MÓDULO 1 – Geração de Dados
 *  Produz inteiros sequenciais; envia para a fila; descarta se cheia.
 * ========================== */
static void task_generator(void *pv) {
    /* Vincula esta tarefa ao Task Watchdog */
    esp_task_wdt_add(NULL);

    int value = 0;
    for (;;) {
        /* Tenta enviar sem bloquear; se a fila estiver cheia, descarta */
        if (xQueueSend(g_queue, &value, 0) == pdTRUE) {
            g_hb_gen = xTaskGetTickCount();
            g_flag_gen_ok = true;
            PRINTF("[GERADOR] Valor %d enfileirado com sucesso.\n", value);
            value++;
        } else {
            /* Descarta, mas segue operando */
            PRINTF("[GERADOR] Fila cheia – valor %d descartado.\n", value);
            value++; // segue sequência mesmo descartando
        }

        /* Checagem de stack em runtime */
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(NULL);
        if (watermark < 100) {
            PRINTF("[GERADOR] Atenção: pouca pilha restante (%u words).\n", (unsigned)watermark);
        }

        esp_task_wdt_reset();
        vTaskDelay(STALL_TICKS(GEN_PERIOD_MS));
    }
}

/* ==========================
 *  MÓDULO 2 – Recepção/"Transmissão"
 *  Recebe da fila; usa malloc/free temporário por item; reage a timeouts.
 * ========================== */
static void task_receiver(void *pv) {
    esp_task_wdt_add(NULL);

    int timeouts = 0;

    for (;;) {
        int rx_val = 0;
        if (xQueueReceive(g_queue, &rx_val, STALL_TICKS(RX_TIMEOUT_MS)) == pdTRUE) {
            /* Recebeu: zera contadores de falha e usa memória dinâmica */
            timeouts = 0;
            g_hb_rx = xTaskGetTickCount();
            g_flag_rx_ok = true;

            int *tmp = (int*) malloc(sizeof(int));
            if (!tmp) {
                PRINTF("[RX] ERRO CRÍTICO: malloc falhou – sem memória.\n");
                /* Sinaliza problema e pede reinício do sistema via supervisor */
                g_flag_rx_ok = false;
                break; // deixa o supervisor recriar
            }
            *tmp = rx_val;

            /* \"Transmissão\": exibe no terminal */
            PRINTF("[RX] Transmitindo valor: %d\n", *tmp);

            free(tmp);

        } else {
            /* TIMEOUT – comportamento escalonado */
            timeouts++;
            PRINTF("[RX] Timeout de %d ms na fila (contagem=%d).\n", RX_TIMEOUT_MS, timeouts);

            if (timeouts == RX_WARN_THRESHOLD) {
                PRINTF("[RX] Aviso: ausência de dados – checando conexões.\n");
            } else if (timeouts == RX_RECOVER_SOFT) {
                PRINTF("[RX] Recuperação leve: limpando estados locais.\n");
                /* (coloque aqui limpezas de buffers/caches se houver) */
            } else if (timeouts == RX_RECOVER_RESET_Q) {
                PRINTF("[RX] Recuperação moderada: resetando a fila.\n");
                xQueueReset(g_queue);
            } else if (timeouts >= RX_FAIL_THRESHOLD) {
                PRINTF("[RX] Falha persistente: encerrando tarefa para recriação pelo supervisor.\n");
                g_flag_rx_ok = false;
                break; // sai do loop para o supervisor detectar e recriar
            }
        }

        /* Telemetria de heap */
        size_t free_heap = xPortGetFreeHeapSize();
        size_t min_heap  = xPortGetMinimumEverFreeHeapSize();
        if (free_heap < (20 * 1024)) {
            PRINTF("[RX] Pouca memória livre: %u bytes (mínimo histórico %u).\n",
                   (unsigned)free_heap, (unsigned)min_heap);
        }

        esp_task_wdt_reset();
        /* Pequena folga para simular processamento */
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    PRINTF("[RX] Tarefa será finalizada para permitir recriação.\n");
    vTaskDelete(NULL);
}

/* ==========================
 *  MÓDULO 3 – Supervisão
 *  Monitora heartbeats, flags e WDT. Recria tarefas quando necessário e
 *  reporta status periodicamente.
 * ========================== */
static void task_supervisor(void *pv) {
    esp_task_wdt_add(NULL);

    int rx_restarts = 0;

    for (;;) {
        vTaskDelay(STALL_TICKS(SUP_PERIOD_MS));
        TickType_t now = xTaskGetTickCount();
        g_hb_sup = now;

        /* Status/flags na tela */
        PRINTF("[SUP] Status – GEN:%s (hb=%u) | RX:%s (hb=%u)\n",
               g_flag_gen_ok ? "OK" : "ERRO",
               (unsigned)g_hb_gen,
               g_flag_rx_ok  ? "OK" : "ERRO",
               (unsigned)g_hb_rx);

        /* GEN parado? (sem heartbeat recente) – recria */
        if ((now - g_hb_gen) > STALL_TICKS(3 * SUP_PERIOD_MS)) {
            PRINTF("[SUP] Detetado GERADOR inativo – reiniciando tarefa.\n");
            if (g_task_gen) {
                vTaskDelete(g_task_gen);
                g_task_gen = NULL;
            }
            xTaskCreatePinnedToCore(task_generator, "task_generator",
                                    GEN_STACK_WORDS, NULL, GEN_TASK_PRIO, &g_task_gen, 1);
            g_hb_gen = xTaskGetTickCount();
            g_flag_gen_ok = false; // será setado pela própria tarefa
        }

        /* RX ausente ou sem batidas – recria */
        if (g_task_rx == NULL || (now - g_hb_rx) > STALL_TICKS(5 * SUP_PERIOD_MS)) {
            PRINTF("[SUP] Detetada RX inativa – recriando tarefa.\n");
            if (g_task_rx) {
                vTaskDelete(g_task_rx);
                g_task_rx = NULL;
            }
            xTaskCreatePinnedToCore(task_receiver, "task_receiver",
                                    RX_STACK_WORDS, NULL, RX_TASK_PRIO, &g_task_rx, 1);
            rx_restarts++;
            g_hb_rx = xTaskGetTickCount();
            g_flag_rx_ok = false; // será setado pela própria tarefa

            /* Heurística: muitas recriações + pouca memória => reiniciar chip */
            if (rx_restarts >= 3) {
                size_t free_heap = xPortGetFreeHeapSize();
                if (free_heap < (16 * 1024)) {
                    PRINTF("[SUP] Memória crítica após várias recriações (%u bytes). Reiniciando dispositivo...\n",
                           (unsigned)free_heap);
                    esp_restart();
                }
            }
        }

        /* Telemetria de heap geral */
        size_t free_heap = xPortGetFreeHeapSize();
        size_t min_heap  = xPortGetMinimumEverFreeHeapSize();
        PRINTF("[SUP] Heap livre=%u bytes (mínimo histórico %u).\n",
               (unsigned)free_heap, (unsigned)min_heap);
        if (min_heap < (8 * 1024)) {
            PRINTF("[SUP] Heap mínimo crítico – reiniciando dispositivo...\n");
            esp_restart();
        }

        esp_task_wdt_reset();
    }
}

/* ==========================
 *  LOG PERIÓDICO (opcional)
 * ========================== */
static void task_logger(void *pv) {
    for (;;) {
        PRINTF("[LOG] HB_GEN=%u | HB_RX=%u | HB_SUP=%u\n",
               (unsigned)g_hb_gen, (unsigned)g_hb_rx, (unsigned)g_hb_sup);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ==========================
 *  app_main – inicialização, WDT, fila e tarefas
 * ========================== */
void app_main(void) {
    PRINTF("[BOOT] Iniciando sistema multitarefa FreeRTOS com WDT.\n");

    /* Inicializa o Task Watchdog (timeout e reset em pânico habilitado) */
    esp_task_wdt_deinit();                 // garante estado limpo (caso já esteja init)
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = WDT_TIMEOUT_SECONDS * 1000,
        .trigger_panic = true,
    };
    esp_task_wdt_init(&wdt_cfg);

    /* Cria fila */
    g_queue = xQueueCreate(QUEUE_LEN, QUEUE_ITEM_SIZE);
    if (!g_queue) {
        PRINTF("[BOOT] ERRO: Falha ao criar fila – reiniciando dispositivo.\n");
        esp_restart();
    }

    /* Cria tarefas principais */
    BaseType_t ok = pdPASS;

    ok &= xTaskCreatePinnedToCore(task_generator, "task_generator", GEN_STACK_WORDS,
                                  NULL, GEN_TASK_PRIO, &g_task_gen, 1) == pdPASS;

    ok &= xTaskCreatePinnedToCore(task_receiver,  "task_receiver",  RX_STACK_WORDS,
                                  NULL, RX_TASK_PRIO,  &g_task_rx,  1) == pdPASS;

    ok &= xTaskCreatePinnedToCore(task_supervisor, "task_supervisor", SUP_STACK_WORDS,
                                  NULL, SUP_TASK_PRIO, &g_task_sup, 1) == pdPASS;

    /* Log auxiliar (opcional) */
    xTaskCreatePinnedToCore(task_logger, "task_logger", LOG_STACK_WORDS,
                            NULL, LOG_TASK_PRIO, &g_task_log, 1);

    if (!ok) {
        PRINTF("[BOOT] ERRO: Falha na criação de tarefas – reiniciando dispositivo.\n");
        esp_restart();
    }

    PRINTF("[BOOT] Tarefas criadas com sucesso. Sistema em execução.\n");
}
