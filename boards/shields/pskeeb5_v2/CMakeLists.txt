if(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)
    target_sources(app PRIVATE custom_status_screen.c)
endif()

# Custom behavior that reports mouse speed across the split link.
target_sources(app PRIVATE mouse_speed.c)
