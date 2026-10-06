/*  pc_gateway.c  –  PC ↔ AWS ↔ LoRa  (versión ncurses) ------------- */
#include "uart_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <arpa/inet.h>
#include <termios.h>
#include <time.h>
#include <stdarg.h>
#include <ncurses.h>          /* Para poner la pantalla en modo texto */
#include "debug_log.h"
#include <stdbool.h>     

#define SERIAL_PORT   "/dev/ttyUSB0"
#define PACKET_SIZE          32
#define SERVER_IP     "3.16.78.13"
#define SERVER_PORT          8080
#define BUFFER_SIZE        2048
#define MAX_MSG_LEN      65536

/* ═══════════  SINCRONIZACIÓN DE SALIDA  ═══════════ */
/* Protege accesos concurrentes a la pantalla (ncurses). */
static pthread_mutex_t term_lock = PTHREAD_MUTEX_INITIALIZER;

/* ═══════════  VENTANAS NCURSES  ═══════════ */
static WINDOW *logw  = NULL;   /* ventana con scroll para mensajes */
static WINDOW *barw  = NULL;   /* ventana inferior: barra de progreso  */

/* ═══════════  BARRA DE PROGRESO  ═══════════ */
#define PB_WIDTH 50
static char bar_tag[32] = "PC->LoRa";  /* etiqueta encima de la barra */
static int  bar_cur  = 0;              /* posición actual */
static int  bar_tot  = 1;              /* total de pasos */
int bar_visible = 0;                   /* indica si la barra está activa */

/* ----------------------------------------------------------------------------
 * draw_bar()
 *   Dibuja la barra de progreso en la ventana inferior usando bar_tag/bar_cur/bar_tot.
 * ----------------------------------------------------------------------------
 */
static void draw_bar(void)
{
    werase(barw);
    int pct    = (bar_cur * 100) / bar_tot;
    int filled = (bar_cur * PB_WIDTH) / bar_tot;

    wprintw(barw, "%s [", bar_tag);
    for (int i = 0; i < PB_WIDTH; ++i)
        waddch(barw, i < filled ? '#' : ' ');
    wprintw(barw, "] %3d%%", pct);
    wrefresh(barw);
}

/* ----------------------------------------------------------------------------
 * print_progress(tag, cur, total)
 *   Actualiza las variables de la barra y la redibuja de manera segura.
 * ----------------------------------------------------------------------------
 */
static void print_progress(const char *tag, int cur, int total)
{
    pthread_mutex_lock(&term_lock);

    strncpy(bar_tag, tag, sizeof(bar_tag)-1);
    bar_tag[sizeof(bar_tag)-1] = '\0';
    bar_cur = cur;
    bar_tot = total <= 0 ? 1 : total;

    draw_bar();

    pthread_mutex_unlock(&term_lock);
}

/* ----------------------------------------------------------------------------
 * console_print(fmt, ...)
 *   Escribe un mensaje formateado en la ventana de log y refresca la pantalla.
 * ----------------------------------------------------------------------------
 */
void console_print(const char *fmt, ...)
{
    pthread_mutex_lock(&term_lock);

    va_list ap; va_start(ap, fmt);
    vw_printw(logw, fmt, ap);
    va_end(ap);
    wrefresh(logw);

    draw_bar();  /* vuelve a pintar la barra */

    pthread_mutex_unlock(&term_lock);
}

/* ═══════════  VARIABLES GLOBALES  ═══════════ */
static int uart_fd = -1;  /* descriptor del puerto serie LoRa */
static int sockfd  = -1;  /* descriptor del socket TCP hacia AWS */

/* ---- coordinación ACK/NACK (UART) ----------- */
static int uart_ack_received = -1;
pthread_mutex_t uart_ack_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  uart_ack_cond  = PTHREAD_COND_INITIALIZER;

/* ---- DEST para reconstruir prefijos de destino  */
typedef enum { DEST_NONE, DEST_A, DEST_B, DEST_ALL } dest_t;

/* ----------------------------------------------------------------------------
 * send_all(fd, buf, len)
 *   Asegura enviar todo el buffer por TCP, reintentando mientras haya datos.
 * ----------------------------------------------------------------------------
 */
static void send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t s = send(fd, buf + off, len - off, 0);
        if (s <= 0) break;
        off += (size_t)s;
    }
}

/* ----------------------------------------------------------------------------
 * read_line(fd, buffer, maxlen)
 *   Lee del descriptor 'fd' hasta '\n' o hasta maxlen-1. Retorna longitud.
 * ----------------------------------------------------------------------------
 */
static int read_line(int fd, char *buffer, int maxlen)
{
    int idx = 0; char ch;
    while (idx < maxlen - 1) {
        int n = read(fd, &ch, 1);
        if (n > 0) {
            if (ch == '\n') { buffer[idx] = '\0'; return idx; }
            buffer[idx++] = ch;
        } else usleep(5000);
    }
    buffer[idx] = '\0';
    return idx;
}

/* ═══════════  HILO: UART → AWS (TCP)  ═══════════
 *
 * uart_reader_thread:
 *   - Recibe líneas desde el LoRa (UART) fragmentado con seq:data:crc.
 *   - Maneja SYN/TOTAL/FIN para sincronizar transferencia.
 *   - Reensambla texto o imágenes, y reenvía al AWS por TCP.
 */
void* uart_reader_thread(void* arg)
{
    int   total_rx_pkts = -1;      /* número total de paquetes esperados */
    int   rx_pkts_seen  = 0;       /* contador de paquetes válidos */

    char  line[512];
    char  msg_buf[MAX_MSG_LEN];
    int   msg_len = 0, expected_seq = 0;
    dest_t current_dest = DEST_NONE;

    bool  streaming_img = false;   /* flag: estamos transitando imagen */
    char  dest_prefix[10] = "";    /* guarda "DEST:A:" etc. para reenviar */

    while (1) {
        /* Leer línea de LoRa */
        if (read_line(uart_fd, line, sizeof(line)) <= 0)
            continue;
        console_print("[UART] %s\n", line);

        /* Captura del resultado de LINKTEST y reenvío inmediato */
        if (strncmp(line, "LINKTEST RESULT", 15) == 0) {
            send_all(sockfd, line, strlen(line));
            send_all(sockfd, "\n", 1);
            console_print("[PC] Reenviado a AWS: %s\n", line);
            continue;
        }

        /* Protocolo de sincronización */
        if (!strcmp(line, "SYN")) {
            expected_seq  = 0; msg_len = 0; current_dest = DEST_NONE;
            total_rx_pkts = -1; rx_pkts_seen = 0;
            streaming_img = false; dest_prefix[0]= '\0';
            continue;
        }
        if (!strncmp(line, "TOTAL:", 6)) {
            total_rx_pkts = atoi(line + 6);
            rx_pkts_seen  = 0;
            continue;
        }
        if (!strcmp(line, "FIN")) {
            /* Al acabar, reenviar o cerrar barra */
            if (!streaming_img) {
                /* reenviar texto completo */
                char out[MAX_MSG_LEN+16]; int out_len=0;
                if (current_dest==DEST_A)
                    out_len = snprintf(out,sizeof(out),"DEST:A:%.*s",msg_len,msg_buf);
                else if (current_dest==DEST_B)
                    out_len = snprintf(out,sizeof(out),"DEST:B:%.*s",msg_len,msg_buf);
                else if (current_dest==DEST_ALL)
                    out_len = snprintf(out,sizeof(out),"DEST:ALL:%.*s",msg_len,msg_buf);
                else { memcpy(out,msg_buf,msg_len); out_len = msg_len; }
                send_all(sockfd,out,(size_t)out_len);
            }
            if (total_rx_pkts>0 && rx_pkts_seen==total_rx_pkts)
                print_progress("LoRa->PC",total_rx_pkts,total_rx_pkts);
            /* reset estado */
            expected_seq=0; msg_len=0; current_dest=DEST_NONE;
            total_rx_pkts=-1; rx_pkts_seen=0; streaming_img=false;
            dest_prefix[0]='\0';
            continue;
        }

        /* Decodificar seq:data:crc */
        char *p_crc = strrchr(line, ':'); if (!p_crc) continue;
        int crc = atoi(p_crc + 1); *p_crc = '\0';
        char *p_col = strchr(line, ':'); if (!p_col) continue;
        *p_col = '\0';
        int seq = atoi(line);
        char *data = p_col + 1;

        /* ACK/NACK LoRa */
        if (!strcmp(data,"ACK") || !strcmp(data,"NACK")) {
            pthread_mutex_lock(&uart_ack_mutex);
            uart_ack_received = seq;
            pthread_cond_signal(&uart_ack_cond);
            pthread_mutex_unlock(&uart_ack_mutex);
            continue;
        }

        /* Validar CRC y reenviar ACK/NACK */
        if (!uart_check_crc(data, crc)) {
            uart_send_nack(uart_fd, seq);
            continue;
        }
        uart_send_ack(uart_fd, seq);

        /* Descartar fuera de orden */
        if (seq != expected_seq) continue;
        expected_seq++;

        /* Actualizar barra de recepción */
        if (total_rx_pkts > 0) {
            rx_pkts_seen++;
            if (!bar_visible) bar_visible = 1;
            print_progress("LoRa->PC", rx_pkts_seen, total_rx_pkts);
        }

        /* Detectar DEST en primer paquete */
        if (seq == 0) {
            if (!strncmp(data,"DEST:A:",7)) {
                current_dest=DEST_A; strcpy(dest_prefix,"DEST:A:"); data+=7;
            } else if (!strncmp(data,"DEST:B:",7)) {
                current_dest=DEST_B; strcpy(dest_prefix,"DEST:B:"); data+=7;
            } else if (!strncmp(data,"DEST:ALL:",9)) {
                current_dest=DEST_ALL; strcpy(dest_prefix,"DEST:ALL:"); data+=9;
            }
            if (!strncmp(data,"IMG:",4)) {
                streaming_img = true;
                char hdr[512];
                int hlen = snprintf(hdr,sizeof(hdr),"%s%s",dest_prefix,data);
                send_all(sockfd,hdr,(size_t)hlen);
                continue;
            }
        }

        /* Si es fragmento de imagen */
        if (streaming_img) {
            send_all(sockfd,data,strlen(data));
            continue;
        }

        /* Texto normal: acumular */
        int plen = strlen(data);
        if (msg_len + plen < MAX_MSG_LEN) {
            memcpy(msg_buf + msg_len, data, plen);
            msg_len += plen;
        } else {
            console_print("[ERROR] msg_buf overflow\n");
        }
    }
    return NULL;
}

/* ═══════════  ENVÍO FRAGMENTADO  ═══════════
 *
 * uart_send_text_fragments:
 *   Toma un string (posiblemente con múltiples líneas/HEX) y lo
 *   fragmenta en paquetes LoRa con SYN/TOTAL/FIN, respetando MAX_RETRIES.
 */
static void uart_send_text_fragments(const char *mensaje)
{
    /* ---------- LIMPIAR \r\n ---------- */
    char *clean = strdup(mensaje);
    for (char *p = clean; *p; ++p)
        if (*p == '\r' || *p == '\n') *p = ' ';

    /* ---------- NUEVO: detectar destino A/B (para FAIL) ---------- */
    char dest_id = 'A';                              /* valor por defecto */
    if      (!strncmp(clean, "DEST:A:", 7))  dest_id = 'A';
    else if (!strncmp(clean, "DEST:B:", 7))  dest_id = 'B';
    else if (!strncmp(clean, "DEST:ALL:", 9)) dest_id = 'A'; /* avisar a ambas */

    /* ---------- CALCULAR FRAGMENTOS ---------- */
    int total_pkts = (strlen(clean) + PACKET_SIZE - 1) / PACKET_SIZE;
    write(uart_fd, "SYN\n", 4);  usleep(100000);
    char tmsg[32];  snprintf(tmsg, sizeof(tmsg), "TOTAL:%d\n", total_pkts);
    write(uart_fd, tmsg, strlen(tmsg));  usleep(100000);

    /* Reset ACK y barra */
    pthread_mutex_lock(&uart_ack_mutex);
    uart_ack_received = -1;
    pthread_mutex_unlock(&uart_ack_mutex);
    print_progress("PC->LoRa", 0, total_pkts);

    /* ---------- ENVÍO CON REINTENTOS ---------- */
    for (int i = 0; i < total_pkts; i++) {

        int off = i * PACKET_SIZE;
        int rem = strlen(clean) - off;
        int len = rem < PACKET_SIZE ? rem : PACKET_SIZE;

        char datos[PACKET_SIZE + 1];
        memcpy(datos, clean + off, len);
        datos[len] = '\0';

        int attempts = 0, acked = 0;
        while (attempts < 5 && !acked) {
            uart_send_packet(uart_fd, i, datos);

            /* esperar ACK con timeout */
            struct timespec dl;  clock_gettime(CLOCK_REALTIME, &dl);
            dl.tv_sec += 2;                      /* TIMEOUT de 2 s */

            pthread_mutex_lock(&uart_ack_mutex);
            while (uart_ack_received != i) {
                if (pthread_cond_timedwait(&uart_ack_cond,
                                            &uart_ack_mutex, &dl) == ETIMEDOUT)
                    break;
            }
            if (uart_ack_received == i) {
                acked = 1;
                uart_ack_received = -1;
            }
            pthread_mutex_unlock(&uart_ack_mutex);

            if (!acked) {
                attempts++;
                console_print("[PC] Reintentando paquete %d (int %d/5)\n",
                              i, attempts);
                usleep(250000);
            }
        }

        /* ---------- NUEVO: si fracasa tras 5 intentos, avisar FAIL ---------- */
        if (!acked) {
            console_print("[ERROR] Paquete %d sin ACK\n", i);

            if (sockfd > 0) {                   /* socket TCP hacia AWS */
                char fail[128];
                int m = snprintf(fail, sizeof(fail),
                                 "DEST:%c:FAIL:No se pudo entregar el mensaje\n",
                                 dest_id);
                send_all(sockfd, fail, (size_t)m);   /* reenvía aviso */
            }
            break;      /* aborta transmisión */
        }

        /* actualizar barra */
        print_progress("PC->LoRa", i + 1, total_pkts);
        usleep(80000);
    }

    write(uart_fd, "FIN\n", 4);
    free(clean);
}


/* ═══════════  HILO: AWS → UART  ═══════════
 *
 * aws_to_uart_thread:
 *   Recibe del AWS por TCP y reenvía por UART al LoRa (texto o imágenes).
 */
void* aws_to_uart_thread(void* arg)
{
    char buffer[BUFFER_SIZE];
    static char img_buf[MAX_MSG_LEN];
    size_t img_len = 0;
    int in_img = 0;
    char dest_prefix[16] = "";

    while (1) {
        int n = recv(sockfd, buffer, sizeof(buffer), 0);
        if (n <= 0) break;
        buffer[n] = '\0';

        /* 0) Extraer opcionalmente el prefijo DEST:XX: si aún no estamos en imagen */
        char *msg = buffer;
        dest_prefix[0] = '\0';
        if (!in_img) {
            if (!strncmp(msg, "DEST:A:", 7)) {
                strcpy(dest_prefix, "DEST:A:");
                msg += 7;
            } else if (!strncmp(msg, "DEST:B:", 7)) {
                strcpy(dest_prefix, "DEST:B:");
                msg += 7;
            } else if (!strncmp(msg, "DEST:ALL:", 9)) {
                strcpy(dest_prefix, "DEST:ALL:");
                msg += 9;
            }
        }

        /* 1) Detectar inicio de imagen */
        if (!in_img && !strncmp(msg, "IMG:", 4)) {
            in_img = 1;
            img_len = 0;
            /* Reconstruir cabecera con DEST:XX:IMG:... */
            int h = snprintf(img_buf, sizeof(img_buf), "%s%s", dest_prefix, msg);
            img_len = (h > 0 ? (size_t)h : 0);
            continue;
        }

        /* 2) Acumular fragmentos de imagen */
        if (in_img) {
            int dlen = strlen(msg);
            if (img_len + dlen > MAX_MSG_LEN) {
                console_print("[PC] Imagen muy grande, descarto\n");
                in_img = 0;
                img_len = 0;
                continue;
            }
            memcpy(img_buf + img_len, msg, dlen);
            img_len += dlen;

            /* 3) Cuando encontremos ENDIMG, enviamos todo */
            char *endp = memmem(img_buf, img_len, "ENDIMG", 6);
            if (endp) {
                {
                    size_t tot_img  = (endp - img_buf) + 6;
                    /* Null-terminate justo tras “ENDIMG” */
                    img_buf[tot_img] = '\0';
                    uart_send_text_fragments(img_buf);

                    /* Si hay datos tras ENDIMG, también los reenviamos */
                    size_t rest_img = img_len - tot_img;
                    if (rest_img) {
                        char tmp[rest_img + 1];
                        memcpy(tmp, img_buf + tot_img, rest_img);
                        tmp[rest_img] = '\0';
                        uart_send_text_fragments(tmp);
                    }
                }
                in_img  = 0;
                img_len = 0;
            }
            continue;
        }

        /* 4) Texto normal: fragmentarlo y enviarlo */
        uart_send_text_fragments(buffer);
    }

    return NULL;
}


/* ═══════════  MAIN  ═══════════
 *
 * main:
 *   - Inicializa ncurses.
 *   - Abre UART y socket hacia AWS.
 *   - Lanza hilos de lectura/escritura bidireccional.
 */
int main(void)
{
    /* ---- ncurses init ---- */
    initscr(); cbreak(); noecho();
    int rows, cols; getmaxyx(stdscr, rows, cols);
    logw = newwin(rows-1, cols, 0, 0);
    barw = newwin(1, cols, rows-1, 0);
    scrollok(logw, TRUE);

    /* ---- inicializar UART ---- */
    uart_fd = init_serial(SERIAL_PORT);
    if (uart_fd < 0) {
        endwin(); perror("init_serial"); return 1;
    }

    /* ---- conectar a AWS ---- */
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv = {0};
    serv.sin_family = AF_INET;
    serv.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &serv.sin_addr);

    console_print("[PC] Conectando a AWS %s:%d ...\n", SERVER_IP, SERVER_PORT);
    if (connect(sockfd, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
        endwin(); perror("connect"); return 1;
    }
    console_print("[PC] Conectado al servidor AWS \n");

    /* ---- lanzar hilos ---- */
    pthread_t th_uart, th_aws;
    pthread_create(&th_uart, NULL, uart_reader_thread, NULL);
    pthread_create(&th_aws , NULL, aws_to_uart_thread , NULL);

    /* ---- esperar finalización ---- */
    pthread_join(th_uart, NULL);
    pthread_join(th_aws , NULL);

    /* ---- cleanup ---- */
    close(sockfd);
    close(uart_fd);
    endwin();  /* restaura la terminal */
    return 0;
}
