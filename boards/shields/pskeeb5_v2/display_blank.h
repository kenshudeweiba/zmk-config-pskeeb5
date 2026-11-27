/*
 * Shared panel-power state for the pskeeb5 LEFT half.
 *
 * display_blank.c is the single authority for the OLED power state. The
 * custom status screen reads it in its flush callback so it can skip the
 * expensive full-frame transpose + SPI write while the panel is blanked.
 */
#pragma once

#include <stdbool.h>

/* True when the panel is expected to be powered on. */
bool pskeeb5_panel_is_on(void);
