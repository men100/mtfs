/* T-Monitor transport for the shared diagnostic console parser. */
#include "mtfs_console_tmonitor.h"

#include <tm/tmonitor.h>
#include <mtkernel/lib/libtm/libtm.h>

void mtfs_console_tmonitor_write(void *context, const char *text)
{
    const UB *start = (const UB *)text;
    INT length = 0;

    (void)context;
    if (text == NULL) {
        return;
    }
    while (text[length] != '\0') {
        ++length;
    }

    /* Send the application's CR/LF bytes unchanged. tm_putchar() rewrites LF. */
    if (length != 0) {
        tm_snd_dat(start, length);
    }
}

int mtfs_console_tmonitor_getchar(void)
{
    return tm_getchar(1);
}
