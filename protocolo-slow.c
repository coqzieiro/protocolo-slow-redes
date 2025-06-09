#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <uuid/uuid.h>

#define PORT 7033
#define MAX_PACKET_SIZE 1472
#define MAX_DATA_SIZE 1440

// Estrutura do pacote SLOW
typedef struct {
    uint8_t sid[16];      // Session ID (UUIDv8)
    uint32_t sttl;        // Session TTL
    uint8_t flags;        // Flags (C R ACK A/R MB)
    uint32_t seqnum;      // Sequence Number
    uint32_t acknum;      // Acknowledgement Number
    uint16_t window;      // Window Size
    uint8_t fid;          // Fragment ID
    uint8_t fo;           // Fragment Offset
    uint8_t data[MAX_DATA_SIZE]; // Data (max 1440 bytes)
} slow_packet_t;

// Função para gerar UUID
void generate_uuid(uint8_t *uuid) {
    uuid_t bin_uuid;
    uuid_generate(bin_uuid);  // Gera um UUID
    memcpy(uuid, bin_uuid, 16);  // Copia para o sid

    // Imprime o UUID gerado (para debug)
    printf("UUID Gerado: ");
    for (int i = 0; i < 16; i++) {
        printf("%02x", uuid[i]);
    }
    printf("\n");
}

// Função para enviar um pacote
void send_packet(int sockfd, struct sockaddr_in *server_addr, slow_packet_t *packet) {
    if (sendto(sockfd, packet, sizeof(slow_packet_t), 0, (struct sockaddr *)server_addr, sizeof(*server_addr)) < 0) {
        perror("Erro ao enviar pacote");
        exit(EXIT_FAILURE);
    }
}

// Função para receber um pacote
void receive_packet(int sockfd, slow_packet_t *packet) {
    struct sockaddr_in from_addr;
    socklen_t addr_len = sizeof(from_addr);
    if (recvfrom(sockfd, packet, sizeof(slow_packet_t), 0, (struct sockaddr *)&from_addr, &addr_len) < 0) {
        perror("Erro ao receber pacote");
        exit(EXIT_FAILURE);
    }
}

// Função para enviar dados e aguardar ACK com retransmissão se necessário
void send_data_with_retries(int sockfd, struct sockaddr_in *server_addr, uint8_t *data, size_t data_size) {
    slow_packet_t packet;
    memset(&packet, 0, sizeof(packet));

    generate_uuid(packet.sid);
    packet.sttl = 0;
    packet.flags = 0x01; // Ack flag
    packet.seqnum = 1;
    packet.acknum = 0;
    packet.window = 16;
    packet.fid = 0;
    packet.fo = 0;
    memcpy(packet.data, data, data_size);

    // Enviar pacote inicial
    send_packet(sockfd, server_addr, &packet);

    // Aguardar confirmação (ACK) por 2 segundos, com tentativas
    int retries = 5;
    slow_packet_t response;
    while (retries > 0) {
        receive_packet(sockfd, &response);
        if (response.flags == 0x01) { // Verificar ACK
            printf("Dados confirmados\n");
            return; // ACK recebido, dados confirmados
        }
        // Se não recebeu ACK, tentar reenviar
        retries--;
        printf("Reenviando dados, tentativas restantes: %d\n", retries);
        send_packet(sockfd, server_addr, &packet); // Reenvia o pacote
        sleep(1); // Aguarda um segundo antes de tentar novamente
    }

    // Caso o ACK não tenha sido recebido após várias tentativas
    printf("Falha ao receber confirmação, terminando...\n");
}

// Função para construir e enviar a mensagem de "3-way connect"
void send_connect(int sockfd, struct sockaddr_in *server_addr) {
    slow_packet_t packet;
    memset(&packet, 0, sizeof(packet));

    // SID vazio (Nil UUID)
    memset(packet.sid, 0, 16);  // SID inicialmente vazio
    packet.sttl = 0; // Session TTL
    packet.flags = 0x01; // Connect flag
    packet.seqnum = 0;
    packet.acknum = 0;
    packet.window = 16; // Tamanho do buffer (exemplo)
    packet.fid = 0;
    packet.fo = 0;

    // Gerar UUID para a sessão
    generate_uuid(packet.sid);

    // Envia o pacote
    send_packet(sockfd, server_addr, &packet);
}

// Função para enviar dados
void send_data(int sockfd, struct sockaddr_in *server_addr, uint8_t *data, size_t data_size) {
    slow_packet_t packet;
    memset(&packet, 0, sizeof(packet));

    // SID e TTL devem ser definidos após o 3-way connect
    generate_uuid(packet.sid);
    packet.sttl = 0; // Exemplo de TTL
    packet.flags = 0x01; // Ack flag
    packet.seqnum = 1; // Sequência
    packet.acknum = 0; // Acknowledgement número (atualmente 0)
    packet.window = 16; // Janela do buffer (tamanho exemplo)
    packet.fid = 0; // Fragment ID
    packet.fo = 0; // Fragment Offset
    memcpy(packet.data, data, data_size); // Copia os dados para o campo 'data'

    // Envia o pacote
    send_packet(sockfd, server_addr, &packet);
}

// Função para enviar o pacote de desconexão
void send_disconnect(int sockfd, struct sockaddr_in *server_addr) {
    slow_packet_t packet;
    memset(&packet, 0, sizeof(packet));

    // Gerar UUID para a sessão
    generate_uuid(packet.sid);
    packet.sttl = 0; // Exemplo de TTL
    packet.flags = 0x03; // Flag de desconexão (connect + revive)
    packet.seqnum = 1; // Sequência
    packet.acknum = 0; // Número de confirmação
    packet.window = 0; // Janela 0 (sem espaço no buffer)
    packet.fid = 0; // Fragment ID
    packet.fo = 0; // Fragment Offset

    // Envia o pacote de desconexão
    send_packet(sockfd, server_addr, &packet);
}

int main() {
    int sockfd;
    struct sockaddr_in server_addr;

    // Criar socket UDP
    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Erro ao criar socket");
        exit(EXIT_FAILURE);
    }

    // Configuração do servidor (central)
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = INADDR_ANY;

    // Enviar 3-way connect
    send_connect(sockfd, &server_addr);

    // Esperar por resposta do servidor
    slow_packet_t response;
    receive_packet(sockfd, &response);

    // Verificar a resposta (exemplo: Accept ou Fail)
    if (response.flags == 0x01) {
        printf("Conexão aceita\n");

        // Enviar dados
        uint8_t data[MAX_DATA_SIZE] = "Dados de exemplo";
        // Preencher o pacote de dados
        send_data_with_retries(sockfd, &server_addr, data, strlen((char *)data));

        // Esperar por confirmação de dados
        receive_packet(sockfd, &response);
        if (response.flags == 0x01) {
            printf("Dados confirmados\n");
        }

        // Enviar desconexão
        send_disconnect(sockfd, &server_addr);
    } else {
        printf("Conexão falhou\n");
    }

    close(sockfd);
    return 0;
}
