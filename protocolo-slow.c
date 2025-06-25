#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>

#define PORT 7033
#define MAX_PACKET 1472
#define HEADER_SIZE 32
#define MAX_DATA (MAX_PACKET - HEADER_SIZE)
#define TIMEOUT_SEC 1
#define MAX_RETRIES 5

/* --- flags --- */
enum {
    F_CONNECT = 1<<4,
    F_REVIVE  = 1<<3,
    F_ACK     = 1<<2,
    F_ACCEPT  = 1<<1,
    F_MORE    = 1<<0
};

/* --- cabeçalho SLOW (packed para não criar padding) --- */
#pragma pack(push,1)
typedef struct {
    uint8_t  sid[16];      // session ID
    uint32_t f_sttl;       // flags (5 bits) | sttl (27 bits)
    uint32_t seqnum;       // número de sequência
    uint32_t acknum;       // número de ack
    uint16_t window;       // janela
    uint8_t  fid;          // file ID
    uint8_t  fo;           // fragment offset
} SlowHeader;
#pragma pack(pop)

/* define timeout de recv em segundos */
static void set_timeout(int sock) {
    struct timeval tv = { .tv_sec = TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* envia header + opcional payload */
static int send_pkt(int sock, struct sockaddr_in *dst, SlowHeader *h,
                    const uint8_t *data, size_t len)
{
    uint8_t buf[HEADER_SIZE + MAX_DATA];
    memcpy(buf, h, HEADER_SIZE);
    if (data && len>0) memcpy(buf+HEADER_SIZE, data, len);
    return sendto(sock, buf, HEADER_SIZE+len, 0,
                  (struct sockaddr*)dst, sizeof(*dst)) < 0 ? -1:0;
}

/* recebe header (payload é ignorado) */
static int recv_hdr(int sock, SlowHeader *h) {
    uint8_t buf[HEADER_SIZE];
    ssize_t r = recv(sock, buf, HEADER_SIZE, 0);
    if (r<HEADER_SIZE) return -1;
    memcpy(h, buf, HEADER_SIZE);
    return 0;
}

/* constrói f_sttl a partir de flags e sttl */
static uint32_t make_fsttl(uint8_t flags, uint32_t sttl) {
    return ( ((uint32_t)flags<<27) | (sttl & 0x07FFFFFFu) );
}

/* 3-way handshake */
static int handshake(int sock, struct sockaddr_in *dst,
                     uint8_t sid[16], uint32_t *sttl, uint32_t *peer_seq)
{
    SlowHeader h = {0};
    /* 1) CONNECT */
    h.f_sttl = make_fsttl(F_CONNECT, 0);
    if (send_pkt(sock, dst, &h, NULL, 0) < 0) { perror("send CONNECT"); return -1; }

    /* 2) SETUP (aguarda ACCEPT) */
    if (recv_hdr(sock, &h) < 0) { perror("recv SETUP"); return -1; }
    if (!( (h.f_sttl>>27) & F_ACCEPT )) {
        fprintf(stderr, "Rejeitado pelo central\n"); return -1;
    }
    /* salva SID e TTL e seq inicial do central */
    memcpy(sid, h.sid, 16);
    *sttl      = h.f_sttl & 0x07FFFFFFu;
    *peer_seq  = h.seqnum;
    printf("Handshake ok: sttl=%u, peer_seq=%u\n", *sttl, *peer_seq);
    return 0;
}

/* send-and-wait-ack de um fragmento */
static int send_fragment(int sock, struct sockaddr_in *dst,
                         uint8_t sid[16], uint32_t sttl,
                         uint32_t *my_seq, uint32_t acknum,
                         const uint8_t *data, size_t len,
                         uint8_t fid, uint8_t fo)
{
    SlowHeader h;
    int tries=0;
    while (tries++<MAX_RETRIES) {
        memset(&h,0,sizeof(h));
        memcpy(h.sid, sid, 16);
        uint8_t flags = F_ACK | (fo<255?F_MORE:0);
        h.f_sttl  = make_fsttl(flags, sttl);
        h.seqnum  = *my_seq;
        h.acknum  = acknum;
        h.window  = htons(MAX_PACKET);
        h.fid     = fid;
        h.fo      = fo;

        if (send_pkt(sock,dst,&h,data,len)<0) { perror("send DATA"); return -1; }
        /* espera ack */
        if (recv_hdr(sock, &h)==0
            && ( (h.f_sttl>>27)&F_ACK )
            && h.acknum==*my_seq )
        {
            (*my_seq)++;
            return 0;
        }
        fprintf(stderr,"retry fragment %u\n", *my_seq);
    }
    return -1;
}

/* envia todo o arquivo em fragments */
static int send_file(int sock, struct sockaddr_in *dst,
                     uint8_t sid[16], uint32_t sttl, uint32_t peer_seq,
                     const char *path)
{
    FILE *f = fopen(path,"rb");
    if (!f) { perror("fopen"); return -1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *buf = malloc(sz);
    fread(buf,1,sz,f); fclose(f);

    uint32_t nfrag = (sz + MAX_DATA-1)/MAX_DATA;
    uint8_t fid = nfrag>1?1:0;
    uint32_t offset=0, my_seq=0;

    for(uint32_t i=0;i<nfrag;i++) {
        size_t chunk = sz-offset > MAX_DATA? MAX_DATA: sz-offset;
        if (send_fragment(sock,dst,sid,sttl,&my_seq,peer_seq,
                          buf+offset,chunk,fid,i)<0)
        {
            free(buf);
            return -1;
        }
        offset+=chunk;
    }
    free(buf);
    printf("Enviado %u fragments\n", nfrag);
    return 0;
}

/* disconnect simples */
static int disconnect_slow(int sock, struct sockaddr_in *dst,
                          uint8_t sid[16], uint32_t sttl, uint32_t peer_seq)
{
    SlowHeader h = {0};
    memcpy(h.sid,sid,16);
    h.f_sttl = make_fsttl(F_CONNECT|F_REVIVE|F_ACK, sttl);
    h.seqnum = 0; h.acknum = peer_seq;
    return send_pkt(sock,dst,&h,NULL,0);
}

int main(int argc, char **argv)
{
    if (argc!=3) {
        fprintf(stderr,"Uso: %s <IP_central> <arquivo>\n",argv[0]);
        return 1;
    }

    /* cria socket UDP */
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    set_timeout(sock);

    /* preenche destino IPv4 */
    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port   = htons(PORT)
    };
    inet_pton(AF_INET, argv[1], &dst.sin_addr);

    /* handshake */
    uint8_t sid[16];
    uint32_t sttl, peer_seq;
    if (handshake(sock,&dst,sid,&sttl,&peer_seq)<0) return 1;

    /* envia arquivo */
    if (send_file(sock,&dst,sid,sttl,peer_seq, argv[2])<0) return 1;

    /* desconecta */
    disconnect_slow(sock,&dst,sid,sttl,peer_seq);
    close(sock);
    return 0;
}
