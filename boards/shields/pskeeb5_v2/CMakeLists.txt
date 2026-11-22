if(CONFIG_ZMK_DISPLAY_STATUS_SCREEN_CUSTOM)
    target_sources(app PRIVATE custom_status_screen.c)
endif()

# Independent 30s screen blanking, decoupled from the 5min IDLE light sleep.
# Only the display half has a chosen zephyr_display, so gate on CONFIG_ZMK_DISPLAY.
if(CONFIG_ZMK_DISPLAY)
    target_sources(app PRIVATE display_blank.c)
endif()

# Custom behavior that reports mouse speed across the split link.
target_sources(app PRIVATE mouse_speed.c)
