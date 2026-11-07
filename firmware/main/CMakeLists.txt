idf_component_register(SRCS "main.c" "model.c" "model_data.c" "test_images.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_timer heap)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
