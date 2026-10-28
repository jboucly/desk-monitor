# Bootloader hook: keeps the backlight off until the app has drawn its first frame.
idf_component_register(SRCS "hooks.c" REQUIRES hal soc)

# The bootloader hooks are weak symbols: force the linker to keep this file
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")
