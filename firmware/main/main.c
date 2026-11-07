/*
 * main.c - chay model INT8 tren ESP32-S3 voi cac anh test nhung san trong flash.
 *
 * Buoc 1: kiem tra dung tung bit: logits tren chip == logits cua golden model NumPy
 * Buoc 2: do toc do (trung binh 50 lan) va thoi gian tung lop
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model.h"

static const char *k_layer_names[NUM_CONV_LAYERS + 2] = {
    "stem", "stage1_conv", "stage1_pw", "stage2_conv", "stage2_pw", "stage3_conv", "stage3_pw", "gap", "fc"};

void app_main(void)
{
    int32_t logits[NUM_CLASSES];
    const int img = IMG_W * IMG_H * IMG_C;

    printf("\n===== Hand gesture INT8 tren ESP32-S3 =====\n");
    printf("SRAM trong: %u bytes\n\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* ---------- 1. kiem tra dung dan ---------- */
    int n_exact = 0, n_correct = 0;
    printf("%-4s %-16s %-16s %6s %8s %s\n", "#", "nhan that", "du doan", "conf", "ms", "logits==golden?");
    for (int i = 0; i < g_num_test; i++) {
        int64_t t0 = esp_timer_get_time();
        model_run(&g_test_rgb[i * img], logits);
        int64_t us = esp_timer_get_time() - t0;
        int pred = model_argmax(logits);
        int exact = memcmp(logits, &g_test_logits[i * NUM_CLASSES], sizeof(logits)) == 0;
        n_exact += exact;
        n_correct += pred == g_test_label[i];
        printf("%-4d %-16s %-16s %5.1f%% %8.2f %s\n", i, g_labels[g_test_label[i]], g_labels[pred],
               100.0f * model_confidence(logits, pred), us / 1000.0, exact ? "OK" : "SAI");
        vTaskDelay(1);   /* nhuong CPU cho idle task (tranh canh bao watchdog) */
    }
    printf("\n=> Trung tung bit voi golden model: %d/%d   |  Dung nhan: %d/%d\n\n", n_exact, g_num_test,
           n_correct, g_num_test);

    /* ---------- 2. benchmark ---------- */
    const int runs = 50;
    uint64_t total = 0, layer_sum[NUM_CONV_LAYERS + 2] = {0};
    for (int r = 0; r < runs; r++) {
        int64_t t0 = esp_timer_get_time();
        model_run(&g_test_rgb[(r % g_num_test) * img], logits);
        total += esp_timer_get_time() - t0;
        for (int l = 0; l < NUM_CONV_LAYERS + 2; l++) {
            layer_sum[l] += g_layer_us[l];
        }
        vTaskDelay(1);
    }
    double avg_ms = total / 1000.0 / runs;
    printf("Toc do trung binh (%d lan): %.2f ms/anh  (%.1f anh/giay, 16.1 M MAC)\n", runs, avg_ms, 1000.0 / avg_ms);
    for (int l = 0; l < NUM_CONV_LAYERS + 2; l++) {
        printf("   %-12s %8.2f ms  %5.1f%%\n", k_layer_names[l], layer_sum[l] / 1000.0 / runs,
               100.0 * layer_sum[l] / total);
    }
    printf("\nXong. (nhan nut RST de chay lai)\n");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
