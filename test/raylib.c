// The headers and static library the Makefile downloads are the ones this
// build links: a window needs a display, which CI has none of, so only the
// functions that don't open one.
#include <math.h>

#include "raylib.h"

static int
same_color(Color a, Color b)
{
        return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

int
main(void)
{
        Color faded = ColorAlpha((Color){ 10, 20, 30, 200 }, 0.5f);
        Color expected = { 10, 20, 30, 100 };

        if (!same_color(faded, expected)) {
                return 1;
        }
        if (GetRandomValue(7, 7) != 7) {
                return 1;
        }
        if (fabsf(GetFrameTime()) > 60.0f) { // silly, but links raymath-free code
                return 1;
        }
        return 0;
}
