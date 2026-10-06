#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

/* Se declara en pc_gateway.c — lo definimos como extern aquí */
void console_print(const char *fmt, ...);

/* Macro de depuración – la usarán uart_protocol.c y tu propio código */
#define DEBUG_PRINTF(...)  console_print(__VA_ARGS__)

#endif /* DEBUG_LOG_H */
