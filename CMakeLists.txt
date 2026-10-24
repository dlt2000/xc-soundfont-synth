cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS
        sampler_synth.c
    INCLUDE_DIRS
        include
    PRIV_INCLUDE_DIRS
        .
    PRIV_REQUIRES
        esp_timer
        esp_common
        heap
        freertos
)
