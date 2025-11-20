/*
 * Monitor de Redes Wi-Fi Seguras em Tempo Real com FreeRTOS
 * Aluno: Daniel Fernandes - RM86936
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_task_wdt.h"

#define MAX_SSID_LEN      32
#define SAFE_WIFI_COUNT    5
#define WDT_TIMEOUT_MS  5000

// Bits para o supervisor
#define BIT_SCANNER_OK (1 << 0)
#define BIT_MONITOR_OK (1 << 1)

// Estrutura de mensagem de rede
typedef struct {
    char ssid[MAX_SSID_LEN];
    int  rssi; // só pra simular alguma info de sinal
} wifi_msg_t;

// Lista de redes seguras (protegida por semáforo)
static const char *safe_ssids[SAFE_WIFI_COUNT] = {
    "Empresa_Corporativa",
    "Home_Office_Daniel",
    "CyberWorkShield",
    "VPN_Segura_01",
    "Rede_TI_Interna"
};

// Recursos globais
static QueueHandle_t      wifi_queue      = NULL;
static SemaphoreHandle_t  sem_lista       = NULL;
static EventGroupHandle_t event_supervisor = NULL;

// ---------- TASK 1: SCANNER / GERADOR DE REDE ATUAL ----------
// Simula a rede Wi-Fi à qual o dispositivo está conectado
void task_wifi_scanner(void *pv) 
{
    static const char *test_networks[] = {
        "Empresa_Corporativa",   // segura
        "WiFi_Cafe",             // não autorizada
        "CyberWorkShield",       // segura
        "Rede_Publica_Onibus",   // não autorizada
        "Home_Office_Daniel",    // segura
        "Shopping_FreeWiFi"      // não autorizada
    };

    const int total_networks = sizeof(test_networks) / sizeof(test_networks[0]);
    int idx = 0;

    for (;;) {
        wifi_msg_t *msg = (wifi_msg_t *) malloc(sizeof(wifi_msg_t));
        if (msg == NULL) {
            printf("[Scanner] Falha ao alocar memória para mensagem Wi-Fi\n");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Preenche os dados antes de mandar
        strncpy(msg->ssid, test_networks[idx], MAX_SSID_LEN - 1);
        msg->ssid[MAX_SSID_LEN - 1] = '\0';
        msg->rssi = -40 - (idx * 3);

        // Faz o log ANTES de mandar pra fila (antes de outra task poder mexer no ponteiro)
        printf("[Scanner] SSID atual gerado: %s (RSSI %d)\n", msg->ssid, msg->rssi);

        if (xQueueSend(wifi_queue, &msg, 0) != pdTRUE) {
            printf("[Scanner] Fila cheia, descartando SSID: %s\n", msg->ssid);
            free(msg);
        } else {
            // só sinaliza, não usa mais o ponteiro
            xEventGroupSetBits(event_supervisor, BIT_SCANNER_OK);
        }

        esp_task_wdt_reset();

        idx = (idx + 1) % total_networks;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ---------- TASK 2: MONITOR DE REDE / VERIFICA SE É SEGURA ----------

static bool is_ssid_safe(const char *ssid)
{
    bool safe = false;

    // Protege o acesso à lista de redes seguras com semáforo (requisito)
    if (xSemaphoreTake(sem_lista, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < SAFE_WIFI_COUNT; i++) {
            if (strcmp(ssid, safe_ssids[i]) == 0) {
                safe = true;
                break;
            }
        }
        xSemaphoreGive(sem_lista);
    }

    return safe;
}

void task_wifi_monitor(void *pv)
{
    wifi_msg_t *msg = NULL;
    int timeout_count = 0;

    for (;;) {
        // Espera até 4 segundos por uma nova mensagem de rede
        BaseType_t ok = xQueueReceive(wifi_queue, &msg, pdMS_TO_TICKS(4000));

        if (ok == pdTRUE && msg != NULL) {
            timeout_count = 0; // reset do contador de falha

            // Verifica se é rede segura
            bool safe = is_ssid_safe(msg->ssid);

            if (safe) {
                printf("[Monitor] Conectado em rede SEGURA: %s (RSSI %d)\n",
                       msg->ssid, msg->rssi);
            } else {
                // ALERTA imediato para rede não autorizada (requisito)
                printf("[Monitor] *** ALERTA *** Rede NÃO AUTORIZADA detectada: %s (RSSI %d)\n",
                       msg->ssid, msg->rssi);
            }

            // Sinaliza para o supervisor que está ativo
            xEventGroupSetBits(event_supervisor, BIT_MONITOR_OK);

            // Alimenta WDT
            esp_task_wdt_reset();

            // Libera memória da mensagem
            free(msg);
            msg = NULL;
        } else {
            // Timeout: não recebeu nada da fila
            timeout_count++;
            printf("[Monitor] Timeout ao receber rede da fila (%d)\n", timeout_count);

            // Técnica de robustez 1: tentativa de recuperação leve
            if (timeout_count == 3) {
                printf("[Monitor] Aviso: sem dados recentes. Verificando integridade da fila.\n");
            }
            // Técnica de robustez 2: recuperação mais agressiva
            else if (timeout_count == 5) {
                printf("[Monitor] Falha prolongada. Resetando fila e sistema para recuperação.\n");
                xQueueReset(wifi_queue);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
        }

        // Pequeno delay para não travar o scheduler
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

// ---------- TASK 3: SUPERVISOR / STATUS DO SISTEMA ----------

void task_supervisor(void *pv)
{
    for (;;) {
        // Aguarda até 5 segundos por sinal das duas tasks
        EventBits_t bits = xEventGroupWaitBits(
            event_supervisor,
            BIT_SCANNER_OK | BIT_MONITOR_OK,
            pdTRUE,          // limpa os bits após leitura
            pdFALSE,         // qualquer um dos bits serve
            pdMS_TO_TICKS(5000)
        );

        if ((bits & BIT_SCANNER_OK) && (bits & BIT_MONITOR_OK)) {
            printf("[Supervisor] Sistema OK (Scanner e Monitor ativos)\n");
        } else if (bits & BIT_SCANNER_OK) {
            printf("[Supervisor] Sistema parcialmente OK (apenas Scanner sinalizou)\n");
        } else if (bits & BIT_MONITOR_OK) {
            printf("[Supervisor] Sistema parcialmente OK (apenas Monitor sinalizou)\n");
        } else {
            printf("[Supervisor] Nenhuma task sinalizou nos últimos 5s. Possível travamento.\n");
        }

        // Alimenta WDT
        esp_task_wdt_reset();

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ---------- FUNÇÃO PRINCIPAL ----------

void app_main(void)
{
    // Configura o Watchdog Timer (WDT)
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT_MS,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_config);

    // Cria fila para ponteiros de wifi_msg_t
    wifi_queue = xQueueCreate(5, sizeof(wifi_msg_t *));
    // Semáforo para proteger lista de redes seguras
    sem_lista = xSemaphoreCreateBinary();
    // Event group para o supervisor
    event_supervisor = xEventGroupCreate();

    if (wifi_queue == NULL || sem_lista == NULL || event_supervisor == NULL) {
        printf("Falha ao criar recursos (fila/semáforo/event group). Reiniciando...\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    // Libera o semáforo da lista na inicialização
    xSemaphoreGive(sem_lista);

    // Cria tasks com prioridades diferentes (requisito)
    TaskHandle_t hScanner, hMonitor, hSupervisor;

    xTaskCreate(task_wifi_scanner,   "WiFiScanner",   4096, NULL, 3, &hScanner);
    xTaskCreate(task_wifi_monitor,   "WiFiMonitor",   4096, NULL, 4, &hMonitor);   // mais crítica
    xTaskCreate(task_supervisor,     "Supervisor",    4096, NULL, 2, &hSupervisor);

    // Adiciona tasks ao WDT
    esp_task_wdt_add(hScanner);
    esp_task_wdt_add(hMonitor);
    esp_task_wdt_add(hSupervisor);
}
