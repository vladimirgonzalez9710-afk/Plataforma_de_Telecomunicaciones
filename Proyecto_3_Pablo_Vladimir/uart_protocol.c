#include "uart_protocol.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <time.h>
#include "debug_log.h"          /* DEBUG_PRINTF → console_print() */

int init_serial(const char *port)
{
    int fd = open(port, O_RDWR | O_NOCTTY);
    if (fd < 0) { perror("open"); return -1; }

    struct termios tty;
    if (tcgetattr(fd, &tty) < 0) { perror("tcgetattr"); close(fd); return -1; }

    cfmakeraw(&tty);                       /* 1) modo raw              */
    cfsetspeed(&tty, B9600);               /* 2) 9600-8N1 sin flow     */
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CRTSCTS;               /* sin RTS/CTS              */
    tty.c_iflag &= ~(IXON | IXOFF | IXANY | ICRNL | INLCR);
    tty.c_oflag &= ~OPOST;                 /* sin traducción CR/NL     */
    tty.c_cc[VMIN]  = 1;                   /* 3) bloqueo mínimo 1 byte */
    tty.c_cc[VTIME] = 1;                   /*    timeout 0.1 s         */

    if (tcsetattr(fd, TCSANOW, &tty) < 0) { perror("tcsetattr"); close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

/* ───────────── recepción con ACK/NACK automáticos ───────────── */
int uart_receive_packet(int fd, char *data_out, int *seq_out)
{
    static char buffer[MAX_DATA_LEN + 32];
    static int  buffer_pos = 0;
    char byte;

    int n = read(fd, &byte, 1);
    if (n <= 0) return 0;              /* sin datos */

    buffer[buffer_pos++] = byte;

    if (byte == '\n') {                /* fin de línea → procesar     */
        buffer[buffer_pos] = '\0';
        buffer_pos = 0;

        int seq, crc;  char data[MAX_DATA_LEN];
        if (sscanf(buffer, "%d:%63[^:]:%d", &seq, data, &crc) == 3) {
            if (uart_check_crc(data, crc)) {
                *seq_out = seq; strcpy(data_out, data);
                uart_send_ack(fd, seq);
                return 1;
            } else {
                uart_send_nack(fd, seq);
            }
        }
    }
    return 0;
}

/* ───────────── envío de un paquete de datos ───────────── */
int uart_send_packet(int fd, int seq, const char *data)
{
    char packet[128];
    int crc = uart_calculate_crc(data);
    snprintf(packet, sizeof(packet), "%d:%s:%d\n", seq, data, crc);
    write(fd, packet, strlen(packet));
    return 1;
}

/* ───────────── ACK / NACK ───────────── */
void uart_send_ack(int fd, int seq)
{
    char buf[64];
    int crc = uart_calculate_crc("ACK");
    int len = snprintf(buf, sizeof(buf), "%d:ACK:%d\n", seq, crc);

    DEBUG_PRINTF("[DEBUG] Enviando ACK UART: %s", buf);  /* log seguro */
    write(fd, buf, len);
    tcdrain(fd);                       /* espera transmisión completa */
}

void uart_send_nack(int fd, int seq)
{
    char msg[64];
    int crc = uart_calculate_crc("NACK");
    int len = snprintf(msg, sizeof(msg), "%d:NACK:%d\n", seq, crc);

    DEBUG_PRINTF("[DEBUG] Enviando NACK UART: %s", msg);
    write(fd, msg, len);
    tcdrain(fd);
}

/* ───────────── utilidades CRC ───────────── */
int uart_calculate_crc(const char *data)
{
    int crc = 0;
    for (int i = 0; data[i] != '\0'; i++) crc += data[i];
    return crc % 256;
}

int uart_check_crc(const char *data, int expected_crc)
{
    return uart_calculate_crc(data) == expected_crc;
}

/* ───────────── trigger de error (ejemplo) ───────────── */
void uart_trigger_error(int fd)
{
    write(fd, "ALARM\n", 6);
}
