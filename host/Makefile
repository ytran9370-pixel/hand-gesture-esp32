# make -C host   -> build and run the firmware's portable C code on the PC (Linux / WSL / macOS)
#   test_pc     : INT8 engine, logits must be bit-exact with the reference model
#   test_logic  : CRC, frame parser, gesture filter, system state machine
FW      = ../firmware/main
CFLAGS  = -std=gnu17 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -Istub -I$(FW)/model -I$(FW)/core
MODEL   = $(FW)/model/model.c $(FW)/model/model_data.c $(FW)/model/test_images.c
CORE    = $(FW)/core/crc16.c $(FW)/core/proto.c $(FW)/core/gesture_filter.c $(FW)/core/system_fsm.c

run: test_pc test_logic
	./test_pc
	./test_logic

test_pc: test_pc.c $(MODEL)
	gcc $(CFLAGS) $^ -lm -o $@

test_logic: test_logic.c $(CORE) $(FW)/model/model_data.c
	gcc $(CFLAGS) $^ -lm -o $@

clean:
	rm -f test_pc test_logic

.PHONY: run clean
