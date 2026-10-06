/* ------------------------------------------------------------------
 *  Servidor_AWS_saldo.c  –  Puente AWS (TCP)
------------------------------------------------------------------*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <errno.h>
#include <mysql/mysql.h>

#define PORT_PC_GATEWAY 8080
#define PORT_TERMINALES 8022
#define BUFFER_SIZE     4096
#define MAX_CLIENTS        2

#define CMD_ENLACE      "CMD:ENLACE\n"
#define LEN_ENLACE      (sizeof(CMD_ENLACE) - 1)

// Tamaño de fragmento para enviar imágenes (bytes HEX)
#define IMG_CHUNK_TX 128
#define BAR_W 50  // ancho de la barra de enlace

static int client_A   = -1;
static int client_B   = -1;
static int pc_gateway = -1;

pthread_mutex_t pc_mutex   = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t tx_mutex_A = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t tx_mutex_B = PTHREAD_MUTEX_INITIALIZER;
// Para coordinar quién pidió la prueba de enlace
static pthread_mutex_t last_req_mutex    = PTHREAD_MUTEX_INITIALIZER;
static int               last_enlace_req_fd = -1;

// Funciones auxiliares para base de datos
static MYSQL *db_connect(void)
{
    MYSQL *conn = mysql_init(NULL);
    if (!conn) return NULL;
    if (!mysql_real_connect(conn,
            "localhost", "chatapp", "TuPassFuerte!", "chat_saldo",
            0, NULL, 0)) {
        fprintf(stderr, "[DB] connect error: %s\n", mysql_error(conn));
        return NULL;
    }
    return conn;
}

static void db_debit(MYSQL *conn,const char *alias, int bytes)
{
    char q[128];
    snprintf(q,sizeof(q),
             "UPDATE saldo_usuarios SET saldo = saldo - %d WHERE usuario='%s'",
             bytes, alias);
    mysql_query(conn,q);
}

static void db_recharge(MYSQL *conn,const char *alias, int monto)
{
    char q[128];
    snprintf(q,sizeof(q),
             "UPDATE saldo_usuarios SET saldo = saldo + %d WHERE usuario='%s'",
             monto, alias);
    mysql_query(conn,q);
}

static int db_get_saldo(MYSQL *conn,const char *alias)
{
    char q[128];
    snprintf(q,sizeof(q),
             "SELECT saldo FROM saldo_usuarios WHERE usuario='%s'", alias);
    if (mysql_query(conn,q)) return 0;
    MYSQL_RES *res = mysql_store_result(conn);
    if (!res) return 0;
    MYSQL_ROW row = mysql_fetch_row(res);
    int saldo = row? atoi(row[0]):0;
    mysql_free_result(res);
    return saldo;
}

// Función que asegura enviar todo el buffer
static void send_all(int fd, const char *buf, size_t len)
{
    size_t off=0;
    while(off<len){
        ssize_t s=send(fd,buf+off,len-off,0);
        if(s<=0) break;
        off+=s;
    }
}

// Dibuja en terminal una barra de éxito/fracaso
static void show_link_bar(int sent, int ok, int lost) {
    int pct_ok = sent ? (ok * 100) / sent : 0;
    int fill   = (pct_ok * BAR_W) / 100;

    printf("\n[RESULTADO PRUEBA ENLACE SERVIDOR]\n[");
    for (int i = 0; i < BAR_W; ++i)
        putchar(i < fill ? '#' : '-');
    printf("] %d%% éxito  (Ok=%d  Perdidos=%d  Total=%d)\n\n",
           pct_ok, ok, lost, sent);
}


// Handler para terminales (A o B)

void* terminal_handler(void *arg)
{
    int fd = *(int*)arg;
    free(arg);

    char alias = (fd == client_A) ? 'A' : 'B';
    char alias_str[2] = { alias, '\0' };
    printf("[AWS] Handler terminal %c (fd=%d)\n", alias, fd);

    MYSQL *conn = db_connect();
    if (!conn) {
        fprintf(stderr, "[AWS] sin DB, se sigue pero no habrá saldo\n");
    }

    char buf[BUFFER_SIZE];
    int n;

    int in_img = 0;
    int img_dest_fd = -1;
    pthread_mutex_t *img_mutex = NULL;

    while ((n = recv(fd, buf, sizeof(buf)-1, 0)) > 0) {
        buf[n] = '\0';

        // ─── 1) Comandos saldo CMD: ───────────────────────────────
        if (n >= 4 && !strncmp(buf, "CMD:", 4)) {
            if (!strncmp(buf + 4, "DEBIT:", 6)) {
                int bytes = atoi(buf + 10);
                if (conn) db_debit(conn, alias_str, bytes);
            }
            else if (!strncmp(buf + 4, "RECARGA:", 8)) {
                int monto = atoi(buf + 12);
                if (conn) db_recharge(conn, alias_str, monto);
            }
            else if (!strncmp(buf + 4, "SALDO?", 6)) {
                int saldo = conn ? db_get_saldo(conn, alias_str) : 0;
                char resp[64];
                int len = snprintf(resp, sizeof(resp), "SALDO:%d\n", saldo);
                send_all(fd, resp, len);
            }
            continue;  // no propagar otros destinos
        }

        // ─── 2) Enlace de prueba CMD:ENLACE ─────────────────────────
        if (n == LEN_ENLACE && strncmp(buf, CMD_ENLACE, LEN_ENLACE) == 0) {
            printf("[AWS] Recibí ENLACE de Terminal %c\n", alias);

            // guardo quién lanzó la prueba
            pthread_mutex_lock(&last_req_mutex);
            last_enlace_req_fd = fd;
            pthread_mutex_unlock(&last_req_mutex);

            // reenvío CMD:ENLACE a la PC-Gateway
            if (pc_gateway > 0) {
                pthread_mutex_lock(&pc_mutex);
                send_all(pc_gateway, buf, n);
                pthread_mutex_unlock(&pc_mutex);
                printf("[AWS] Reenvié ENLACE al PC-Gateway\n");
            } else {
                fprintf(stderr, "[AWS] ERROR: PC-Gateway no conectado, no puedo reenviar ENLACE\n");
            }
            continue;
        }

        // ─── 3) Inicio de imagen IMG: ───────────────────────────────
        if (!in_img && n >= 4 && !memcmp(buf, "IMG:", 4)) {
            if (buf[4] == 'A') {
                img_dest_fd = client_A; img_mutex = &tx_mutex_A;
            } else if (buf[4] == 'B') {
                img_dest_fd = client_B; img_mutex = &tx_mutex_B;
            } else if (!memcmp(buf + 4, "CELL", 4)) {
                img_dest_fd = pc_gateway; img_mutex = &pc_mutex;
            } else {
                img_dest_fd = -1; img_mutex = NULL;
            }
            if (img_dest_fd > 0 && img_mutex) {
                pthread_mutex_lock(img_mutex);
                send_all(img_dest_fd, buf, n);
                pthread_mutex_unlock(img_mutex);
            }
            in_img = 1;
            continue;
        }

        // ─── 4) Fragmentos en imagen ────────────────────────────────
        if (in_img) {
            if (img_dest_fd > 0 && img_mutex) {
                pthread_mutex_lock(img_mutex);
                for (int off = 0; off < n; off += IMG_CHUNK_TX) {
                    int chunk = (n - off > IMG_CHUNK_TX) ? IMG_CHUNK_TX : n - off;
                    send_all(img_dest_fd, buf + off, chunk);
                }
                pthread_mutex_unlock(img_mutex);
            }
            if (memmem(buf, n, "ENDIMG", 6)) {
                in_img = 0;
                img_dest_fd = -1;
                img_mutex = NULL;
            }
            continue;
        }

        // ─── 5) Reenvío texto normal por prefijos ────────────────────
        if (n > 7 && !strncmp(buf, "DEST:A:", 7) && client_A > 0) {
            send_all(client_A, buf + 7, n - 7);
        }
        else if (n > 7 && !strncmp(buf, "DEST:B:", 7) && client_B > 0) {
            send_all(client_B, buf + 7, n - 7);
        }
        else if (n > 9 && !strncmp(buf, "DEST:ALL:", 9)) {
            if (client_A > 0) send_all(client_A, buf + 9, n - 9);
            if (client_B > 0) send_all(client_B, buf + 9, n - 9);
            pthread_mutex_lock(&pc_mutex);
            if (pc_gateway > 0) send_all(pc_gateway, buf + 9, n - 9);
            pthread_mutex_unlock(&pc_mutex);
        }
        else if (n > 10 && !strncmp(buf, "DEST:CELL:", 10)) {
            pthread_mutex_lock(&pc_mutex);
            if (pc_gateway > 0) send_all(pc_gateway, buf + 10, n - 10);
            pthread_mutex_unlock(&pc_mutex);
        }
    }

    printf("[AWS] Terminal %c desconectada\n", alias);
    close(fd);
    if (fd == client_A)      client_A = -1;
    else if (fd == client_B) client_B = -1;
    if (conn) mysql_close(conn);
    return NULL;
}
/* =================================================================
 *  PC GATEWAY HANDLER  (socket fd ⇄ celular / Raspberry / PC-gateway)
 * =================================================================*/
void* pc_gateway_handler(void *arg)
{
    int fd = *(int*)arg;
    free(arg);
    printf("[AWS] PC Gateway conectada (fd=%d)\n", fd);

    MYSQL *conn = db_connect();
    if (!conn)
        fprintf(stderr, "[AWS] ⚠️  NO se pudo conectar a MariaDB – comandos CMD ignorados\n");

    const char *alias_cell = "CELL";

    char buf[BUFFER_SIZE];
    int  n;

    /* ← NUEVO: estado para imágenes */
    int in_img = 0;
    int img_dest_fd = -1;
    pthread_mutex_t *img_mutex = NULL;

    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        buf[n] = '\0';

        /* 0) LINKTEST -------------------------------------------------- */
        if (!strncmp(buf, "LINKTEST RESULT", 15)) {
            int sent, ok, lost;
            sscanf(buf, "LINKTEST RESULT sent=%d ok=%d lost=%d",
                   &sent, &ok, &lost);
            show_link_bar(sent, ok, lost);
            continue;
        }

        /* 1) COMANDOS CMD: -------------------------------------------- */
        if (n >= 4 && !strncmp(buf, "CMD:", 4)) {
            if (!strncmp(buf + 4, "DEBIT:", 6) && conn)
                db_debit(conn, alias_cell, atoi(buf + 10));
            else if (!strncmp(buf + 4, "RECARGA:", 8) && conn)
                db_recharge(conn, alias_cell, atoi(buf + 12));
            else if (!strncmp(buf + 4, "SALDO?", 6) && conn) {
                int saldo = db_get_saldo(conn, alias_cell);
                char resp[64];
                int len = snprintf(resp, sizeof(resp), "SALDO:%d\n", saldo);
                send_all(fd, resp, (size_t)len);
            }
            continue;
        }

        /* 2) IMG: encabezado ------------------------------------------- */
        /*    Formato: IMG:A:filename.png\n …fragmentos… ENDIMG\n        */
        if (!in_img && n >= 4 && !memcmp(buf, "IMG:", 4)) {           /* ← NUEVO */
            if (buf[4] == 'A')      { img_dest_fd = client_A; img_mutex = &tx_mutex_A; }
            else if (buf[4] == 'B') { img_dest_fd = client_B; img_mutex = &tx_mutex_B; }
            else { img_dest_fd = -1; img_mutex = NULL; }

            if (img_dest_fd > 0 && img_mutex) {
                pthread_mutex_lock(img_mutex);
                send_all(img_dest_fd, buf, n);                       /* reenvía encabezado */
                pthread_mutex_unlock(img_mutex);
            }
            in_img = 1;
            continue;
        }

        /* 3) IMG: fragmentos ------------------------------------------- */
        if (in_img) {                                                /* ← NUEVO */
            if (img_dest_fd > 0 && img_mutex) {
                pthread_mutex_lock(img_mutex);
                for (int off = 0; off < n; off += IMG_CHUNK_TX) {
                    int chunk = (n - off > IMG_CHUNK_TX) ? IMG_CHUNK_TX : n - off;
                    send_all(img_dest_fd, buf + off, chunk);
                }
                pthread_mutex_unlock(img_mutex);
            }
            if (memmem(buf, n, "ENDIMG", 6)) {   /* fin de imagen */
                in_img = 0; img_dest_fd = -1; img_mutex = NULL;
            }
            continue;
        }

        /* 4) RUTEO DE TEXTO (DEST:…) ----------------------------------- */
        if (n > 7 && !strncmp(buf, "DEST:A:", 7) && client_A > 0) {
            send_all(client_A, buf + 7, n - 7);
        }
        else if (n > 7 && !strncmp(buf, "DEST:B:", 7) && client_B > 0) {
            send_all(client_B, buf + 7, n - 7);
        }
        else if (n > 9 && !strncmp(buf, "DEST:ALL:", 9)) {
            if (client_A > 0) send_all(client_A, buf + 9, n - 9);
            if (client_B > 0) send_all(client_B, buf + 9, n - 9);
        }

        /* 5) Nada más: descarta                                         */
        /*    (ya no hay broadcast por defecto) */                      /* ← CAMBIO */
    }

    /* ---- cleanup ---------------------------------------------------- */
    printf("[AWS] PC Gateway desconectada (fd=%d)\n", fd);
    close(fd);
    pthread_mutex_lock(&pc_mutex);  pc_gateway = -1;  pthread_mutex_unlock(&pc_mutex);
    if (conn) mysql_close(conn);
    return NULL;
}


// MAIN
int main(void)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    mysql_library_init(0,NULL,NULL);

    int opt=1;
    int sock_term = socket(AF_INET,SOCK_STREAM,0);
    setsockopt(sock_term,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in a_term={.sin_family=AF_INET,.sin_addr.s_addr=INADDR_ANY,
                               .sin_port=htons(PORT_TERMINALES)};
    bind(sock_term,(void*)&a_term,sizeof(a_term));
    listen(sock_term,MAX_CLIENTS);

    int sock_pc = socket(AF_INET,SOCK_STREAM,0);
    setsockopt(sock_pc,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in a_pc={.sin_family=AF_INET,.sin_addr.s_addr=INADDR_ANY,
                             .sin_port=htons(PORT_PC_GATEWAY)};
    bind(sock_pc,(void*)&a_pc,sizeof(a_pc));
    listen(sock_pc,1);

    printf("[AWS] Esperando PC Gateway (puerto %d)...\n", PORT_PC_GATEWAY);
    int pc_fd = accept(sock_pc,NULL,NULL);
    pc_gateway = pc_fd;
    pthread_t tpc; int *p1=malloc(sizeof(int)); *p1=pc_fd;
    pthread_create(&tpc,NULL,pc_gateway_handler,p1);
    pthread_detach(tpc);

    printf("[AWS] Esperando terminales (puerto %d)...\n", PORT_TERMINALES);
    while(1){
        int fd=accept(sock_term,NULL,NULL);
        if(fd<0) continue;
        if(client_A<0){ client_A=fd; puts("[AWS] Conectó Terminal A"); }
        else if(client_B<0){ client_B=fd; puts("[AWS] Conectó Terminal B"); }
        else { puts("[AWS] Rechazada conexión extra"); close(fd); continue; }
        pthread_t th; int *p=malloc(sizeof(int)); *p=fd;
        pthread_create(&th,NULL,terminal_handler,p);
        pthread_detach(th);
    }
    mysql_library_end();
    return 0;
}
