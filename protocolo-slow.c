/**
 * Nome e número USP dos membros

* Fernando Alee Suaiden - 12680836
* Felipe da Costa Coqueiro - 11781361
* Flávio Masaaki Ito - 12609046


 * @file protocolo-slow.c
 * @brief Implementação de um cliente (peripheral) para o protocolo de transporte SLOW.
 *
 * Este programa estabelece uma conexão com um servidor (central) SLOW,
 * transfere um arquivo de forma confiável usando fragmentação e retransmissão,
 * e encerra a conexão.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <endian.h>
#include <netdb.h>

// --- Constantes do Protocolo ---
#define PORT 7033
#define MAX_PACKET 1472
#define HEADER_SIZE 32
#define MAX_DATA (MAX_PACKET - HEADER_SIZE)
#define TIMEOUT_SEC 1
#define MAX_RETRIES 5

// --- Flags do Cabeçalho SLOW ---
enum {
    F_CONNECT = 1 << 4,
    F_REVIVE  = 1 << 3,
    F_ACK     = 1 << 2,
    F_ACCEPT  = 1 << 1,
    F_MORE    = 1 << 0
};

// --- Estrutura do Cabeçalho SLOW ---
#pragma pack(push, 1)
typedef struct {
    uint8_t  sid[16];      // Session ID
    uint32_t f_sttl;       // Flags (5 bits) | Session TTL (27 bits)
    uint32_t seqnum;       // Sequence Number
    uint32_t acknum;       // Acknowledgement Number
    uint16_t window;       // Window Size
    uint8_t  fid;          // Fragment ID
    uint8_t  fo;           // Fragment Offset
} SlowHeader;
#pragma pack(pop)

/**
 * @brief Configura um timeout de recebimento no socket.
 * @param sock O descritor do socket.
 */
static void set_timeout(int sock) {
    struct timeval tv = { .tv_sec = TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/**
 * @brief Envia um pacote SLOW (cabeçalho + dados opcionais).
 * @param sock O descritor do socket.
 * @param dst A estrutura de endereço do destino.
 * @param h O cabeçalho SLOW a ser enviado.
 * @param data Ponteiro para os dados (payload).
 * @param len Comprimento dos dados.
 * @return 0 em caso de sucesso, -1 em caso de erro.
 */
static int send_pkt(int sock, struct sockaddr_in *dst, SlowHeader *h,
                    const uint8_t *data, size_t len)
{
    uint8_t buf[MAX_PACKET];
    memcpy(buf, h, HEADER_SIZE);
    if (data && len > 0) {
        memcpy(buf + HEADER_SIZE, data, len);
    }
    ssize_t sent_bytes = sendto(sock, buf, HEADER_SIZE + len, 0,
                                (struct sockaddr*)dst, sizeof(*dst));
    return sent_bytes < 0 ? -1 : 0;
}

/**
 * @brief Recebe um cabeçalho SLOW, ignorando o payload.
 * @param sock O descritor do socket.
 * @param h Ponteiro para a estrutura onde o cabeçalho será armazenado.
 * @return 0 em caso de sucesso, -1 em caso de erro (e.g., timeout).
 */
static int recv_hdr(int sock, SlowHeader *h) {
    uint8_t buf[MAX_PACKET];
    ssize_t r = recv(sock, buf, MAX_PACKET, 0);
    if (r < HEADER_SIZE) return -1;
    memcpy(h, buf, HEADER_SIZE);
    return 0;
}

/**
 * @brief Monta o campo de 32 bits que combina flags e sttl.
 * @param flags Os 5 bits de flags.
 * @param sttl Os 27 bits de Session TTL.
 * @return O valor de 32 bits combinado.
 */
static uint32_t make_fsttl(uint8_t flags, uint32_t sttl) {
    return (flags & 0x1Fu) | (sttl << 5);
}

/**
 * @brief Realiza o 3-way handshake para iniciar uma sessão SLOW.
 * @param sock O descritor do socket.
 * @param dst Endereço do central.
 * @param sid Buffer para armazenar o Session ID recebido.
 * @param sttl Ponteiro para armazenar o Session TTL recebido.
 * @param peer_seq Ponteiro para armazenar o número de sequência inicial do central.
 * @return 0 em sucesso, -1 em erro.
 */
static int handshake(int sock, struct sockaddr_in *dst,
                     uint8_t sid[16], uint32_t *sttl, uint32_t *peer_seq)
{
    SlowHeader h = {0};
    h.f_sttl = htole32(make_fsttl(F_CONNECT, 0));
    h.window = htole16(MAX_PACKET);

    if (send_pkt(sock, dst, &h, NULL, 0) < 0) { perror("send CONNECT"); return -1; }

    if (recv_hdr(sock, &h) < 0) { perror("recv SETUP"); return -1; }

    uint32_t f_sttl_host = le32toh(h.f_sttl);
    uint8_t received_flags = f_sttl_host & 0x1Fu;

    if (!(received_flags & F_ACCEPT)) {
        fprintf(stderr, "Conexão rejeitada pelo central.\n"); return -1;
    }

    memcpy(sid, h.sid, 16);
    *sttl = f_sttl_host >> 5;
    *peer_seq = le32toh(h.seqnum);
    printf("Handshake ok: sttl=%u, peer_seq=%u\n", *sttl, *peer_seq);
    return 0;
}

/**
 * @brief Envia um fragmento de dados e aguarda confirmação (ACK), com retentativas.
 * @param sock O descritor do socket.
 * @param dst Endereço do central.
 * @param sid O Session ID da sessão.
 * @param sttl O Session TTL da sessão.
 * @param my_seq Ponteiro para o número de sequência do peripheral.
 * @param acknum O número de sequência do último pacote recebido do central.
 * @param data Ponteiro para o fragmento de dados a ser enviado.
 * @param len Tamanho do fragmento de dados.
 * @param fid O Fragment ID.
 * @param fo O Fragment Offset (índice do fragmento atual).
 * @param is_last_fragment Flag que indica se este é o último fragmento.
 * @return 0 em sucesso, -1 em erro.
 */
static int send_fragment(int sock, struct sockaddr_in *dst, uint8_t sid[16],
                         uint32_t sttl, uint32_t *my_seq, uint32_t acknum,
                         const uint8_t *data, size_t len, uint8_t fid, uint8_t fo,
                         int is_last_fragment)
{
    SlowHeader h;
    int tries = 0;
    while (tries++ < MAX_RETRIES) {
        memset(&h, 0, sizeof(h));
        memcpy(h.sid, sid, 16);
        
        uint8_t flags = F_ACK;
        if (!is_last_fragment) {
            flags |= F_MORE;
        }

        h.f_sttl = htole32(make_fsttl(flags, sttl));
        h.seqnum = htole32(*my_seq);
        h.acknum = htole32(acknum);
        h.window = htole16(MAX_PACKET * 10);
        h.fid = fid;
        h.fo = fo;

        if (send_pkt(sock, dst, &h, data, len) < 0) {
            perror("send DATA");
            return -1;
        }
        
        if (recv_hdr(sock, &h) == 0 &&
            ((le32toh(h.f_sttl) & 0x1Fu) & F_ACK) &&
            (le32toh(h.acknum) == *my_seq))
        {
            (*my_seq)++;
            return 0;
        }
        fprintf(stderr, "Retry do fragmento com seqnum %u\n", *my_seq);
    }
    fprintf(stderr, "Falha ao enviar fragmento com seqnum %u após %d tentativas.\n", *my_seq, MAX_RETRIES);
    return -1;
}

/**
 * @brief Lê um arquivo e o envia em múltiplos fragmentos SLOW.
 * @param sock O descritor do socket.
 * @param dst Endereço do central.
 * @param sid O Session ID da sessão.
 * @param sttl O Session TTL da sessão.
 * @param peer_seq O último número de sequência recebido do central.
 * @param path O caminho do arquivo a ser enviado.
 * @return 0 em sucesso, -1 em erro.
 */
static int send_file(int sock, struct sockaddr_in *dst, uint8_t sid[16],
                     uint32_t sttl, uint32_t peer_seq, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return -1; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *buf = malloc(sz);
    if (!buf) { perror("malloc"); fclose(f); return -1; }
    fread(buf, 1, sz, f);
    fclose(f);

    uint32_t nfrag = (sz == 0) ? 1 : (sz + MAX_DATA - 1) / MAX_DATA;
    uint8_t fid = (nfrag > 1) ? 1 : 0;
    uint32_t offset = 0, my_seq = 1; // Inicia seq de dados em 1

    for (uint32_t i = 0; i < nfrag; i++) {
        size_t chunk = (sz - offset > MAX_DATA) ? MAX_DATA : sz - offset;
        int is_last = (i == nfrag - 1);

        if (send_fragment(sock, dst, sid, sttl, &my_seq, peer_seq,
                          buf + offset, chunk, fid, i, is_last) < 0)
        {
            free(buf);
            return -1;
        }
        offset += chunk;
    }

    free(buf);
    printf("Enviado %u fragments.\n", nfrag);
    return 0;
}

/**
 * @brief Envia uma mensagem de Disconnect para encerrar a sessão.
 * @param sock O descritor do socket.
 * @param dst Endereço do central.
 * @param sid O Session ID da sessão.
 * @param sttl O Session TTL da sessão.
 * @param peer_seq O último número de sequência recebido do central.
 * @return 0 em sucesso, -1 em erro.
 */
static int disconnect_slow(int sock, struct sockaddr_in *dst, uint8_t sid[16],
                           uint32_t sttl, uint32_t peer_seq)
{
    SlowHeader h = {0};
    memcpy(h.sid, sid, 16);
    
    uint8_t flags = F_CONNECT | F_REVIVE | F_ACK;
    h.f_sttl = htole32(make_fsttl(flags, sttl));
    // seqnum e acknum em um ack de disconnect devem ser o mesmo
    h.seqnum = htole32(peer_seq);
    h.acknum = htole32(peer_seq);

    return send_pkt(sock, dst, &h, NULL, 0);
}

/**
 * @brief Ponto de entrada principal do programa cliente SLOW.
 */
int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Uso: %s <hostname_central> <arquivo>\n", argv[0]);
        return 1;
    }

    struct sockaddr_in dst;
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int status = getaddrinfo(argv[1], "7033", &hints, &res);
    if (status != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(status));
        return 1;
    }
    memcpy(&dst, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }
    set_timeout(sock);

    uint8_t sid[16];
    uint32_t sttl, peer_seq;
    printf("Realizando handshake com %s...\n", argv[1]);
    if (handshake(sock, &dst, sid, &sttl, &peer_seq) < 0) {
        fprintf(stderr, "Falha no handshake.\n");
        close(sock);
        return 1;
    }

    printf("Enviando arquivo '%s'...\n", argv[2]);
    if (send_file(sock, &dst, sid, sttl, peer_seq, argv[2]) < 0) {
        fprintf(stderr, "Falha ao enviar o arquivo.\n");
        close(sock);
        return 1;
    }

    printf("Desconectando.\n");
    disconnect_slow(sock, &dst, sid, sttl, peer_seq);
    close(sock);

    printf("Operação concluída com sucesso.\n");
    return 0;
}