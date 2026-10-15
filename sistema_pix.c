/*
 * ============================================================================
 *  INF0336 - A01 - Sincronismo do Sistema PIX
 * ============================================================================
 *
 *  Q1 (5.0p) Condicao de corrida no modelo N:1 (sem protecao x com mutex)
 *  Q2 (3.0p) Protecao minima: mutex na base de contas + fila FCFS + thread pool
 *  Q3 (2.0p) Debugging por sinais: Ctrl+C (SIGINT) imprime o estado da fila
 *
 *  Compilar:
 *      gcc -O0 -Wall -pthread sistema_pix.c -o sistema_pix
 *
 *  Executar:
 *      ./sistema_pix                   -> menu
 *      ./sistema_pix q1                -> experimento Q1 (N = 1..512)
 *      ./sistema_pix q1 N K [0|1]      -> uma execucao Q1 (1 = com mutex)
 *      ./sistema_pix q2                -> Q2/Q3 caso de exemplo (pool=3, 8 PIX)
 *      ./sistema_pix q2 T M            -> Q2/Q3 pool de T threads, M PIX aleatorios
 *
 *  Durante a Q2/Q3: Ctrl+C mostra a fila (debug) | Ctrl+\ aborta
 * ============================================================================
 */
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ========================================================================== */
/*                         ESTRUTURAS COMPARTILHADAS                          */
/* ========================================================================== */

typedef struct {
    int  id;
    long saldo;
} conta_t;

struct timespec t0;                 // instante inicial (relogio da simulacao)

long agora_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
}

// Dorme ate completar `ms`, mesmo se um sinal (Ctrl+C) interromper o usleep
void dormir_ms(long ms) {
    long fim = agora_ms() + ms;
    long resta;
    while ((resta = fim - agora_ms()) > 0)
        usleep(resta * 1000);
}

/* ========================================================================== */
/*                  QUESTAO 1 - CONDICAO DE CORRIDA (N:1)                     */
/* ========================================================================== */
/*
 * Modelo N:1 -> N usuarios (remetentes) enviam PIX para 1 unico recebedor.
 * Cada remetente e atendido por uma thread; todas acessam a mesma base.
 *
 *   RECURSO .............. saldo da conta recebedora (q1_contas[0].saldo)
 *   SECAO CRITICA ........ ler saldo -> processar -> escrever saldo (credito)
 *   CONDICAO DE CORRIDA .. duas threads leem o mesmo saldo antigo e uma
 *                          sobrescreve o credito da outra (credito perdido)
 */

#define Q1_VALOR_PIX   1            // cada PIX transfere R$ 1
#define Q1_REPETICOES  5            // execucoes por valor de N no experimento

conta_t *q1_contas;                 // base de dados compartilhada
int q1_K = 1000;                    // PIX enviados por cada remetente
int q1_protegido = 0;               // 0 = sem protecao, 1 = com mutex
pthread_mutex_t q1_mutex;

// Simula a latencia de acesso a base de dados
void q1_acesso_bd(void) {
    for (volatile int i = 0; i < 50; i++);
}

void q1_pix(int origem, int destino, long valor) {
    // Debito: cada conta de origem pertence a uma unica thread -> sem disputa
    if (q1_contas[origem].saldo < valor) return;
    q1_contas[origem].saldo -= valor;

    if (q1_protegido) pthread_mutex_lock(&q1_mutex);      // entry section
    /* ================= SECAO CRITICA ================= */
    long saldo = q1_contas[destino].saldo;    // 1. le o saldo do recebedor
    q1_acesso_bd();                           // 2. processa (latencia do BD)
    q1_contas[destino].saldo = saldo + valor; // 3. escreve o novo saldo
    /* ================================================= */
    if (q1_protegido) pthread_mutex_unlock(&q1_mutex);    // exit section
}

void *q1_runner(void *param) {
    int origem = *(int *)param;
    for (int k = 0; k < q1_K; k++)
        q1_pix(origem, 0, Q1_VALOR_PIX);      // N:1 -> sempre para a conta 0
    pthread_exit(0);
}

// Executa a simulacao com N remetentes e devolve o saldo final do recebedor
long q1_simula(int N) {
    q1_contas = malloc(sizeof(conta_t) * (N + 1));
    q1_contas[0].id = 0;
    q1_contas[0].saldo = 0;                               // recebedor
    for (int i = 1; i <= N; i++) {                        // remetentes
        q1_contas[i].id = i;
        q1_contas[i].saldo = (long)q1_K * Q1_VALOR_PIX;
    }

    pthread_t *tid = malloc(sizeof(pthread_t) * N);
    int *ids = malloc(sizeof(int) * N);
    for (int i = 0; i < N; i++) {
        ids[i] = i + 1;
        pthread_create(&tid[i], NULL, q1_runner, &ids[i]);
    }
    for (int i = 0; i < N; i++)
        pthread_join(tid[i], NULL);

    long recebido = q1_contas[0].saldo;
    free(q1_contas);
    free(tid);
    free(ids);
    return recebido;
}

void q1_experimento(void) {
    int valores_N[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
    int qtd = sizeof(valores_N) / sizeof(valores_N[0]);

    printf("Experimento: cada remetente envia K=%d PIX de R$%d ao recebedor\n",
           q1_K, Q1_VALOR_PIX);
    printf("Protecao: %s | %d execucoes por N\n\n",
           q1_protegido ? "MUTEX" : "NENHUMA", Q1_REPETICOES);
    printf("%6s %12s %12s %12s %10s\n",
           "N", "esperado", "pior saldo", "R$ perdidos", "erros");
    printf("------------------------------------------------------------\n");

    for (int j = 0; j < qtd; j++) {
        int N = valores_N[j];
        long esperado = (long)N * q1_K * Q1_VALOR_PIX;
        long pior = esperado;
        int erros = 0;
        for (int r = 0; r < Q1_REPETICOES; r++) {
            long obtido = q1_simula(N);
            if (obtido != esperado) erros++;
            if (obtido < pior) pior = obtido;
        }
        printf("%6d %12ld %12ld %12ld %7d/%d%s\n", N, esperado, pior,
               esperado - pior, erros, Q1_REPETICOES,
               erros ? "  <-- INCONSISTENTE" : "");
    }
}

int questao1(int argc, char *argv[]) {
    pthread_mutex_init(&q1_mutex, NULL);

    printf("=================== QUESTAO 1: CONDICAO DE CORRIDA ===================\n\n");

    if (argc < 1) {                         // experimento completo
        q1_protegido = 0; q1_experimento();
        printf("\n");
        q1_protegido = 1; q1_experimento();
        return 0;
    }

    int N = atoi(argv[0]);
    if (argc > 1) q1_K = atoi(argv[1]);
    if (argc > 2) q1_protegido = atoi(argv[2]);
    if (N < 1) N = 1;

    long esperado = (long)N * q1_K * Q1_VALOR_PIX;
    long obtido = q1_simula(N);
    printf("N=%d K=%d protecao=%s\n", N, q1_K, q1_protegido ? "mutex" : "nenhuma");
    printf("Saldo esperado do recebedor: R$ %ld\n", esperado);
    printf("Saldo obtido do recebedor:   R$ %ld\n", obtido);
    printf("Dinheiro perdido:            R$ %ld\n", esperado - obtido);
    return 0;
}

/* ========================================================================== */
/*          QUESTOES 2 e 3 - MUTEX + FILA FCFS + DEBUG POR SINAIS             */
/* ========================================================================== */
/*
 * Q2: - Mutex protege a base de contas (debito + credito atomicos)
 *     - Requisicoes que superam o tamanho da thread pool ficam numa fila FIFO
 *       (head/tail) atendida por um escalonador FCFS
 *     - Fila = bounded-buffer com mutex + semaforo contador (sem busy waiting)
 * Q3: - Ctrl+C (SIGINT): o tratador acorda a thread de debug, que imprime o
 *       estado da pool e da fila. Ctrl+\ (SIGQUIT) encerra o programa.
 */

#define NUM_CONTAS 5
#define MAX_POOL   16

conta_t contas[NUM_CONTAS];
pthread_mutex_t mutex_bd;           // protege a base de dados (secao critica)

/* Requisicao PIX: faz o papel do PCB na fila de prontos */
typedef struct requisicao {
    int  id;                        // P1, P2, ... (id < 0 = sentinela)
    int  origem, destino;
    long valor;
    int  chegada_ms;                // instante de chegada
    int  duracao_ms;                // "burst": tempo de processamento simulado
    long inicio_ms, fim_ms;         // preenchidos ao executar
    int  thread;                    // thread da pool que atendeu
    struct requisicao *prox;
} requisicao_t;

/* Fila FCFS (FIFO head/tail) */
typedef struct {
    requisicao_t *head, *tail;
    int tamanho;                    // requisicoes reais aguardando
    pthread_mutex_t lock;           // protege a fila (secao critica)
    sem_t itens;                    // conta requisicoes disponiveis
} fila_t;

fila_t fila;
int TAM_POOL = 3;
requisicao_t *em_execucao[MAX_POOL];    // o que cada thread esta processando

sem_t sem_debug;                    // acordado pelo tratador de sinal
volatile sig_atomic_t encerrar = 0;

// Insere no fim da fila (tail) -> preserva a ordem de chegada
void enfileira(requisicao_t *r) {
    pthread_mutex_lock(&fila.lock);
    r->prox = NULL;
    if (fila.tail == NULL) fila.head = r;
    else fila.tail->prox = r;
    fila.tail = r;
    if (r->id > 0) fila.tamanho++;  // sentinelas nao contam
    pthread_mutex_unlock(&fila.lock);
    sem_post(&fila.itens);          // avisa a pool que ha trabalho
}

/* ----------------------------- Thread pool -------------------------------- */
void *worker(void *param) {
    int w = *(int *)param;
    while (1) {
        // Espera (bloqueada, sem busy waiting) ate haver requisicao
        while (sem_wait(&fila.itens) == -1 && errno == EINTR);

        // Escalonador FCFS: retira SEMPRE do inicio (head) da fila
        pthread_mutex_lock(&fila.lock);
        requisicao_t *r = fila.head;
        fila.head = r->prox;
        if (fila.head == NULL) fila.tail = NULL;
        if (r->id < 0) {            // sentinela: fim do trabalho
            pthread_mutex_unlock(&fila.lock);
            free(r);
            break;
        }
        fila.tamanho--;
        r->inicio_ms = agora_ms();
        r->thread = w;
        em_execucao[w] = r;
        pthread_mutex_unlock(&fila.lock);

        printf("[%6ld ms] T%d inicia  P%-2d (chegou %5d ms, esperou %5ld ms)\n",
               r->inicio_ms, w, r->id, r->chegada_ms,
               r->inicio_ms - r->chegada_ms);

        // Processamento simulado (validacao, antifraude, rede...)
        dormir_ms(r->duracao_ms);

        // Protecao minima: debito + credito atomicos na base compartilhada
        pthread_mutex_lock(&mutex_bd);
        /* ================= SECAO CRITICA ================= */
        int ok = contas[r->origem].saldo >= r->valor;
        if (ok) {
            contas[r->origem].saldo  -= r->valor;
            contas[r->destino].saldo += r->valor;
        }
        /* ================================================= */
        pthread_mutex_unlock(&mutex_bd);

        pthread_mutex_lock(&fila.lock);
        r->fim_ms = agora_ms();
        em_execucao[w] = NULL;
        pthread_mutex_unlock(&fila.lock);

        printf("[%6ld ms] T%d termina P%-2d  conta %d -> conta %d  R$ %ld %s\n",
               r->fim_ms, w, r->id, r->origem, r->destino, r->valor,
               ok ? "OK" : "RECUSADO (saldo insuficiente)");
    }
    pthread_exit(0);
}

/* ------------------------ Q3: debugging por sinais ------------------------ */
// O tratador apenas acorda a thread de debug:
// sem_post e async-signal-safe; printf NAO e.
void tratador_sigint(int sig) {
    (void)sig;
    sem_post(&sem_debug);
}

void *debugger(void *param) {
    (void)param;
    while (1) {
        while (sem_wait(&sem_debug) == -1 && errno == EINTR);
        if (encerrar) break;

        pthread_mutex_lock(&fila.lock);     // retrato consistente da fila
        long t = agora_ms();
        printf("\n================= DEBUG (Ctrl+C) t = %ld ms =================\n", t);
        printf("Thread pool (%d threads):\n", TAM_POOL);
        for (int w = 0; w < TAM_POOL; w++) {
            requisicao_t *r = em_execucao[w];
            if (r)
                printf("  T%d: RUNNING P%-2d  executou %5ld ms de %5d ms\n",
                       w, r->id, t - r->inicio_ms, r->duracao_ms);
            else
                printf("  T%d: livre\n", w);
        }
        printf("Fila FCFS (READY) - %d requisicao(oes) aguardando:\n", fila.tamanho);
        printf("  HEAD -> ");
        if (fila.tamanho == 0) printf("(vazia) -> ");
        for (requisicao_t *r = fila.head; r != NULL; r = r->prox)
            if (r->id > 0)
                printf("[P%d R$%ld espera %ldms] -> ", r->id, r->valor,
                       t - r->chegada_ms);
        printf("TAIL\n");
        pthread_mutex_lock(&mutex_bd);
        printf("Saldos:");
        for (int i = 0; i < NUM_CONTAS; i++)
            printf("  c%d=R$%ld", i, contas[i].saldo);
        pthread_mutex_unlock(&mutex_bd);
        printf("\n==============================================================\n\n");
        pthread_mutex_unlock(&fila.lock);
    }
    pthread_exit(0);
}

/* --------------------------- Caso de exemplo ------------------------------ */
// {origem, destino, valor, chegada_ms, duracao_ms}
int exemplo[][5] = {
    {1, 0, 100,    0, 3000},    // P1
    {2, 0,  50,    0, 2000},    // P2
    {3, 0,  70,  500, 1000},    // P3
    {4, 0,  30, 1000, 4000},    // P4  <- chega com a pool cheia
    {1, 2,  20, 1500, 1500},    // P5
    {2, 3,  10, 2000, 1000},    // P6
    {3, 4,  40, 2500, 2000},    // P7
    {4, 1,  25, 3000,  500},    // P8
};

int questao2_3(int argc, char *argv[]) {
    int M;
    requisicao_t **reqs;

    if (argc > 0) TAM_POOL = atoi(argv[0]);
    if (TAM_POOL < 1 || TAM_POOL > MAX_POOL) TAM_POOL = 3;

    // Base de dados inicial
    for (int i = 0; i < NUM_CONTAS; i++) {
        contas[i].id = i;
        contas[i].saldo = 1000;
    }
    long total_inicial = 1000L * NUM_CONTAS;

    // Monta as requisicoes (aleatorias ou caso de exemplo)
    if (argc > 1) {
        M = atoi(argv[1]);
        if (M < 1) M = 1;
        srand(time(NULL));
        reqs = malloc(sizeof(requisicao_t *) * M);
        int chegada = 0;
        for (int i = 0; i < M; i++) {
            requisicao_t *r = calloc(1, sizeof(requisicao_t));
            r->id = i + 1;
            r->origem = 1 + rand() % (NUM_CONTAS - 1);
            r->destino = 0;                         // N:1
            r->valor = 1 + rand() % 100;
            chegada += rand() % 400;
            r->chegada_ms = chegada;
            r->duracao_ms = 500 + rand() % 2500;
            reqs[i] = r;
        }
    } else {
        M = sizeof(exemplo) / sizeof(exemplo[0]);
        reqs = malloc(sizeof(requisicao_t *) * M);
        for (int i = 0; i < M; i++) {
            requisicao_t *r = calloc(1, sizeof(requisicao_t));
            r->id = i + 1;
            r->origem = exemplo[i][0];
            r->destino = exemplo[i][1];
            r->valor = exemplo[i][2];
            r->chegada_ms = exemplo[i][3];
            r->duracao_ms = exemplo[i][4];
            reqs[i] = r;
        }
    }

    // Sincronizacao
    pthread_mutex_init(&mutex_bd, NULL);
    pthread_mutex_init(&fila.lock, NULL);
    sem_init(&fila.itens, 0, 0);
    sem_init(&sem_debug, 0, 0);
    fila.head = fila.tail = NULL;
    fila.tamanho = 0;

    // Q3: associa o tratador ao sinal SIGINT (Ctrl+C)
    if (signal(SIGINT, tratador_sigint) == SIG_ERR)
        printf("Nao foi possivel capturar o sinal :(\n");

    printf("============ QUESTOES 2 e 3: MUTEX + FCFS + DEBUG POR SINAIS ============\n\n");
    printf("Sistema PIX | pool = %d threads | %d requisicoes | PID %d\n",
           TAM_POOL, M, getpid());
    printf("Pressione Ctrl+C para ver a fila (debug) | Ctrl+\\ para abortar\n\n");

    clock_gettime(CLOCK_MONOTONIC, &t0);

    pthread_t pool[MAX_POOL], tdebug;
    int ids[MAX_POOL];
    pthread_create(&tdebug, NULL, debugger, NULL);
    for (int w = 0; w < TAM_POOL; w++) {
        ids[w] = w;
        pthread_create(&pool[w], NULL, worker, &ids[w]);
    }

    // Servidor: recebe as requisicoes no instante de chegada e enfileira
    for (int i = 0; i < M; i++) {
        long espera = reqs[i]->chegada_ms - agora_ms();
        if (espera > 0) dormir_ms(espera);
        printf("[%6ld ms] chegou P%-2d (R$ %ld, burst %d ms)\n",
               agora_ms(), reqs[i]->id, reqs[i]->valor, reqs[i]->duracao_ms);
        enfileira(reqs[i]);
    }

    // Uma sentinela por thread: encerra a pool apos esvaziar a fila
    for (int w = 0; w < TAM_POOL; w++) {
        requisicao_t *s = calloc(1, sizeof(requisicao_t));
        s->id = -1;
        enfileira(s);
    }
    for (int w = 0; w < TAM_POOL; w++)
        pthread_join(pool[w], NULL);

    encerrar = 1;
    sem_post(&sem_debug);
    pthread_join(tdebug, NULL);

    // Relatorio do escalonamento FCFS
    printf("\n====================== RELATORIO FCFS ======================\n");
    printf("%4s %8s %8s %8s %8s %8s %7s %8s\n",
           "PIX", "chegada", "burst", "inicio", "fim", "espera", "thread", "turnar.");
    double soma_espera = 0, soma_turn = 0;
    for (int i = 0; i < M; i++) {
        requisicao_t *r = reqs[i];
        long espera = r->inicio_ms - r->chegada_ms;
        long turn = r->fim_ms - r->chegada_ms;
        soma_espera += espera;
        soma_turn += turn;
        printf("  P%-2d %8d %8d %8ld %8ld %8ld %6s%d %8ld\n", r->id, r->chegada_ms,
               r->duracao_ms, r->inicio_ms, r->fim_ms, espera, "T", r->thread, turn);
    }
    printf("Tempo medio de espera:     %.0f ms\n", soma_espera / M);
    printf("Turnaround medio:          %.0f ms\n", soma_turn / M);

    long total_final = 0;
    printf("Saldos finais:");
    for (int i = 0; i < NUM_CONTAS; i++) {
        printf("  c%d=R$%ld", i, contas[i].saldo);
        total_final += contas[i].saldo;
    }
    printf("\nDinheiro no sistema: inicial R$ %ld | final R$ %ld -> %s\n",
           total_inicial, total_final,
           total_inicial == total_final ? "CONSISTENTE" : "INCONSISTENTE");

    for (int i = 0; i < M; i++) free(reqs[i]);
    free(reqs);
    return 0;
}

/* ========================================================================== */
/*                                   MAIN                                     */
/* ========================================================================== */

void uso(const char *prog) {
    printf("Uso:\n");
    printf("  %s q1                 experimento Q1 (N = 1..512, sem e com mutex)\n", prog);
    printf("  %s q1 N K [0|1]       uma execucao Q1 (1 = com mutex)\n", prog);
    printf("  %s q2                 Q2/Q3 caso de exemplo (pool=3, 8 PIX)\n", prog);
    printf("  %s q2 T M             Q2/Q3 pool de T threads, M PIX aleatorios\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc >= 2) {
        if (strcmp(argv[1], "q1") == 0) return questao1(argc - 2, argv + 2);
        if (strcmp(argv[1], "q2") == 0 || strcmp(argv[1], "q3") == 0)
            return questao2_3(argc - 2, argv + 2);
        uso(argv[0]);
        return 1;
    }

    // Sem argumentos: menu interativo
    printf("INF0336 - A01 - Sincronismo do Sistema PIX\n\n");
    printf("  1) Questao 1   - condicao de corrida (experimento N:1)\n");
    printf("  2) Questoes 2/3 - mutex + fila FCFS + debug por Ctrl+C\n");
    printf("Escolha: ");
    fflush(stdout);

    int op = 0;
    if (scanf("%d", &op) != 1) op = 0;
    printf("\n");
    if (op == 1) return questao1(0, NULL);
    if (op == 2) return questao2_3(0, NULL);
    uso(argv[0]);
    return 1;
}
