import socket
import time
import uuid

# Configurações do servidor
SERVER_HOST = '0.0.0.0'
SERVER_PORT = 7033
BUFFER_SIZE = 1472

# Função para gerar um UUID para a sessão (simulando o comportamento do SLOW)
def generate_uuid():
    return uuid.uuid4().bytes

# Função para simular a resposta de conexão (3-way connect)
def handle_connect(data, addr, sock):
    # Imprimir o conteúdo do pacote recebido e os bytes específicos
    print(f"Pacote CONNECT recebido: {data}")
    print(f"Byte 3 (Flag Connect): {data[3]:#04x}")  # Imprime o valor da flag como hexadecimal
    
    if data[3] == 0x01:  # Se a flag "Connect" estiver ativada
        print("Recebido pedido de conexão, enviando resposta de 'Accept'")
        
        # Gerar resposta de "accept"
        response = bytearray(data)  # Criar uma cópia da mensagem recebida
        response[7] = 0x01  # Flag "Accept"
        response[6] = 0x00  # Resetando a flag de "Revive"
        
        # Enviar a resposta de "accept"
        sock.sendto(response, addr)

# Função para simular o envio de dados e o envio de ack
def handle_data(data, addr, sock):
    # Imprimir o conteúdo do pacote de dados recebido
    print(f"Pacote DATA recebido: {data}")
    print(f"Byte 3 (Flag Data): {data[3]:#04x}")  # Imprime o valor da flag como hexadecimal
    
    if data[3] == 0x10:  # Se for pacote de dados (flag 0x10)
        print("Recebido pacote de dados, enviando ack")
        
        # Enviar ack de volta
        response = bytearray(data)  # Copiar a mensagem recebida
        sock.sendto(response, addr)

# Função para simular a desconexão
def handle_disconnect(data, addr, sock):
    # Imprimir o conteúdo do pacote de desconexão recebido
    print(f"Pacote DISCONNECT recebido: {data}")
    print(f"Byte 3 (Flag Disconnect): {data[3]:#04x}")  # Imprime o valor da flag como hexadecimal
    
    if data[3] == 0x02:  # Se for a flag de desconexão
        print("Recebido pedido de desconexão, enviando ack de desconexão")
        
        # Enviar ack de desconexão
        response = bytearray(data)  # Copiar a mensagem recebida
        sock.sendto(response, addr)

def main():
    # Criar socket UDP
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((SERVER_HOST, SERVER_PORT))

    print(f"Servidor aguardando na porta {SERVER_PORT}...")

    while True:
        # Receber dados do peripheral
        data, addr = sock.recvfrom(BUFFER_SIZE)
        print(f"Pacote recebido de {addr}")
        
        # Imprimir o conteúdo do pacote recebido para diagnóstico
        print(f"Conteúdo do pacote recebido: {data}")

        # Imprimir os 16 primeiros bytes para entender o pacote
        print(f"Primeiros 16 bytes do pacote: {data[:16]}")

        # Verifique a flag para 3-way connect e envie a resposta "accept"
        if data[3] == 0x01:  # Se for um pacote de conexão
            handle_connect(data, addr, sock)

        # Se o pacote de dados for recebido, envie ack de volta
        elif data[3] == 0x10:  # Se for pacote de dados (flag 0x10)
            handle_data(data, addr, sock)

        # Se o pacote de desconexão for recebido, envie ack de desconexão
        elif data[3] == 0x02:  # Se for pacote de desconexão
            handle_disconnect(data, addr, sock)

        time.sleep(1)  # Simula algum atraso

if __name__ == "__main__":
    main()
