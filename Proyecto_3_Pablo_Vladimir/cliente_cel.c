// cliente_celular_saldo.c  –  Celular ↔ Raspberry ↔ AWS
// ===============================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <ctype.h>
#include <errno.h>
#include <ctype.h>   

#define SERVER_IP       "192.168.1.57"
#define SERVER_PORT     8022
#define BUFFER_SIZE     1024
#define IMG_CHUNK_RAW   100          /* bytes crudos leídos por fread */
#define MAX_NAME_LEN    256
#define BAR_W           50           /* ancho de la barra enlace */
#define LINE_MAX      4096
/* -------------------- estado global ---------------------------- */
static int   sockfd;
static volatile int  running     = 1;
static volatile long saldo_local = 0;
static int   overdraft_used      = 0;

/* -------------------- utilidades ------------------------------- */
static unsigned char hex2byte(char hi, char lo) {
    unsigned char v = 0;
    if (isxdigit(hi))
        v = (isdigit(hi) ? hi - '0' : tolower(hi) - 'a' + 10) << 4;
    if (isxdigit(lo))
        v |= (isdigit(lo) ? lo - '0' : tolower(lo) - 'a' + 10);
    return v;
}

/* ——— Función añadida para mostrar la barra de enlace ——— */
static void show_link_bar(int sent, int ok, int lost) {
    int pct_ok = (sent ? (ok * 100) / sent : 0);
    int fill   = (pct_ok * BAR_W) / 100;

    puts("\n[RESULTADO PRUEBA ENLACE]");
    putchar('[');
    for (int i = 0; i < BAR_W; ++i)
        putchar(i < fill ? '#' : '-');
    printf("] %d %% éxito  (Ok=%d  Perdidos=%d  Total=%d)\n\n",
           pct_ok, ok, lost, sent);
}

/* =================================================================
 *  HILO DE RECEPCIÓN
 * =================================================================*/#include <ctype.h>   /* isxdigit() */
static void *recibir(void *arg)
{
    char  buffer[BUFFER_SIZE];
    FILE *img_file = NULL;
    int   in_img   = 0;                   /* estado imagen */

    /* ─── acumulador para mensajes de texto largos ─── */
    static char line_buf[LINE_MAX];
    static int  line_len = 0;

    while (running) {
        int n = recv(sockfd, buffer, sizeof(buffer), 0);
        if (n <= 0) { usleep(80000); continue; }

        /* recorta CR/LF del final del fragmento */
        while (n > 0 && (buffer[n-1] == '\n' || buffer[n-1] == '\r'))
            n--;
        buffer[n] = '\0';

        /* ───────────────── 0) RESULTADOS DE LINKTEST ───────────────── */
        if (!in_img && strncmp(buffer, "LINKTEST RESULT", 15) == 0) {
            int sent = 0, ok = 0, lost = 0;
            sscanf(buffer, "LINKTEST RESULT sent=%d ok=%d lost=%d",
                   &sent, &ok, &lost);
            show_link_bar(sent, ok, lost);
            continue;
        }

        /* ───────────────── 1) SALDO:nnn ────────────────────────────── */
        if (!in_img && strncmp(buffer, "SALDO:", 6) == 0) {
            saldo_local = atol(buffer + 6);
            printf("[SALDO ACTUAL] %ld créditos\n", saldo_local);
            continue;
        }
        /* ───────────────── fallo de entrega ───────────────── */
        if (!in_img && strncmp(buffer, "FAIL:", 5) == 0) {
            /* Lo que venga después de "FAIL:" es el motivo */
            printf("Error de entrega: %s\n", buffer + 5);
            continue;
        }
        /* ───────────────── 2) INICIO DE IMAGEN IMG:CELL: ───────────── */
        if (!in_img && strncmp(buffer, "IMG:", 4) == 0) {
            char *p  = buffer + 4;
            char *p2 = strchr(p, ':');
            if (!p2) continue;
            *p2 = '\0';
            char *dest    = p;
            char *fname_h = p2 + 1;

            /* sólo aceptamos si va dirigido a CELL o a ALL */
            if (strcmp(dest, "CELL") && strcmp(dest, "ALL"))
                continue;

            char *dotpng = strstr(fname_h, ".png");
            if (!dotpng) continue;

            size_t base_len = (dotpng - fname_h) + 4;     /* incluye ".png" */
            if (base_len >= MAX_NAME_LEN) continue;

            char fname[MAX_NAME_LEN];
            memcpy(fname, fname_h, base_len);
            fname[base_len] = '\0';

            img_file = fopen(fname, "wb");
            if (!img_file) { perror("fopen imagen"); continue; }

            in_img = 1;
            printf("[INFO] Recibiendo %s …\n", fname);

            /* escribe cualquier HEX pegado al encabezado -------------- */
            char *h = fname_h + base_len;
            while (*h && !isxdigit((unsigned char)*h)) h++; /* salta \r\n */

            for (; isxdigit((unsigned char)h[0]) &&
                   isxdigit((unsigned char)h[1]); h += 2)
            {
                unsigned char byte = hex2byte(h[0], h[1]);
                fwrite(&byte, 1, 1, img_file);
            }
            continue;
        }

        /* ───────────────── 3) PROCESO DE IMAGEN EN CURSO ───────────── */
        if (in_img) {
            /* FIN o ENDIMG ⇒ cerrar archivo */
            if (!strcmp(buffer, "FIN") || !strcmp(buffer, "ENDIMG")) {
                fclose(img_file);
                img_file = NULL;
                in_img   = 0;
                printf("[INFO] Imagen recibida y guardada ✅\n");
            } else {
                /* convierte HEX a binario */
                int len = n;
                for (int i = 0; i + 1 < len; i += 2) {
                    if (!isxdigit((unsigned char)buffer[i]) ||
                        !isxdigit((unsigned char)buffer[i+1]))
                        continue;           /* salta caracteres no-hex */

                    unsigned char byte = hex2byte(buffer[i], buffer[i+1]);
                    fwrite(&byte, 1, 1, img_file);
                }
            }
            continue;
        }

        /* ───────────────── 4) TEXTO NORMAL (acumulado hasta FIN) ───── */
        if (!strcmp(buffer, "FIN")) {          /* FIN -> imprimir bloque */
            if (line_len > 0) {
                line_buf[line_len] = '\0';
                printf("[RECIBIDO]: %s\n", line_buf);
                line_len = 0;                  /* reinicia para próximo msg */
            }
            continue;
        }

        /* añade fragmento al acumulador */
        if (line_len + n >= LINE_MAX)
            n = LINE_MAX - line_len - 1;       /* evita overflow duro */

        memcpy(line_buf + line_len, buffer, n);
        line_len += n;
        /* no se imprime aquí; se hará cuando llegue la línea FIN */
    }
    return NULL;
}

/* --------------------- comandos saldo --------------------------- */
static void cmd_solicitar_saldo(void) {
    send(sockfd, "CMD:SALDO?", 10, 0);
}
static void cmd_recargar(long monto) {
    char cmd[64];
    int m = snprintf(cmd, sizeof(cmd), "CMD:RECARGA:%ld", monto);
    send(sockfd, cmd, m, 0);
}
static void notificar_debito(long bytes) {
    char cmd[64];
    int m = snprintf(cmd, sizeof(cmd), "CMD:DEBIT:%ld", bytes);
    send(sockfd, cmd, m, 0);
}

/* --------------------- envío de imagen -------------------------- */
static void enviar_imagen(const char *destino, const char *nombre) {
    FILE *f = fopen(nombre, "rb");
    if (!f) { perror("fopen"); return; }

    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    fseek(f, 0, SEEK_SET);
    notificar_debito(bytes);

    char hdr[BUFFER_SIZE];
    int hlen = snprintf(hdr, sizeof(hdr), "IMG:%s:%s\n", destino, nombre);
    send(sockfd, hdr, hlen, 0);
    usleep(200000);

    unsigned char raw[IMG_CHUNK_RAW];
    char hex[IMG_CHUNK_RAW*2 +1];
    size_t r;
    while ((r = fread(raw,1,IMG_CHUNK_RAW,f)) > 0) {
        for (size_t i=0; i<r; ++i) sprintf(hex+2*i, "%02X", raw[i]);
        hex[2*r] = '\0';
        send(sockfd, hex, 2*r, 0);
        usleep(80000);
    }
    fclose(f);

    send(sockfd, "ENDIMG\n", 7, 0);
    printf("[INFO] Imagen '%s' enviada a %s (%ld bytes)\n",
           nombre, destino, bytes);
}

/* =================================================================
 *  MAIN
 * =================================================================*/
int main(void) {
    struct sockaddr_in serv = {0};
    pthread_t tid;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return 1; }

    serv.sin_family = AF_INET;
    serv.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &serv.sin_addr);

    if (connect(sockfd,(struct sockaddr*)&serv,sizeof(serv))<0) {
        perror("connect"); return 1;
    }

    pthread_create(&tid, NULL, recibir, NULL);
    cmd_solicitar_saldo();  /* saldo inicial */

    char input[BUFFER_SIZE], dest[16];
    while (running) {
        printf("\n[MENU]\n"
               "1. Enviar texto\n"
               "2. Enviar imagen\n"
               "3. Consultar saldo\n"
               "4. Recargar saldo\n"
               "5. Salir\n"
               "6. Prueba de enlace\n> ");
        if (!fgets(input,sizeof(input),stdin)) break;

        switch (input[0]) {
        /* ------------ 1. TEXTO ---------------------------------- */
        case '1': {
            printf("Destino (A/B/ALL): ");
            if (!fgets(dest, sizeof(dest), stdin)) break;
            dest[strcspn(dest, "\r\n")] = '\0';

            printf("Mensaje: ");
            if (!fgets(input, sizeof(input), stdin)) break;
            input[strcspn(input, "\r\n")] = '\0';

            long len       = strlen(input);
            long projected = saldo_local - len;

            if (saldo_local <= 0) {
                printf("❌ Sin saldo (saldo=%ld)\n", saldo_local);
                break;
            }
            if (projected < 0) {
                if (!overdraft_used) overdraft_used = 1;
                else { puts("❌ Sobregiro ya usado"); break; }
            }

            notificar_debito(len);

            char msg[BUFFER_SIZE];
            int m = snprintf(msg, sizeof(msg), "DEST:%s:%s",
                             dest, input);
            send(sockfd, msg, m, 0);
        } break;

        /* ------------ 2. IMAGEN -------------------------------- */
        case '2': {
            printf("Destino (A/B/ALL): ");
            fgets(dest,sizeof(dest),stdin);
            dest[strcspn(dest,"\r\n")] = 0;
            printf("Archivo PNG: ");
            fgets(input,sizeof(input),stdin);
            input[strcspn(input,"\r\n")] = 0;
            enviar_imagen(dest,input);
        } break;

        /* ------------ 3. SALDO --------------------------------- */
        case '3': cmd_solicitar_saldo(); break;

        /* ------------ 4. RECARGA ------------------------------- */
        case '4':
            printf("Monto a recargar: ");
            fgets(input,sizeof(input),stdin);
            cmd_recargar(atol(input));
            break;

        /* ------------ 5. SALIR -------------------------------- */
        case '5':
            running = 0;
            shutdown(sockfd, SHUT_RDWR);
            break;

        /* ------------ 6. PRUEBA DE ENLACE ---------------------- */
        case '6': {
            int n_pk = 15;
            printf("Se enviarán %d paquetes de prueba.\n", n_pk);
            char cmd[32];
            int m = snprintf(cmd, sizeof(cmd),
                             "CMD:LINKTEST:%d", n_pk);
            send(sockfd, cmd, m, 0);
            puts("[INFO] Prueba de enlace solicitada…");
        } break;

        default: puts("Opción inválida");
        }
    }

    pthread_join(tid, NULL);
    close(sockfd);
    return 0;
}