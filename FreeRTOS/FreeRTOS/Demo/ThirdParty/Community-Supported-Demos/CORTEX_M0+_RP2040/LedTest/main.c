#include <stdio.h>
#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "task.h"

#define LED1_PIN 16  // Red
#define LED2_PIN 17  // Yellow
#define LED3_PIN 18  // Green

static const char *pin_to_name(uint pin) {
    switch (pin) {
        case LED1_PIN: return "Red";
        case LED2_PIN: return "Yellow";
        case LED3_PIN: return "Green";
        default: return "Unknown";
    }
}

void vLedTask(void *pvParameters) {
    uint pin = (uint)(uintptr_t)pvParameters;
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT);

    TickType_t delay;
    if (pin == LED1_PIN) delay = pdMS_TO_TICKS(100);
    else if (pin == LED2_PIN) delay = pdMS_TO_TICKS(1000);
    else delay = pdMS_TO_TICKS(50);

    printf("[%s] Task started on GPIO %d, delay=%lums\n", pin_to_name(pin), pin, delay);

    uint32_t count = 0;
    for (;;) {
        gpio_put(pin, 1);
        printf("[%7lu] %s ON  (cycle %lu)\n", xTaskGetTickCount(), pin_to_name(pin), count);
        vTaskDelay(delay);
        gpio_put(pin, 0);
        vTaskDelay(delay);
        count++;
    }
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);  // Give serial monitor time to connect

    printf("\n\n=== FreeRTOS LED Test ===\n");
    printf("Tick rate: %d Hz\n", configTICK_RATE_HZ);
    printf("Creating 3 LED tasks...\n\n");

    xTaskCreate(vLedTask, "Red",    256, (void*)(uintptr_t)LED1_PIN, 1, NULL);
    xTaskCreate(vLedTask, "Yellow", 256, (void*)(uintptr_t)LED2_PIN, 2, NULL);
    xTaskCreate(vLedTask, "Green",  256, (void*)(uintptr_t)LED3_PIN, 3, NULL);

    printf("Starting scheduler...\n\n");
    vTaskStartScheduler();

    printf("ERROR: Scheduler exited!\n");
    for (;;);
}

/* FreeRTOS hook functions required by the config */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;
    (void)pcTaskName;
    printf("STACK OVERFLOW: %s\n", pcTaskName);
    for (;;);
}

void vApplicationTickHook(void) {
}

void vApplicationMallocFailedHook(void) {
    printf("MALLOC FAILED!\n");
    for (;;);
}
