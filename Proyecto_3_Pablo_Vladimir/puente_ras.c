// ================================
// puente_raspberry.c
// ================================
// Raspi ↔ LoRa  (UART)  +  Raspi ↔ Celular (TCP)
// --------------------------------------------------

#include "uart_protocol.h"

#include <stdio.h>         
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <ncurses.h>
#include <time.h>
#include <stdbool.h>

#include "debug_log.h"     

#undef  printf               
#define printf  DEBUG_PRINTF 
#define LOSS_PCT 30   // porcentaje de pérdida deseado (0-100)

#define SERIAL_PORT "/dev/ttyUSB0"
#define PORT        8022
#define BUFFER_SIZE 2048
#define PACKET_SIZE 32
#define MAX_RETRIES 5
#define TIMEOUT     2
#define MAX_MSG_LEN 4096
typedef enum { DEST_NONE, DEST_A, DEST_B, DEST_ALL } dest_t;
/* ═════ GLOBAL: sincronización de salida + ncurses ventanas ═════ */
static pthread_mutex_t term_lock = PTHREAD_MUTEX_INITIALIZER;
static WINDOW *logw = NULL, *barw = NULL;
int bar_visible = 0;                       

/* ═════ barra de progreso (compartida) ═════ */
#define PB_WIDTH 50
static char bar_tag[32] = "Raspi->LoRa";
static int  bar_cur  = 0, bar_tot = 1;

static void run_linktest(int n_pkts);

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

void print_progress(const char *tag, int cur, int total)
{
    pthread_mutex_lock(&term_lock);

    strncpy(bar_tag, tag, sizeof(bar_tag)-1);
    bar_tag[sizeof(bar_tag)-1] = '\0';
    bar_cur = cur;
    bar_tot = total <= 0 ? 1 : total;

    draw_bar();
    bar_visible =1;       

    pthread_mutex_unlock(&term_lock);
}

/* ═════ impresión segura ═════ */
void console_print(const char *fmt, ...)
{
    pthread_mutex_lock(&term_lock);

    va_list ap; va_start(ap, fmt);
    vw_printw(logw, fmt, ap);
    va_end(ap);
    wrefresh(logw);

    if (bar_visible) draw_bar();
    pthread_mutex_unlock(&term_lock);
}

/* ═════ globales específicas del puente ═════ */
int fd;          // UART
int client_fd;   // socket TCP celular

pthread_mutex_t ack_mutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  ack_cond    = PTHREAD_COND_INITIALIZER;
int current_tx_seq = -1;
int ack_received   = -1;

/* ───────── linea UART ───────── */
int read_line(int fd, char *buffer, int maxlen)
{
    int idx = 0; char ch;
    while (idx < maxlen - 1) {
        int n = read(fd, &ch, 1);
        if (n <= 0) { usleep(10000); continue; }
        if (ch == '\n') { buffer[idx] = '\0'; return idx; }
        buffer[idx++] = ch;
    }
    buffer[idx] = '\0'; return idx;
}

static void run_linktest(int n_pkts) {
    int sent = n_pkts, ok = 0, lost = 0;

    for (int i = 0; i < n_pkts; ++i) {
        
        if (rand() % 100 < LOSS_PCT) {
            console_print("[Puente] LINKTEST: simulando pérdida pkt %d\n", i);
            lost++;
            print_progress("LINKTEST", i+1, n_pkts);
            usleep(20000);
            continue;
        }
        
        int retries = 0;
        bool success = false;

        /* Resetear ACK pendiente */
        pthread_mutex_lock(&ack_mutex);
        ack_received = -1;
        pthread_mutex_unlock(&ack_mutex);

        /* Intentar envío con reintentos */
        while (retries < MAX_RETRIES && !success) {
            uart_send_packet(fd, i, "");  // paquete vacío de prueba

            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += TIMEOUT;

            pthread_mutex_lock(&ack_mutex);
            int rc = 0;
            while (ack_received != i && rc != ETIMEDOUT)
                rc = pthread_cond_timedwait(&ack_cond, &ack_mutex, &ts);
            success = (ack_received == i);
            /* limpiar ACK para siguiente iteración */
            ack_received = -1;
            pthread_mutex_unlock(&ack_mutex);

            if (!success) {
                retries++;
                console_print("[Puente] LINKTEST: reintentando pkt %d (%d/%d)\n",
                              i, retries, MAX_RETRIES);
                sleep(100000);
            }
        }

        if (success) ++ok; else ++lost;
        /* Actualizar barra local de progreso */
        print_progress("LINKTEST", i+1, n_pkts);
        usleep(10000);
    }

    /* Construir resultado */
    char res[128];
    int len = snprintf(res, sizeof(res),
                       "LINKTEST RESULT sent=%d ok=%d lost=%d\n",
                       sent, ok, lost);

    /* Enviar al celular (TCP) */
    if (client_fd > 0)
        send(client_fd, res, len, 0);

    /* Enviar a la PC (UART) */
    write(fd, res, len);
}


/* ═══════════  HILO: UART → TCP (celular)  ═══════════ */
void* uart_reader_thread(void* arg)
{
    int   total_rx_pkts = -1;          /* valor de TOTAL:x si llega          */
    char  line[512];

    char  msg_buf[MAX_MSG_LEN];        /* reconstrucción para DEST:…         */
    int   msg_len = 0, expected_seq = 0;
    dest_t current_dest = DEST_NONE;

    while (1) {

        /* Leer línea desde UART */
        if (read_line(fd, line, sizeof(line)) <= 0) { usleep(10000); continue; }
        console_print("[UART→Raspi] %s\n", line);

        /* ─── Cabeceras LoRa ─── */
        if (!strcmp(line, "SYN")) {
            expected_seq  = 0;
            msg_len       = 0;
            current_dest  = DEST_NONE;
            total_rx_pkts = -1;
            continue;
        }

        if (!strncmp(line, "TOTAL:", 6)) {
            total_rx_pkts = atoi(line + 6);
            print_progress("LoRa->Raspi", 0, total_rx_pkts);   /* barra 0 % */
            continue;
        }

        if (!strcmp(line, "FIN")) {
            /* Enviar FIN al celular */
            if (client_fd > 0) send(client_fd, "FIN\n", 4, 0);

            /* Barra al 100 % si estaba activa */
            if (total_rx_pkts > 0)
                print_progress("LoRa->Raspi", total_rx_pkts, total_rx_pkts);

            /* Reinicio de estado */
            expected_seq  = 0;
            msg_len       = 0;
            current_dest  = DEST_NONE;
            total_rx_pkts = -1;
            continue;
        }

        /* ─── Decodificar seq:data:crc ─── */
        char *p_crc = strrchr(line, ':');      if (!p_crc) continue;
        int  crc_recv = atoi(p_crc + 1);       *p_crc = '\0';

        char *p_col = strchr(line, ':');       if (!p_col) continue;
        *p_col = '\0';
        int  seq  = atoi(line);
        char *data = p_col + 1;

        /* ACK / NACK que llegan de LoRa */
        if (!strcmp(data,"ACK") || !strcmp(data,"NACK")) {
            pthread_mutex_lock(&ack_mutex);
            ack_received = (!strcmp(data,"ACK")) ? seq : -2;
            pthread_cond_signal(&ack_cond);
            pthread_mutex_unlock(&ack_mutex);
            continue;
        }

        /* Validar CRC y responder ACK propio */
        if (uart_calculate_crc(data) != crc_recv) { uart_send_nack(fd, seq); continue; }
        uart_send_ack(fd, seq);

        /* Descartar paquetes fuera de orden */
        if (seq != expected_seq) continue;
        expected_seq++;

        /* REENVÍO INMEDIATO al celular (con \n) */
        if (client_fd > 0) {
            char out[520];
            int nbytes = snprintf(out, sizeof(out), "%s\n", data);
            send(client_fd, out, nbytes, 0);
        }

        /* Actualizar barra de recepción */
        if (total_rx_pkts > 0)
            print_progress("LoRa->Raspi", seq + 1, total_rx_pkts);

        /* Primera trama: averiguamos DEST */
        if (seq == 0) {
            if      (!strncmp(data,"DEST:A:",7)){ current_dest = DEST_A; data += 7; }
            else if (!strncmp(data,"DEST:B:",7)){ current_dest = DEST_B; data += 7; }
            else if (!strncmp(data,"DEST:ALL:",9)){ current_dest = DEST_ALL; data += 9; }
        }

        /* Copiar payload al buffer grande */
        int plen = (int)strlen(data);
        if (msg_len + plen < MAX_MSG_LEN) {
            memcpy(msg_buf + msg_len, data, plen);
            msg_len += plen;
        } else {
            console_print("[ERROR] msg_buf overflow\n");
        }
    }
    return NULL;
}


/* ═════════ hilo TCP (celular) → UART ═════════ */
void* tcp_listener_thread(void* arg)
{
    /* socket servidor */
    int server_fd = socket(AF_INET,SOCK_STREAM,0);
    if (server_fd<0){ console_print("[Puente] socket error\n"); exit(1); }
    int opt=1; setsockopt(server_fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));

    struct sockaddr_in serv={0}, cli; socklen_t clilen=sizeof(cli);
    serv.sin_family=AF_INET; serv.sin_addr.s_addr=INADDR_ANY; serv.sin_port=htons(PORT);
    if (bind(server_fd,(struct sockaddr*)&serv,sizeof(serv))<0){
        console_print("[Puente] bind error\n"); exit(1);
    }
    listen(server_fd,1);
    console_print("[Puente] Esperando conexión en %d…\n",PORT);
    client_fd = accept(server_fd,(struct sockaddr*)&cli,&clilen);
    console_print("[Puente] Celular conectado \n");

    char buffer[BUFFER_SIZE];
    char total_buf[BUFFER_SIZE*10];
    int  total_len=0;

    while (1) {
        int n=recv(client_fd,buffer,sizeof(buffer),0);
        if (n<=0){ console_print("[Puente] TCP cerrado\n"); break; }

        /* ——— Interceptar petición de prueba de enlace ——— */
        if (strncmp(buffer, "CMD:LINKTEST:", 13) == 0) {
            int n_pkts = atoi(buffer + 13);
            console_print("[Puente] Iniciando LINKTEST de %d pkts…\n", n_pkts);
            run_linktest(n_pkts);
            continue;
        }


        memcpy(total_buf+total_len,buffer,n); total_len+=n;
        int total_pkts=(total_len+PACKET_SIZE-1)/PACKET_SIZE;

        /* inicio de protocolo */
        write(fd,"SYN\n",4); sleep(1);
        char tmsg[32]; snprintf(tmsg,sizeof(tmsg),"TOTAL:%d\n",total_pkts);
        write(fd,tmsg,strlen(tmsg)); sleep(1);

        print_progress("Raspi->LoRa",0,total_pkts);

        for (int i=0;i<total_pkts;i++){
            int off=i*PACKET_SIZE, remain=total_len-off;
            int chunk=remain<PACKET_SIZE?remain:PACKET_SIZE;

            char datos[PACKET_SIZE+1]={0};
            memcpy(datos,total_buf+off,chunk); datos[chunk]='\0';
            for(int j=0;j<chunk;j++) if(datos[j]=='\n'||datos[j]=='\r') datos[j]=' ';

            int retries=0,ok=0;
            while(retries<MAX_RETRIES && !ok){
                current_tx_seq=i;
                uart_send_packet(fd,i,datos);

                struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); ts.tv_sec+=TIMEOUT;
                pthread_mutex_lock(&ack_mutex);
                int rc=0;
                while(ack_received!=i && rc!=ETIMEDOUT)
                    rc=pthread_cond_timedwait(&ack_cond,&ack_mutex,&ts);
                ok=(ack_received==i); ack_received=-1;
                pthread_mutex_unlock(&ack_mutex);

                if(!ok){
                    retries++;
                    console_print("[Puente] Reintentando %d (%d/%d)\n",
                                   i,retries,MAX_RETRIES);
                    sleep(1);
                }
            }
        }
            if (!ok) {
                console_print("[Puente] ERROR paquete %d\n", i);

                /* ⬇️ NUEVO: avisar al celular que el mensaje NO se entregó */
                if (client_fd > 0)
                    send(client_fd, "FAIL:Entrega LoRa\n", 18, 0);

                break;          /* aborta el resto del envío */
            }
            print_progress("Raspi->LoRa",i+1,total_pkts);
            usleep(100000);
        }
        write(fd,"FIN\n",4);
        total_len=0;
    }

    close(client_fd); close(server_fd);
    return NULL;
}

/* ═════════ MAIN ═════════ */
int main(void)
{ 
      srand(time(NULL));
    /* ncurses */
    initscr(); cbreak(); noecho();
    int rows,cols; getmaxyx(stdscr,rows,cols);
    logw=newwin(rows-1,cols,0,0); barw=newwin(1,cols,rows-1,0);
    scrollok(logw,TRUE);

    fd = init_serial(SERIAL_PORT);
    if(fd<0){ endwin(); perror("init_serial"); return 1; }

    pthread_t th_uart, th_tcp;
    pthread_create(&th_uart,NULL,uart_reader_thread,NULL);
    pthread_create(&th_tcp ,NULL,tcp_listener_thread,NULL);

    pthread_join(th_tcp ,NULL); /* sale al cerrar conexión */
    close(fd);
    endwin();
    return 0;
}
