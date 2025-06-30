/**
 * Nome e número USP dos membros
 * Fernando Alee Suaiden - 12680836
 * Felipe da Costa Coqueiro - 11781361
 * Flávio Masaaki Ito - 12609046
 * * @file protocolo-slow-corrigido.c
 * @brief Implementação de um cliente (peripheral) para o protocolo de transporte SLOW.
 *
 * VERSÃO CORRIGIDA: Implementa controle de fluxo com Janela Deslizante.
 *
 * Este programa estabelece uma conexão com um servidor (central) SLOW,
 * transfere um arquivo de forma confiável usando fragmentação, retransmissão,
 * e controle de fluxo por janela deslizante, e encerra a conexão.
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
#include <stdbool.h> // Para usar bool, true, false
#include <time.h>      // Para time()

// --- Constantes do Protocolo ---
#define PORT 7033
#define MAX_PACKET 1472
#define HEADER_SIZE 32
#define MAX_DATA (MAX_PACKET - HEADER_SIZE)
#define TIMEOUT_SEC 1 // Timeout para retransmissão
#define MAX_RETRIES 5 // Mantido para o handshake, mas a lógica de retransmissão muda no envio de dados

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
    uint8_t  sid[16];
    uint32_t f_sttl;
    uint32_t seqnum;
    uint32_t acknum;
    uint16_t window;
    uint8_t  fid;
    uint8_t  fo;
} SlowHeader;
#pragma pack(pop)

// --- Estrutura para rastrear pacotes em trânsito (in-flight) ---
typedef struct {
    uint8_t* data;
    size_t    len;
    uint32_t  seqnum;
    uint8_t   fid;
    uint8_t   fo;
    bool      is_last;
    time_t    sent_time;
    bool      ack_received;
} InFlightPacket;


static int send_pkt(int sock, struct sockaddr_in *dst, SlowHeader *h,
                    const uint8_t *data, size_t len)
{
    uint8_t buf[MAX_PACKET];
    memcpy(buf, h, HEADER_SIZE);
    if (data && len > 0) {
        memcpy(buf + HEADER_SIZE, data, len);
    }
    return sendto(sock, buf, HEADER_SIZE + len, 0, (struct sockaddr*)dst, sizeof(*dst)) < 0 ? -1 : 0;
}

static int recv_hdr(int sock, SlowHeader *h, struct sockaddr_in* src) {
    uint8_t buf[MAX_PACKET];
    socklen_t src_len = sizeof(*src);
    ssize_t r = recvfrom(sock, buf, MAX_PACKET, 0, (struct sockaddr*)src, &src_len);
    if (r < HEADER_SIZE) return -1;
    memcpy(h, buf, HEADER_SIZE);
    return 0;
}

static uint32_t make_fsttl(uint8_t flags, uint32_t sttl) {
    return htole32((flags & 0x1F) | (sttl << 5));
}

// O handshake não muda muito, mas é bom garantir que o timeout seja usado apenas para a conexão inicial.
static int handshake(int sock, struct sockaddr_in *dst,
                     uint8_t sid[16], uint32_t *sttl, uint32_t *peer_seq)
{
    SlowHeader h = {0};
    h.f_sttl = make_fsttl(F_CONNECT, 0);
    h.window = htole16(MAX_PACKET * 10); // Informa um buffer grande

    // Lógica de retentativa para o handshake
    for (int i=0; i < MAX_RETRIES; i++) {
        if (send_pkt(sock, dst, &h, NULL, 0) < 0) { perror("send CONNECT"); return -1; }

        struct timeval tv = { .tv_sec = TIMEOUT_SEC, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        if (recv_hdr(sock, &h, dst) == 0) {
             uint32_t f_sttl_host = le32toh(h.f_sttl);
             if ((f_sttl_host & 0x1F) & F_ACCEPT) {
                memcpy(sid, h.sid, 16);
                *sttl = f_sttl_host >> 5;
                *peer_seq = le32toh(h.seqnum);
                printf("Handshake ok: sttl=%u, peer_seq=%u\n", *sttl, *peer_seq);

                // Desativa o timeout de recebimento para usar select() no loop principal
                tv.tv_sec = 0; tv.tv_usec = 0;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                return 0;
             }
        }
        fprintf(stderr, "Tentativa de handshake %d falhou...\n", i+1);
    }
    fprintf(stderr, "Conexão rejeitada ou timeout.\n"); return -1;
}

/**
 * @brief Função principal que envia um arquivo usando Janela Deslizante.
 */
static int send_file_sliding_window(int sock, struct sockaddr_in *dst, uint8_t sid[16],
                                    uint32_t sttl, uint32_t initial_peer_seq, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror("fopen"); return -1; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *file_buf = malloc(sz);
    if (!file_buf) { perror("malloc"); fclose(f); return -1; }
    fread(file_buf, 1, sz, f);
    fclose(f);

    uint32_t nfrag = (sz == 0) ? 1 : (sz + MAX_DATA - 1) / MAX_DATA;
    printf("Arquivo de %ld bytes será enviado em %u fragmentos.\n", sz, nfrag);

    // Buffer para guardar o estado dos pacotes enviados
    InFlightPacket *sent_packets = calloc(nfrag + 1, sizeof(InFlightPacket));
    
    // Variáveis da Janela Deslizante
    uint32_t window_base = 1;
    uint32_t next_seq_num = 1;
    uint32_t peer_window = 10; // Começa com uma janela conservadora
    uint32_t last_ack_received = initial_peer_seq;

    while (window_base <= nfrag) {
        // 1. ENVIAR NOVOS PACOTES (se a janela permitir)
        while (next_seq_num < window_base + peer_window && next_seq_num <= nfrag) {
            uint32_t offset = (next_seq_num - 1) * MAX_DATA;
            size_t chunk = (sz - offset > MAX_DATA) ? MAX_DATA : sz - offset;
            bool is_last = (next_seq_num == nfrag);

            // Prepara o cabeçalho
            SlowHeader h = {0};
            memcpy(h.sid, sid, 16);
            uint8_t flags = F_ACK | (is_last ? 0 : F_MORE);
            h.f_sttl = make_fsttl(flags, sttl);
            h.seqnum = htole32(next_seq_num);
            h.acknum = htole32(last_ack_received);
            h.window = htole16(MAX_PACKET * 10); // Nossa janela
            h.fid = (nfrag > 1) ? 1 : 0;
            h.fo = next_seq_num - 1;
            
            // Envia o pacote
            send_pkt(sock, dst, &h, file_buf + offset, chunk);
            printf("--> Enviado pacote seq=%u\n", next_seq_num);

            // Armazena estado do pacote
            sent_packets[next_seq_num].seqnum = next_seq_num;
            sent_packets[next_seq_num].sent_time = time(NULL);
            sent_packets[next_seq_num].ack_received = false;

            next_seq_num++;
        }

        // 2. ESPERAR POR ACKS (usando select para não bloquear) e verificar TIMEOUTS
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);
        
        struct timeval select_timeout = { .tv_sec = TIMEOUT_SEC, .tv_usec = 0 };

        int rv = select(sock + 1, &read_fds, NULL, NULL, &select_timeout);

        if (rv > 0) { // Temos um ACK!
            SlowHeader ack_h;
            if (recv_hdr(sock, &ack_h, dst) == 0) {
                uint32_t acked_seq = le32toh(ack_h.acknum);
                printf("<-- Recebido ACK para seq=%u\n", acked_seq);

                if (acked_seq >= window_base && acked_seq < next_seq_num) {
                    sent_packets[acked_seq].ack_received = true;
                    last_ack_received = le32toh(ack_h.seqnum); // Atualiza o último seq do central
                    peer_window = le16toh(ack_h.window); // ATUALIZA A JANELA DO RECEPTOR
                    if (peer_window == 0) peer_window = 1; // Evitar janela 0 para não parar
                }
            }
        }
        
        // 3. DESLIZAR A JANELA
        while (window_base <= nfrag && sent_packets[window_base].ack_received) {
            window_base++;
            printf("Janela deslizou, nova base: %u\n", window_base);
        }

        // 4. VERIFICAR E REENVIAR PACOTES COM TIMEOUT
        time_t now = time(NULL);
        for (uint32_t i = window_base; i < next_seq_num; i++) {
            if (!sent_packets[i].ack_received && (now - sent_packets[i].sent_time) >= TIMEOUT_SEC) {
                printf("!!! TIMEOUT para pacote seq=%u. Reenviando.\n", i);
                
                uint32_t offset = (i - 1) * MAX_DATA;
                size_t chunk = (sz - offset > MAX_DATA) ? MAX_DATA : sz - offset;
                bool is_last = (i == nfrag);

                SlowHeader h = {0};
                memcpy(h.sid, sid, 16);
                uint8_t flags = F_ACK | (is_last ? 0 : F_MORE);
                h.f_sttl = make_fsttl(flags, sttl);
                h.seqnum = htole32(i);
                h.acknum = htole32(last_ack_received);
                h.window = htole16(MAX_PACKET * 10);
                h.fid = (nfrag > 1) ? 1 : 0;
                h.fo = i - 1;

                send_pkt(sock, dst, &h, file_buf + offset, chunk);
                sent_packets[i].sent_time = time(NULL); // Reseta o timer do pacote
            }
        }
    }

    free(file_buf);
    free(sent_packets);
    printf("Envio de %u fragmentos concluído.\n", nfrag);
    return 0;
}


static int disconnect_slow(int sock, struct sockaddr_in *dst, uint8_t sid[16],
                           uint32_t sttl, uint32_t peer_seq)
{
    SlowHeader h = {0};
    memcpy(h.sid, sid, 16);
    
    uint8_t flags = F_CONNECT | F_REVIVE | F_ACK;
    h.f_sttl = make_fsttl(flags, sttl);
    h.seqnum = htole32(peer_seq); // Usa o último seq do peer como base
    h.acknum = htole32(peer_seq);

    return send_pkt(sock, dst, &h, NULL, 0);
}


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

    if (getaddrinfo(argv[1], "7033", &hints, &res) != 0) {
        perror("getaddrinfo"); return 1;
    }
    memcpy(&dst, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    uint8_t sid[16];
    uint32_t sttl, peer_seq;
    printf("Realizando handshake com %s...\n", argv[1]);
    if (handshake(sock, &dst, sid, &sttl, &peer_seq) < 0) {
        close(sock);
        return 1;
    }

    printf("Enviando arquivo '%s'...\n", argv[2]);
    if (send_file_sliding_window(sock, &dst, sid, sttl, peer_seq, argv[2]) < 0) {
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