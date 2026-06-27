file(GLOB_RECURSE SOURCES
    "Source/*.c"
    "Source/*.cpp"
)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS "Source"
    REQUIRES
        tactility
        tactility_lvgl
        lvgl
        freertos
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
