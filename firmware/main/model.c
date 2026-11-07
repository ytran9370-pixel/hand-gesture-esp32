/*
 * model.c - engine suy luan INT8 (C thuan, khong thu vien ngoai)
 *
 *   anh RGB uint8 --LUT--> int8 --7 x [conv + ReLU (+maxpool)]--> GAP --> FC --> 18 logits int32
 *
 * Bo cuc du lieu: activation NHWC (pixel truoc, kenh sau), trong so OHWI.
 * Bo nho: 2 buffer 32 KB dung luan phien (tensor lon nhat = 32x32x32), trong so nam trong flash.
 */
#include <math.h>
#include <string.h>
#include "esp_timer.h"
#include "model.h"

#define ACT_BYTES (32 * 32 * 32)
static int8_t s_buf[2][ACT_BYTES] __attribute__((aligned(16)));
uint32_t g_layer_us[NUM_CONV_LAYERS + 2];

/* ---------- requantization fixed-point (giong TFLite / export_int8.py) ---------- */
static inline int32_t high_mul(int32_t a, int32_t b)          /* round(a*b / 2^31) */
{
    int64_t ab = (int64_t)a * b;
    int64_t nudge = ab >= 0 ? (1LL << 30) : (1 - (1LL << 30));
    return (int32_t)((ab + nudge) / (1LL << 31));
}

static inline int32_t round_shift_right(int32_t x, int e)     /* round(x / 2^e) */
{
    int32_t mask = (int32_t)((1LL << e) - 1);
    int32_t rem = x & mask, res = x >> e;
    return res + (rem > ((mask >> 1) + (res < 0)));
}

static inline int32_t requant(int32_t acc, int32_t mult, int32_t shift)   /* acc * M */
{
    int left = shift > 0 ? shift : 0, right = shift > 0 ? 0 : -shift;
    return round_shift_right(high_mul((int32_t)((uint32_t)acc << left), mult), right);
}

static inline int32_t dot(const int8_t *a, const int8_t *b, int n)
{
    int32_t s = 0;
    for (int i = 0; i < n; i++) {
        s += a[i] * b[i];
    }
    return s;
}

/* ---------- conv 2D + ReLU (clamp [0,127]) ---------- */
static void conv2d(const conv_layer_t *L, const int8_t *in, int8_t *out)
{
    const int H = L->in_hw, C = L->cin, k = L->k;
    for (int oy = 0; oy < L->out_hw; oy++) {
        for (int ox = 0; ox < L->out_hw; ox++) {
            const int iy0 = oy * L->stride - L->pad, ix0 = ox * L->stride - L->pad;
            /* cat cua so kernel o vien anh (padding = 0 vi zero_point = 0) */
            const int ky0 = iy0 < 0 ? -iy0 : 0, ky1 = iy0 + k > H ? H - iy0 : k;
            const int kx0 = ix0 < 0 ? -ix0 : 0, kx1 = ix0 + k > H ? H - ix0 : k;
            int8_t *y = out + (oy * L->out_hw + ox) * L->cout;
            for (int oc = 0; oc < L->cout; oc++) {
                const int8_t *w = L->w + oc * k * k * C;
                int32_t acc = L->bias[oc];
                for (int ky = ky0; ky < ky1; ky++) {
                    /* trong 1 hang, (kx, c) lien tuc trong ca input lan weight -> 1 tich vo huong */
                    acc += dot(in + ((iy0 + ky) * H + ix0 + kx0) * C, w + (ky * k + kx0) * C, (kx1 - kx0) * C);
                }
                int32_t v = requant(acc, L->mult[oc], L->shift[oc]);
                y[oc] = (int8_t)(v < 0 ? 0 : (v > 127 ? 127 : v));   /* ReLU + bao hoa int8 */
            }
        }
    }
}

static void maxpool2x2(const int8_t *in, int hw, int c, int8_t *out)
{
    const int o = hw / 2;
    for (int y = 0; y < o; y++) {
        for (int x = 0; x < o; x++) {
            const int8_t *a = in + (2 * y * hw + 2 * x) * c, *b = a + c, *d = a + hw * c, *e = d + c;
            int8_t *r = out + (y * o + x) * c;
            for (int i = 0; i < c; i++) {
                int8_t m = a[i];
                m = b[i] > m ? b[i] : m;
                m = d[i] > m ? d[i] : m;
                r[i] = e[i] > m ? e[i] : m;
            }
        }
    }
}

void model_run(const uint8_t *rgb, int32_t *logits)
{
    int cur = 0;
    /* 1. tien xu ly: (p/255 - 0.5)/0.5 roi luong tu hoa -> gop thanh 1 bang tra 256 phan tu */
    for (int i = 0; i < IMG_W * IMG_H * IMG_C; i++) {
        s_buf[0][i] = g_input_lut[rgb[i]];
    }
    /* 2. 7 lop conv */
    for (int l = 0; l < NUM_CONV_LAYERS; l++) {
        const conv_layer_t *L = &g_conv_layers[l];
        int64_t t0 = esp_timer_get_time();
        conv2d(L, s_buf[cur], s_buf[cur ^ 1]);
        cur ^= 1;
        if (L->pool) {
            maxpool2x2(s_buf[cur], L->out_hw, L->cout, s_buf[cur ^ 1]);
            cur ^= 1;
        }
        g_layer_us[l] = (uint32_t)(esp_timer_get_time() - t0);
    }
    /* 3. global average pool 4x4x96 -> 96, requant ve scale cua gap_aq */
    int64_t t0 = esp_timer_get_time();
    int8_t feat[96];
    const int npix = 4 * 4;
    for (int c = 0; c < 96; c++) {
        int32_t s = 0;
        for (int p = 0; p < npix; p++) {
            s += s_buf[cur][p * 96 + c];
        }
        int32_t v = requant(s, g_gap_mult, g_gap_shift);
        feat[c] = (int8_t)(v < -128 ? -128 : (v > 127 ? 127 : v));
    }
    g_layer_us[NUM_CONV_LAYERS] = (uint32_t)(esp_timer_get_time() - t0);
    /* 4. fully connected 96 -> 18, giu nguyen int32 (argmax khong can doi ve float) */
    t0 = esp_timer_get_time();
    for (int o = 0; o < NUM_CLASSES; o++) {
        logits[o] = dot(feat, g_fc_w + o * 96, 96) + g_fc_b[o];
    }
    g_layer_us[NUM_CONV_LAYERS + 1] = (uint32_t)(esp_timer_get_time() - t0);
}

int model_argmax(const int32_t *logits)
{
    int best = 0;
    for (int i = 1; i < NUM_CLASSES; i++) {
        best = logits[i] > logits[best] ? i : best;
    }
    return best;
}

float model_confidence(const int32_t *logits, int cls)
{
    float mx = logits[model_argmax(logits)] * g_logit_scale, sum = 0.f;
    for (int i = 0; i < NUM_CLASSES; i++) {
        sum += expf(logits[i] * g_logit_scale - mx);
    }
    return expf(logits[cls] * g_logit_scale - mx) / sum;
}
