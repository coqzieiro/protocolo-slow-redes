# Cliente do Protocolo SLOW

## Descrição

Este projeto é uma implementação em C de um cliente (`peripheral`) para o protocolo de transporte customizado **SLOW**. O programa estabelece uma conexão com um servidor, transfere um arquivo de forma confiável e encerra a sessão.

A implementação utiliza funcionalidades essenciais para garantir uma transferência de dados robusta sobre UDP:
* **Fragmentação**: Arquivos grandes são divididos em pacotes menores, com no máximo 1440 bytes de dados.
* **Controle de Fluxo com Janela Deslizante**: Em vez de esperar pela confirmação de cada pacote, o cliente envia múltiplos pacotes até preencher a janela de recepção do servidor. A janela "desliza" para frente à medida que as confirmações (`ACKs`) são recebidas, permitindo o envio contínuo de dados e maximizando a vazão.
* **Retransmissão por Timeout**: Um mecanismo de timeout garante que pacotes perdidos (cujos `ACKs` não chegam a tempo) sejam reenviados para o servidor.

O funcionamento se baseia em um laço principal que orquestra as três fases da comunicação:
1.  **Handshake**: Uma conexão de 3 vias (`3-way connect`) é estabelecida com o servidor `slow.gmelodie.com:7033`.
2.  **Transferência de Arquivo**: O arquivo especificado é lido, fragmentado e enviado usando a lógica de janela deslizante descrita acima.
3.  **Desconexão**: Uma mensagem de `Disconnect` é enviada para encerrar a sessão de forma limpa.

## Nome e número USP dos membros

* Fernando Alee Suaiden - 12680836
* Felipe da Costa Coqueiro - 11781361
* Flávio Masaaki Ito - 12609046

## Como Usar

### Compilação

Renomeie o arquivo de código-fonte para `slow_client.c` e use o GCC para compilá-lo:

```bash
gcc -Wall slow_client.c -o slow_client
```

### Execução

Para executar o programa, forneça o hostname do servidor e o caminho do arquivo que deseja enviar.

```Bash
./slow_client slow.gmelodie.com <caminho_para_o_arquivo>
```

### Exemplo Prático

Para testar o comportamento da janela deslizante, é ideal usar um arquivo grande. Você pode criar um arquivo de teste de 1MB com o seguinte comando:

```Bash
# Cria um arquivo de teste chamado 'arquivo_grande.dat' com 1MB de dados aleatórios
dd if=/dev/urandom of=arquivo_grande.dat bs=1M count=1
```

Agora, execute o cliente com este arquivo:

```Bash
./slow_client slow.gmelodie.com arquivo_grande.dat
```

No terminal: você verá uma rajada inicial de pacotes sendo enviados sem pausas, seguida por um fluxo contínuo de confirmações (ACKs) chegando e novos pacotes sendo enviados, demonstrando a janela deslizante em ação.