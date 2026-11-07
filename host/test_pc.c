/* test_pc.c - chay dung engine cua firmware tren PC de kiem tra TRUOC khi nap chip */
#include <stdio.h>
#include <string.h>
#include "model.h"

int main(void)
{
    int32_t logits[NUM_CLASSES];
    int exact = 0;
    for (int i = 0; i < g_num_test; i++) {
        model_run(&g_test_rgb[i * IMG_W * IMG_H * IMG_C], logits);
        int ok = memcmp(logits, &g_test_logits[i * NUM_CLASSES], sizeof(logits)) == 0;
        exact += ok;
        printf("anh %d: that=%-16s doan=%-16s %s\n", i, g_labels[g_test_label[i]], g_labels[model_argmax(logits)],
               ok ? "trung tung bit" : "SAI KHAC");
    }
    printf("%d/%d anh trung tung bit voi golden model -> %s\n", exact, g_num_test, exact == g_num_test ? "PASS" : "FAIL");
    return exact == g_num_test ? 0 : 1;
}
