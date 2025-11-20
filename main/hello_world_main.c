/*
 * Monitor de Redes Wi-Fi Seguras em Tempo Real com FreeRTOS
 Nomes: Antonio Ramos Ferreira – RM88311
        Daniel Corrêa Fernandes – RM86936
        Leonardo Filiaci Bianor – RM89370
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


#define BIT_SCANNER_OK (1 << 0)
#define BIT_MONITOR_OK (1 << 1)


typedef struct {
    char ssid[MAX_SSID_LEN];
    int  rssi; 
} wifi_msg_t;


static const char *safe_ssids[SAFE_WIFI_COUNT] = {
    "Empresa_Corporativa",
    "Home_Office_Daniel",
    "CyberWorkShield",
    "VPN_Segura_01",
    "Rede_TI_Interna"
};


static QueueHandle_t      wifi_queue      = NULL;
static SemaphoreHandle_t  sem_lista       = NULL;
static EventGroupHandle_t event_supervisor = NULL;


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

        
        strncpy(msg->ssid, test_networks[idx], MAX_SSID_LEN - 1);
        msg->ssid[MAX_SSID_LEN - 1] = '\0';
        msg->rssi = -40 - (idx * 3);

        
        printf("[Scanner] SSID atual gerado: %s (RSSI %d)\n", msg->ssid, msg->rssi);

        if (xQueueSend(wifi_queue, &msg, 0) != pdTRUE) {
            printf("[Scanner] Fila cheia, descartando SSID: %s\n", msg->ssid);
            free(msg);
        } else {
            
            xEventGroupSetBits(event_supervisor, BIT_SCANNER_OK);
        }

        esp_task_wdt_reset();

        idx = (idx + 1) % total_networks;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}



static bool is_ssid_safe(const char *ssid)
{
    bool safe = false;

    
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
        
        BaseType_t ok = xQueueReceive(wifi_queue, &msg, pdMS_TO_TICKS(4000));

        if (ok == pdTRUE && msg != NULL) {
            timeout_count = 0; 

            
            bool safe = is_ssid_safe(msg->ssid);

            if (safe) {
                printf("[Monitor] Conectado em rede SEGURA: %s (RSSI %d)\n",
                       msg->ssid, msg->rssi);
            } else {
                
                printf("[Monitor] *** ALERTA *** Rede NÃO AUTORIZADA detectada: %s (RSSI %d)\n",
                       msg->ssid, msg->rssi);
            }

            
            xEventGroupSetBits(event_supervisor, BIT_MONITOR_OK);

            
            esp_task_wdt_reset();

            
            free(msg);
            msg = NULL;
        } else {
            
            timeout_count++;
            printf("[Monitor] Timeout ao receber rede da fila (%d)\n", timeout_count);

            
            if (timeout_count == 3) {
                printf("[Monitor] Aviso: sem dados recentes. Verificando integridade da fila.\n");
            }
            
            else if (timeout_count == 5) {
                printf("[Monitor] Falha prolongada. Resetando fila e sistema para recuperação.\n");
                xQueueReset(wifi_queue);
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }
        }

        
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}



void task_supervisor(void *pv)
{
    for (;;) {
        
        EventBits_t bits = xEventGroupWaitBits(
            event_supervisor,
            BIT_SCANNER_OK | BIT_MONITOR_OK,
            pdTRUE,          
            pdFALSE,         
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

        
        esp_task_wdt_reset();

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}



void app_main(void)
{
    
    esp_task_wdt_config_t wdt_config = {
        .timeout_ms = WDT_TIMEOUT_MS,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true
    };
    esp_task_wdt_init(&wdt_config);

    
    wifi_queue = xQueueCreate(5, sizeof(wifi_msg_t *));
    
    sem_lista = xSemaphoreCreateBinary();
    
    event_supervisor = xEventGroupCreate();

    if (wifi_queue == NULL || sem_lista == NULL || event_supervisor == NULL) {
        printf("Falha ao criar recursos (fila/semáforo/event group). Reiniciando...\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    
    xSemaphoreGive(sem_lista);

    
    TaskHandle_t hScanner, hMonitor, hSupervisor;

    xTaskCreate(task_wifi_scanner,   "WiFiScanner",   4096, NULL, 3, &hScanner);
    xTaskCreate(task_wifi_monitor,   "WiFiMonitor",   4096, NULL, 4, &hMonitor);   
    xTaskCreate(task_supervisor,     "Supervisor",    4096, NULL, 2, &hSupervisor);

    
    esp_task_wdt_add(hScanner);
    esp_task_wdt_add(hMonitor);
    esp_task_wdt_add(hSupervisor);
}
