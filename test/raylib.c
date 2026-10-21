// The raylib the Makefile builds is the one this build links. A window needs a
// display, which CI has none of, so only what works without one.
#include "raylib.h"

int
main(void)
{
        Color faded = ColorAlpha((Color) { 10, 20, 30, 255 }, 0.5f);

        if (faded.r != 10 || faded.g != 20 || faded.b != 30) {
                return 1;
        }
        if (faded.a != 127) { // 255 * 0.5, truncated
                return 1;
        }
        if (GetRandomValue(7, 7) != 7) {
                return 1;
        }
        return 0;
}
