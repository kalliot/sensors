idf_component_register(SRCS "app_main.c" "ds18b20.c" "cJSON.c" "statereader.c" "counter.c" "temperatures.c"
                    "flashmem.c" INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
