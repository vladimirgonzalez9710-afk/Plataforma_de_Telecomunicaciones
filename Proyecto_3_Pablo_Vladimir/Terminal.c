// ================================
// terminal_cliente_saldo.c
// ================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>   /* isxdigit() */

#define SERVER_IP   "3.16.78.13"
#define SERVER_PORT 8022

#define BUFFER_SIZE   1024
#define IMG_CHUNK_RAW 32          // 32 bytes crudos -> 64 chars hex
#define HEX_BUF_MAX  65536   // 64 kB de espacio para la cadena HEX
#define IMG_HEADER_PREFIX "IMG:"
#define IMG_END_MARKER   "ENDIMG"
#define TERMINAL_ID      "A"      // Cambia a "B" para la otra terminal

static int sockfd;
static volatile int running = 1;
static long saldo_local = 0;      // reflejo opcional (el real vive en MariaDB)
static int overdraft_used = 0;             // 0=no usado aún, 1=sobregiro ya consumido

// ---------------- Estado recepción imagen -------------------------
static int   receiving_image = 0;
static FILE *img_file        = NULL;
static char  recv_dest[16];
static char  filename_buf[256];
static char  hex_buf[HEX_BUF_MAX];
static int   hex_buf_len     = 0;

static unsigned char hex2byte(char hi, char lo)
{
    unsigned char v = 0;
    if (hi >= '0' && hi <= '9') v = (hi - '0') << 4;
    else if (hi >= 'A' && hi <= 'F') v = (hi - 'A' + 10) << 4;
    else if (hi >= 'a' && hi <= 'f') v = (hi - 'a' + 10) << 4;
    if (lo >= '0' && lo <= '9') v |= (lo - '0');
    else if (lo >= 'A' && lo <= 'F') v |= (lo - 'A' + 10);
    else if (lo >= 'a' && lo <= 'f') v |= (lo - 'a' + 10);
    return v;
}

//-------------------------------------------------------------------
// ENVÍO AUXILIAR SALDO
//-------------------------------------------------------------------
static void notificar_debito(long bytes)
{
    char cmd[64];
    int n = snprintf(cmd, sizeof(cmd), "CMD:DEBIT:%ld", bytes);
    send(sockfd, cmd, n, 0);
    saldo_local -= bytes; // reflejo local
}

static void cmd_recargar(long monto)
{
    char cmd[64];
    int n = snprintf(cmd, sizeof(cmd), "CMD:RECARGA:%ld", monto);
    send(sockfd, cmd, n, 0);
    printf("Recarga exitosa.");
}

static void cmd_solicitar_saldo(void)
{
    const char *q = "CMD:SALDO?";
    send(sockfd, q, strlen(q), 0);
}

//-------------------------------------------------------------------
// RECEPCIÓN
//-------------------------------------------------------------------

//-------------------------------------------------------------------
static void *recibir(void *arg)
{
    unsigned char buffer[BUFFER_SIZE];

    /* Para encabezados de imagen fragmentados */
    static char img_hdr[256];
    static int  img_hdr_len = 0;

    while (running) {
        int len = recv(sockfd, buffer, sizeof(buffer), 0);
        if (len <= 0) { usleep(100000); continue; }

        /* ---------- 1) Mensaje de saldo ------------------------------- */
        if (!strncmp((char*)buffer, "SALDO:", 6)) {
            saldo_local = atol((char*)buffer + 6);
            printf("\n[SERVER] Saldo actualizado: %ld créditos\n", saldo_local);
            continue;
        }
        /* ---------- 1-bis) FALLÓ LA ENTREGA --------------------------- */
        if (!receiving_image && strncmp((char*)buffer, "FAIL:", 5) == 0) {
            printf("❌ %s\n", buffer + 5);   
            continue;
        }
        /* ---------- 2) Procesar datos HEX si ya estamos en imagen ------ */
        if (receiving_image) {
            if (hex_buf_len + len > HEX_BUF_MAX) {
                fprintf(stderr, "[ERROR] Buffer HEX overflow (cap=%d)\n", HEX_BUF_MAX);
                fclose(img_file);
                receiving_image = 0; hex_buf_len = 0;
                continue;
            }
            memcpy(hex_buf + hex_buf_len, buffer, len);
            hex_buf_len += len;

            /* ¿ENDIMG? */
            char *endp = memmem(hex_buf, hex_buf_len,
                                IMG_END_MARKER, strlen(IMG_END_MARKER));

            /* ----- convertir pares HEX válidos a binario -------------- */
            int dataLen = endp ? (int)(endp - hex_buf) : hex_buf_len;

            /* filtra cualquier char no-hex (\r, \n, etc.) --------------- */
            int wr = 0;
            for (int rd = 0; rd < dataLen; ++rd) {
                if (isxdigit((unsigned char)hex_buf[rd]))
                    hex_buf[wr++] = hex_buf[rd];
            }
            dataLen = wr;

            int pairs = dataLen / 2;
            for (int i = 0; i < pairs; ++i) {
                unsigned char byte = hex2byte(hex_buf[2*i], hex_buf[2*i+1]);
                fwrite(&byte, 1, 1, img_file);
            }

            /* desplaza sobrante (pares incompletos) -------------------- */
            int leftover = dataLen - pairs * 2;
            memmove(hex_buf, hex_buf + pairs * 2, leftover);
            hex_buf_len = leftover;

            if (endp) {
                fclose(img_file);
                receiving_image = 0; hex_buf_len = 0;
                printf("[INFO] Imagen %s recibida y guardada ✅\n", filename_buf);
            }
            continue;
        }

        /* ---------- 3) ¿Estamos construyendo el encabezado IMG:? ------- */
        if (img_hdr_len || (len >= 4 && !memcmp(buffer, IMG_HEADER_PREFIX, 4))) {

            /* Acumular fragmentos */
            int space = sizeof(img_hdr) - img_hdr_len - 1;
            int cp    = len < space ? len : space;
            memcpy(img_hdr + img_hdr_len, buffer, cp);
            img_hdr_len += cp;
            img_hdr[img_hdr_len] = '\0';

            /* ¿Encabezado completo? (IMG:DEST:nombre.png) */
            char *p_first = strchr(img_hdr + strlen(IMG_HEADER_PREFIX), ':');
            char *p_ext   = strstr(img_hdr, ".png");
            if (p_first && p_ext && p_ext > p_first) {

                /* — filename — */
                int fnameLen = (p_ext - p_first) + 4;           /* incluye ".png" */
                if (fnameLen >= (int)sizeof(filename_buf))
                    fnameLen = sizeof(filename_buf) - 1;
                memcpy(filename_buf, p_first + 1, fnameLen);
                filename_buf[fnameLen] = '\0';

                /* Abrir archivo de salida */
                img_file = fopen(filename_buf, "wb");
                if (!img_file) { perror("fopen"); img_hdr_len = 0; continue; }

                receiving_image = 1;
                hex_buf_len     = 0;
                printf("[INFO] Recibiendo %s...\n", filename_buf);

                /* Copiar cualquier HEX pegado al header (salta \r\n) ---- */
                int hdrSize = (p_ext - img_hdr) + 4;            /* hasta ".png" */
                while (hdrSize < img_hdr_len &&
                       (img_hdr[hdrSize] == '\n' || img_hdr[hdrSize] == '\r'))
                    hdrSize++;

                int rem = img_hdr_len - hdrSize;
                if (rem > 0) {
                    memcpy(hex_buf, img_hdr + hdrSize, rem);
                    hex_buf_len = rem;
                }
                img_hdr_len = 0;      /* reiniciar acumulador */
                continue;
            }
            /* Si aún incompleto, esperar otro recv() */
            continue;
        }

        /* ---------- 4) Texto normal (filtramos FIN y vacíos) ----------- */
        if ((len == 3 && !memcmp(buffer, "FIN", 3)) || len == 1) continue;

        buffer[len] = '\0';
        printf("[RECIBIDO]: %s\n", buffer);
    }
    return NULL;
}

//-------------------------------------------------------------------
// ENVÍO: imagen en HEX + DEBITO
//-------------------------------------------------------------------
static void enviar_imagen(const char *destino, const char *nombre)
{
    FILE *f = fopen(nombre, "rb");
    if (!f) { perror("fopen"); return; }

    /* Calcula tamaño real para debitar */
    struct stat st; long bytes = 0;
    if (!stat(nombre, &st)) bytes = st.st_size;

    char hdr[BUFFER_SIZE];
    int hlen = snprintf(hdr, sizeof(hdr), "IMG:%s:%s", destino, nombre);

    notificar_debito(bytes);          // avisa antes de enviar
    send(sockfd, hdr, hlen, 0);
    usleep(200000);

    unsigned char raw[IMG_CHUNK_RAW];
    char hexline[IMG_CHUNK_RAW * 2];
    size_t n;
    while ((n = fread(raw, 1, IMG_CHUNK_RAW, f)) > 0) {
        for (size_t i = 0; i < n; ++i) sprintf(hexline + 2*i, "%02X", raw[i]);
        send(sockfd, hexline, 2*n, 0);
        usleep(100000);
    }
    send(sockfd, IMG_END_MARKER, strlen(IMG_END_MARKER), 0);
    fclose(f);

    printf("[INFO] Imagen '%s' enviada a %s. (%ld bytes)\n", nombre, destino, bytes);
}

//-------------------------------------------------------------------
int main(void)
{
    struct sockaddr_in serv = {0};
    pthread_t tid;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    serv.sin_family = AF_INET;
    serv.sin_port   = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &serv.sin_addr);
    if (connect(sockfd, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
        perror("connect"); return 1; }

    pthread_create(&tid, NULL, recibir, NULL);

    // Consulta saldo inicial
    cmd_solicitar_saldo();

    while (running) {
        char input[BUFFER_SIZE], dest[16];
        printf("\n[MENU Terminal %s | Saldo local=%ld]\n"
               "1. Enviar texto\n2. Enviar imagen\n3. Recargar saldo\n"
               "4. Verificar saldo\n5. Salir\n> ", TERMINAL_ID, saldo_local);
        if (!fgets(input, sizeof(input), stdin)) break;

        switch (input[0]) {
        case '1': { // enviar texto
            printf("Destino (1=A 2=B 3=CELL): ");
            fgets(input, sizeof(input), stdin);
            if      (input[0] == '1') strcpy(dest, "A");
            else if (input[0] == '2') strcpy(dest, "B");
            else                       strcpy(dest, "CELL");

            printf("Mensaje: ");
            fgets(input, sizeof(input), stdin);
            // ∎ eliminar CR/LF para que strlen sea exacto
            input[strcspn(input, "\r\n")] = '\0';

            long len = strlen(input);
            long projected = saldo_local - len;

            // ∎ bloqueo absoluto si ya estoy a cero
            if (saldo_local <= 0) {
                printf("❌ No tienes saldo para enviar mensajes (saldo=%ld)\n",
                       saldo_local);
                break;
            }

            // ∎ si me pasaría a negativo…
            if (projected < 0) {
                // …solo lo permito si aún no usé sobregiro y tenía saldo positivo
                if (!overdraft_used) {
                    overdraft_used = 1;
                } else {
                    printf("❌ Saldo insuficiente y sobregiro ya consumido\n");
                    break;
                }
            }

            // ∎ aquí restamos len créditos
            notificar_debito(len);

            // ∎ mensajito al servidor
            char mensaje[BUFFER_SIZE];
            int m = snprintf(mensaje, sizeof(mensaje),
                             "DEST:%s:[%s]:%s",
                             dest, TERMINAL_ID, input);
            send(sockfd, mensaje, m, 0);
        } break;

        case '2': { // enviar imagen
            printf("Destino (1=A 2=B 3=CELL): "); fgets(input, sizeof(input), stdin);
            if      (input[0] == '1') strcpy(dest, "A");
            else if (input[0] == '2') strcpy(dest, "B");
            else                       strcpy(dest, "CELL");

            printf("Usa: image <nombre.png>\n> "); fgets(input, sizeof(input), stdin);
            if (strncmp(input, "image ", 6) == 0) {
                char *name = strtok(input + 6, "\r\n");
                enviar_imagen(dest, name);
            }
        } break;

        case '3': { // recargar saldo
            printf("Monto a recargar: "); fgets(input, sizeof(input), stdin);
            long monto = atol(input);
            cmd_recargar(monto);
        } break;

        case '4':   // verificar saldo
            cmd_solicitar_saldo();
            break;

        case '5':   // salir
            running = 0;
            shutdown(sockfd, SHUT_RDWR);
            break;
        default:
            puts("Opción inválida");
        }
    }

    pthread_join(tid, NULL);
    close(sockfd);
    return 0;
}