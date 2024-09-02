idf_component_register(SRCS "app_main.c" "ds18b20.c" "cJSON.c" "statereader.c" "counter.c" "temperature/temperatures.c"
                    "flashmem.c" "factoryreset.c" "apwebserver/server.c" "ota/ota.c" "statistics/statistics.c" "device/device.c" INCLUDE_DIRS "."
                    EMBED_TXTFILES ${project_dir}/server_certs/ca_cert.pem)
target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
