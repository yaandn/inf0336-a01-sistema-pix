# INF0336 – A01: Sincronismo do Sistema PIX (em C)

Dois programas em C com POSIX threads (pthreads), mutex, semáforos e sinais, todos conceitos dos slides.

```
sistema_pix.c   -> código único com as Questões 1, 2 e 3
saida_q1.txt    -> saída real do experimento da Q1
saida_q2.txt    -> saída real da Q2/Q3 (com dois Ctrl+C no meio)
```

## Compilar e executar

```bash
gcc -O0 -Wall -pthread sistema_pix.c -o sistema_pix

./sistema_pix                   # menu interativo
./sistema_pix q1                # Q1: experimento N = 1..512, sem e com mutex
./sistema_pix q1 64 1000 0      # Q1: uma execução, N=64, K=1000, sem proteção
./sistema_pix q1 64 1000 1      # idem, com mutex
./sistema_pix q2                # Q2/Q3: caso de exemplo (pool = 3, 8 PIX)
./sistema_pix q2 4 20           # Q2/Q3: pool de 4 threads, 20 PIX aleatórios
# durante a Q2/Q3: Ctrl+C -> mostra a fila | Ctrl+\ -> aborta
```

O código está dividido em blocos comentados: utilidades compartilhadas → Questão 1 (`questao1`) → Questões 2/3 (`questao2_3`) → `main`.

---

## Questão 1: a condição de corrida

**Modelo N:1:** N remetentes enviam PIX para **um único** recebedor (conta 0). Cada remetente é atendido por uma thread, e todas compartilham a base de dados `contas[]`.

| Conceito (le07) | No sistema PIX |
|---|---|
| Recurso compartilhado | `contas[0].saldo` (saldo do recebedor) |
| Seção crítica | ler saldo → processar → escrever `saldo + valor` |
| Condição de corrida | duas threads leem o **mesmo saldo antigo**; a segunda escrita apaga o crédito da primeira |

Intercalação que causa a perda (igual ao exemplo `counter++` dos slides):

```
T1: saldo = contas[0].saldo        // lê 100
T2: saldo = contas[0].saldo        // lê 100   <- troca de contexto
T1: contas[0].saldo = 100 + 1      // 101
T2: contas[0].saldo = 100 + 1      // 101      <- crédito de T1 perdido!
```

A função `acesso_bd()` simula a latência de acesso ao banco de dados, o que alarga a janela entre a leitura e a escrita (como acontece num sistema real).

### Resultado do experimento (máquina com 2 núcleos, K = 1000 PIX de R$1 por remetente)

| N | Esperado | Pior saldo obtido | R$ perdidos | Execuções com erro |
|---:|---:|---:|---:|---:|
| 1 | 1.000 | 1.000 | 0 | 0/5 |
| 2 | 2.000 | 1.080 | 920 | 5/5 |
| 8 | 8.000 | 3.043 | 4.957 | 3/5 |
| 64 | 64.000 | 40.812 | 23.188 | 5/5 |
| 512 | 512.000 | 304.518 | 207.482 | 5/5 |

**Conclusão:** com **N = 1** não existe concorrência e o saldo fica sempre correto. **A partir de N = 2** o saldo do recebedor já fica inconsistente, e a perda cresce com N, porque mais threads disputam a mesma seção crítica. Com `pthread_mutex_lock/unlock` envolvendo a seção crítica, todas as execuções dão o valor exato (segunda tabela em `saida_q1.txt`).

> Os números mudam a cada execução (o escalonamento não é determinístico) e dependem da quantidade de núcleos, mas o padrão "N ≥ 2 → inconsistente" se repete.

---

## Questão 2: proteção mínima + fila FCFS

Arquitetura (thread pool dos slides le05_02 + bounded buffer do le08):

```
 requisições PIX ──► enfileira() ──► [ HEAD  P6 → P7 → P8  TAIL ] ──► T0 T1 T2 (pool)
                      (tail)          mutex fila.lock                 retiram do head
                                      semáforo fila.itens             (FCFS)
                                                         │
                                                         ▼
                                       mutex_bd → débito + crédito atômicos
```

- **`mutex_bd`**: protege a seção crítica da base de contas. O débito e o crédito acontecem juntos, então nenhum PIX se perde e o dinheiro total é conservado.
- **Fila FIFO (`head`/`tail`)**: quando chegam mais requisições do que o tamanho da pool, elas esperam na fila (estado *READY*).
- **Escalonador FCFS**: cada thread livre retira sempre do `head`, ou seja, a ordem de atendimento é a ordem de chegada (não preemptivo).
- **Semáforo contador `fila.itens`**: as threads da pool bloqueiam em `sem_wait` quando a fila está vazia, **sem busy waiting**; `sem_post` acorda uma delas quando chega uma requisição.
- **Sentinelas**: no fim, uma requisição com `id = -1` por thread encerra a pool de forma limpa.

### Caso de exemplo (pool = 3)

| PIX | Chegada | Burst | Início | Fim | Espera | Thread |
|---|---:|---:|---:|---:|---:|---|
| P1 | 0 | 3000 | 0 | 3000 | 0 | T2 |
| P2 | 0 | 2000 | 0 | 2000 | 0 | T1 |
| P3 | 500 | 1000 | 500 | 1500 | 0 | T0 |
| P4 | 1000 | 4000 | 1500 | 5500 | 500 | T0 |
| P5 | 1500 | 1500 | 2000 | 3500 | 500 | T1 |
| P6 | 2000 | 1000 | 3000 | 4000 | 1000 | T2 |
| P7 | 2500 | 2000 | 3500 | 5500 | 1000 | T1 |
| P8 | 3000 | 500 | 4000 | 4500 | 1000 | T2 |

Tempo médio de espera: **500 ms** · Turnaround médio: **2375 ms** · Dinheiro no sistema: R$ 5000 → R$ 5000 (**consistente**).

O P4 chega em 1000 ms com as 3 threads ocupadas, por isso espera na fila até o P3 terminar em 1500 ms. A partir daí as requisições são atendidas estritamente pela ordem de chegada. *Efeito comboio* (le06): o P8 (burst de 500 ms) espera 1000 ms atrás de requisições mais longas.

---

## Questão 3: debugging por sinais

- `signal(SIGINT, tratador_sigint)` registra o tratador do **Ctrl+C**.
- O tratador só faz `sem_post(&sem_debug)`, porque `printf` **não é async-signal-safe** e `sem_post` é.
- A **thread de debug** fica bloqueada em `sem_wait(&sem_debug)`. Quando é acordada, trava `fila.lock` (para ler um retrato consistente) e imprime:
  - o estado de cada thread da pool (RUNNING Px ou livre) e quanto do burst já executou;
  - a fila FCFS do `HEAD` ao `TAIL`, com quanto tempo cada requisição está esperando;
  - os saldos atuais.
- Como o Ctrl+C foi capturado, o programa **não termina**: use **Ctrl+\\** (SIGQUIT) para abortar.
- Detalhe de robustez: um sinal interrompe o `usleep` (EINTR). Por isso `dormir_ms()` dorme em laço até o prazo, e um Ctrl+C não "adianta" a chegada nem encurta o burst das requisições.

Exemplo real (Ctrl+C em t ≈ 2,2 s):

```
================= DEBUG (Ctrl+C) t = 2202 ms =================
Thread pool (3 threads):
  T0: RUNNING P4   executou   702 ms de  4000 ms
  T1: RUNNING P5   executou   202 ms de  1500 ms
  T2: RUNNING P1   executou  2202 ms de  3000 ms
Fila FCFS (READY) - 1 requisicao(oes) aguardando:
  HEAD -> [P6 R$10 espera 202ms] -> TAIL
Saldos:  c0=R$1120  c1=R$1000  c2=R$950  c3=R$930  c4=R$1000
==============================================================
```

---

## Roteiro sugerido para o vídeo (até 5 min)

1. **(0:00–0:40)** Apresentar o modelo N:1 e apontar no código o recurso e a seção crítica (`q1_pix()` em `sistema_pix.c`).
2. **(0:40–1:50)** Rodar `./sistema_pix q1`: mostrar N=1 correto, inconsistência a partir de N=2 e o mutex zerando as perdas.
3. **(1:50–3:20)** Mostrar a estrutura da fila, o `mutex_bd`, o semáforo e o laço do worker; rodar `./sistema_pix q2`.
4. **(3:20–4:30)** Apertar Ctrl+C duas vezes durante a execução e explicar a saída do debug; encerrar com o relatório FCFS (saldo consistente).
5. **(4:30–5:00)** Conclusão: o que muda com e sem sincronização.
