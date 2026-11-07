# make -C host        -> build + chay engine tren PC (Linux / macOS / WSL / MSYS2)
SRC = test_pc.c ../firmware/main/model.c ../firmware/main/model_data.c ../firmware/main/test_images.c
run: test_pc
	./test_pc
test_pc: $(SRC)
	gcc -O2 -Wall -Wextra -fsanitize=address,undefined -Istub -I../firmware/main $(SRC) -lm -o $@
clean:
	rm -f test_pc
