#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#define PORT          7033
#define MAX_PACKET    1472
#define HEADER_SIZE   32
#define MAX_DATA      (MAX_PACKET - HEADER_SIZE)
#define TIMEOUT_SEC   1
#define MAX_RETRIES   5

/* --- flags --- */
enum {
    F_CONNECT = 1<<4,
    F_REVIVE  = 1<<3,
    F_ACK     = 1<<2,
    F_ACCEPT  = 1<<1,
    F_MORE    = 1<<0
};

/* cabeçalho SLOW (packed) */
#pragma pack(push,1)
typedef struct {
    uint8_t  sid[16];      // Session ID
    uint32_t f_sttl;       // flags (5 bits) | sttl (27 bits)
    uint32_t seqnum;       // número de sequência
    uint32_t acknum;       // número de ack
    uint16_t window;       // janela (não usado)
    uint8_t  fid;          // file ID
    uint8_t  fo;           // fragment offset
} SlowHeader;
#pragma pack(pop)

/* monta f_sttl a partir de flags e sttl */
static uint32_t make_fsttl(uint8_t flags, uint32_t sttl) {
    return (((uint32_t)flags<<27) | (sttl & 0x07FFFFFFu));
}

/* define timeout de recv */
static void set_timeout(int sock) {
    struct timeval tv = { .tv_sec = TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <arquivo_saida>\n", argv[0]);
        return 1;
    }
    const char *out_path = argv[1];

    // 1) cria e faz bind do socket UDP
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_addr   = {.s_addr = INADDR_ANY},
        .sin_port   = htons(PORT)
    };
    if (bind(sock, (struct sockaddr*)&srv, sizeof(srv)) < 0) {
        perror("bind"); close(sock); return 1;
    }
    set_timeout(sock);

    // 2) recebe CONNECT
    SlowHeader h;
    struct sockaddr_in cli;
    socklen_t cli_len = sizeof(cli);
    printf("Aguardando CONNECT na porta %d...\n", PORT);
    while (1) {
        ssize_t r = recvfrom(sock, &h, HEADER_SIZE, 0,
                             (struct sockaddr*)&cli, &cli_len);
        if (r < HEADER_SIZE) continue;
        uint8_t flags = (uint8_t)(h.f_sttl >> 27);
        if (flags & F_CONNECT) {
            char ipstr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cli.sin_addr, ipstr, sizeof(ipstr));
            printf("CONNECT de %s:%d\n", ipstr, ntohs(cli.sin_port));
            break;
        }
    }

    // 3) responde SETUP (ACCEPT)
    uint8_t sid[16];
    srand(time(NULL));
    for (int i = 0; i < 16; i++) sid[i] = rand() & 0xFF;
    uint32_t sttl = 1000;         // ex: 1000 ms
    uint32_t server_seq = 0;
    memset(&h, 0, sizeof(h));
    memcpy(h.sid, sid, 16);
    h.f_sttl  = make_fsttl(F_ACCEPT, sttl);
    h.seqnum  = server_seq;
    h.acknum  = 0;
    h.window  = htons(MAX_PACKET);
    sendto(sock, &h, HEADER_SIZE, 0,
           (struct sockaddr*)&cli, cli_len);
    printf("SETUP enviado (ACCEPT), sttl=%u, seq=%u\n", sttl, server_seq);

    // 4) abre arquivo de saída
    FILE *f = fopen(out_path, "wb");
    if (!f) { perror("fopen"); close(sock); return 1; }

    // 5) recebe fragmentos, grava e ACK
    uint32_t client_seq = 0;
    while (1) {
        uint8_t buf[MAX_PACKET];
        ssize_t r = recvfrom(sock, buf, sizeof(buf), 0,
                             (struct sockaddr*)&cli, &cli_len);
        if (r < HEADER_SIZE) continue;
        memcpy(&h, buf, HEADER_SIZE);
        uint8_t flags = (uint8_t)(h.f_sttl >> 27);

        // desconexão?
        if ((flags & F_CONNECT) && (flags & F_REVIVE) && (flags & F_ACK)) {
            printf("Disconnect recebido.\n");
            // envia ACK final
            SlowHeader out = {0};
            memcpy(out.sid, sid, 16);
            out.f_sttl = make_fsttl(F_ACK, sttl);
            out.seqnum = server_seq;
            out.acknum = h.seqnum;
            sendto(sock, &out, HEADER_SIZE, 0,
                   (struct sockaddr*)&cli, cli_len);
            break;
        }

        // dado normal?
        if ((flags & F_ACK) && (r > HEADER_SIZE)) {
            size_t data_len = r - HEADER_SIZE;
            fwrite(buf + HEADER_SIZE, 1, data_len, f);
            fflush(f);
            // ACK de volta
            SlowHeader out = {0};
            memcpy(out.sid, sid, 16);
            out.f_sttl = make_fsttl(F_ACK, sttl);
            out.seqnum = server_seq++;
            out.acknum = h.seqnum;
            sendto(sock, &out, HEADER_SIZE, 0,
                   (struct sockaddr*)&cli, cli_len);
            printf("Fragmento %u recebido (%zu bytes), enviado ACK %u\n",
                   h.seqnum, data_len, server_seq-1);
        }
    }

    fclose(f);
    close(sock);
    printf("Arquivo recebido em '%s'. Servidor encerrado.\n", out_path);
    return 0;
}
