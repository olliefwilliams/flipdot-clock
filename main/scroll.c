#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "include/flipdot.h"
#include "include/text.h"
#include "include/fill.h"

static const char text[] = "NOW WITH CMAKE";

static dotboard_t dots;
static int x = DOT_COLUMNS;

void scroll_update()
{
    // Unset the whole board
    fill_off(&dots);

    // Draw some text
    render_text_4x5(&dots, x, 4, text);

    // Write the dotboard repeatedly
    write_dotboard(&dots, false);
    x --;

    // Start scrolling again if we reach the end
    if (x == -(strlen(text) * 5)) {
        x = DOT_COLUMNS;
    }
}
