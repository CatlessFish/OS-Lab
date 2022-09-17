#include <aarch64/intrinsic.h>
#include "kernel/init.h"
#include "driver/uart.h"

static char hello[16];

define_early_init(fun1) {
    char s[] = "Hello world!\n";
    int i = 0;
    do {
        hello[i] = s[i];
    } while (s[i++] != '\0');
    return;
}

define_init(fun2) {
    char* c = hello;
    while (*c != '\0') {
        uart_put_char(*(c++));
    }
    return;
}

NO_RETURN void main()
{
    if (cpuid() != 0) arch_stop_cpu();
    extern char edata[], end[];
    for (char* p = (char*) &edata; p < (char*) &end; p++) *p = 0;
    do_early_init();
    do_init();
    arch_stop_cpu();
}
