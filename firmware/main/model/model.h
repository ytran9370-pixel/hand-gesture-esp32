/* model.h - CNN nhan dien cu chi tay, INT8 doi xung, chay tren ESP32-S3 */
#pragma once
#include <stdint.h>

#define IMG_W 64
#define IMG_H 64
#define IMG_C 3
#define NUM_CLASSES 18
#define NUM_CONV_LAYERS 7

typedef struct {
    const char *name;
    uint8_t k, stride, pad, pool;      /* pool = 1: maxpool 2x2 sau lop nay */
    uint16_t in_hw, cin, out_hw, cout; /* out_hw = kich thuoc TRUOC khi pool */
    const int8_t *w;                   /* [cout][k][k][cin]  (OHWI)          */
    const int32_t *bias, *mult, *shift;/* moi kenh ra 1 gia tri              */
} conv_layer_t;

/* ---- du lieu sinh boi tools/export_int8.py (model_data.c) ---- */
extern const int8_t g_input_lut[256];
extern const conv_layer_t g_conv_layers[NUM_CONV_LAYERS];
extern const int32_t g_gap_mult, g_gap_shift;
extern const int8_t g_fc_w[NUM_CLASSES * 96];
extern const int32_t g_fc_b[NUM_CLASSES];
extern const float g_logit_scale;
extern const char *const g_labels[NUM_CLASSES];

/* ---- anh test (test_images.c) ---- */
extern const int g_num_test;
extern const uint8_t g_test_label[];
extern const int32_t g_test_logits[];
extern const uint8_t g_test_rgb[];

/* ---- engine (model.c) ---- */
/* rgb: 64*64*3 byte, thu tu R,G,B theo tung pixel (HWC). Ket qua: 18 logits int32. */
void model_run(const uint8_t *rgb, int32_t *logits);
int model_argmax(const int32_t *logits);
float model_confidence(const int32_t *logits, int cls);   /* softmax(logit * scale)[cls] */
extern uint32_t g_layer_us[NUM_CONV_LAYERS + 2];           /* thoi gian tung lop (us)     */
