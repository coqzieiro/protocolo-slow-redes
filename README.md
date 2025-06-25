# README

Este projeto implementa um _peripheral_ e um _servidor_ para testar o protocolo SLOW (camada de transporte sobre UDP/7033).  

## Requisitos

- GCC (compatível com C99)
- Sistema Linux/Unix

## Compilação

No diretório raiz do projeto, execute:

```bash
gcc -std=c99 -Wall -Wextra protocolo-slow.c -o protocolo -W
gcc -std=c99 -Wall -Wextra servidor.c -o servidor -W
````

* `-std=c99` ativa o padrão C99.
* `-Wall -Wextra` habilita avisos de compilação.
* `-W` inclui warnings adicionais

## Execução

1. **Inicie o servidor** em um terminal.

   ```bash
   ./servidor arquivo_recebido.dat
   ```

   * `arquivo_recebido.dat`: caminho onde o servidor irá gravar os dados recebidos

2. **Execute o cliente (peripheral)** em outro terminal:

   ```bash
   ./protocolo 127.0.0.1 mensagem.txt
   ```

   * `127.0.0.1`: endereço IPv4 do servidor central
   * `mensagem.txt`: caminho do arquivo que você quer enviar

### Exemplo completo

Num terminal (servidor):

```bash
$ ./servidor recebido.dat
Aguardando CONNECT na porta 7033...
CONNECT de 127.0.0.1:XXXXX
SETUP enviado (ACCEPT), sttl=1000, seq=0
Fragmento 0 recebido (N bytes), enviado ACK 0
...
Disconnect recebido.
Arquivo recebido em 'recebido.dat'. Servidor encerrado.
```

Em outro terminal (cliente):

```bash
$ ./protocolo 127.0.0.1 meu_arquivo.txt
Handshake ok: sttl=1000, peer_seq=0
Enviado M fragments
```

Após o término, verifique o conteúdo:

```bash
cat recebido.dat
```

## Estrutura do código

* **protocolo-slow\.c**
  Implementa o *peripheral*:

  * 3-way handshake (CONNECT → SETUP)
  * Stop-and-wait com fragmentação (DATA → ACK)
  * Disconnect (CONNECT|REVIVE|ACK)

* **servidor.c**
  Simula o *central*:

  * Recebe CONNECT, responde ACCEPT
  * Recebe fragmentos, grava em arquivo e responde ACK
  * Recebe DISCONNECT e encerra sessão