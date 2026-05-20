/* Stubs for symbols defined in src/main.c. The test build excludes main.c
 * because it owns main(); these unused references keep the linker happy. */

#include <stdio.h>
#include "types.h"
#include "main_app.h"

void show_usage(int argc, char* argv[])
{
    (void)argc; (void)argv;
    fprintf(stderr, "[test stub] show_usage called\n");
}

U32 get_disk_info(char* device)
{
    (void)device;
    return 0;
}

void show_result(struct tm* tm_info)
{
    (void)tm_info;
}

void sig_handler(int signo)
{
    (void)signo;
}
