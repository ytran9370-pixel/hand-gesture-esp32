/* commands.h - infer task: executes every request received from the PC (HG-ICD-001 section 5). */
#pragma once
#include "esp_err.h"

/* Power-on self-test: runs the embedded golden images and checks the logits bit by bit (FR-DIAG-01).
 * Returns the number of images that passed. */
int commands_post(void);

esp_err_t commands_start(void);
