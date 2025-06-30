# Cliente do Protocolo SLOW

## Descrição

Este projeto é uma implementação em C de um cliente (peripheral) para o protocolo de transporte customizado **SLOW**. O programa estabelece uma conexão com um servidor, transfere um arquivo de forma confiável e encerra a sessão.

O funcionamento se baseia em um laço principal que orquestra as três fases da comunicação:
1.  **Handshake**: Uma conexão é estabelecida com o servidor `slow.gmelodie.com:7033`.
2.  **Transferência de Arquivo**: O arquivo especificado é lido, dividido em fragmentos de no máximo 1440 bytes e enviado. O programa implementa uma lógica de "parar-e-esperar", aguardando a confirmação (ACK) de cada fragmento antes de enviar o próximo e reenviando em caso de timeout.
3.  **Desconexão**: Uma mensagem de `Disconnect` é enviada para encerrar a sessão de forma limpa.

## Nome e número USP dos membros

* Fernando Alee Suaiden - 12680836
* Felipe da Costa Coqueiro - 11781361
* Flávio Masaaki Ito - 12609046

## Como Usar

O programa foi desenvolvido para ser compilado e executado em um ambiente Linux.

### Compilação

Use o GCC para compilar o código-fonte `protocolo-slow.c`:

```bash
gcc -Wall protocolo-slow.c -o meu_peripheral
```
### Execução

Para executar o programa, forneça o hostname do servidor e o caminho do arquivo que deseja enviar.

```Bash
./meu_peripheral slow.gmelodie.com nome_do_arquivo.txt
```

### Exemplo prático:
```Bash
# Exemplo enviando um arquivo de texto chamado "mensagem.txt", que já está criado no repositório
./meu_peripheral slow.gmelodie.com mensagem.txt
```
