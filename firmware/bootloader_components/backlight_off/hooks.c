// The backlight pin (GPIO38) floats after reset, which lights the panel while the ST7701S is not
// initialized yet: the screen flashed white at every boot. Drive it low as early as possible.
#include "hal/gpio_ll.h"
#include "soc/gpio_struct.h"
#include "soc/io_mux_reg.h"

#define BACKLIGHT_GPIO 38

void bootloader_hooks_include(void) {}

void bootloader_before_init(void) {
  gpio_ll_func_sel(&GPIO, BACKLIGHT_GPIO, PIN_FUNC_GPIO);
  gpio_ll_set_level(&GPIO, BACKLIGHT_GPIO, 0);
  gpio_ll_output_enable(&GPIO, BACKLIGHT_GPIO);
}
