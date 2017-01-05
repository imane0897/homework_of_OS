#include <alloc.h>
#include <dos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FINISHED 0
#define RUNNING 1
#define READY 2
#define BLOCKED 3

#define NTCB 10
#define NTEXT 20
#define NBUF 200
#define NSTACK 1024

#define GET_INDOS 0x34
#define GET_CRIT_ERR 0x5d06

char far *indos_ptr = 0;
char far *crit_err_ptr = 0;

int timecount = 0;
int TL;
int current_pid = -1;
int n = 0;
int swtchtcb = 1; // used as semaphore to decide swtch

typedef int(far *codeptr)(void);
void interrupt (*old_int8)(void);

typedef struct {
    int value;
    struct TCB *wq;
} semaphore;

semaphore mutex = {1, NULL};
semaphore mutexfb = {1, NULL}; // fetch free buffer
semaphore sfb = {NBUF, NULL};  // semaphore free buffer
semaphore empty = {NBUF, NULL};
semaphore full = {0, NULL};

struct buffer {
    int id;
    int size;
    char text[NTEXT];
    struct buffer *next;
} buf[NBUF], *freebuf;

struct TCB {
    unsigned char *stack;
    unsigned ss;
    unsigned sp;
    char state;
    char name[NTEXT];
    struct buffer *mq;
    semaphore mutex;
    semaphore sm;
    struct TCB *next;
} tcb[NTCB];

struct int_regs {
    unsigned bp, di, si, ds, es, dx, cx, bx, ax, ip, cs, flags, off, seg;
};

int intbuf[NBUF], buftemp;

void over();
void destroy(int id);
void wait(semaphore *sem);
void signal(semaphore *sem);
void block(struct TCB **qp);
void wakeupFirst(struct TCB **qp);
void send(char *receiver, char *a, int size);
int receive(char *sender, char *b);

void InitDos(void) {
    union REGS regs;
    struct SREGS segregs;

    regs.h.ah = GET_INDOS;

    intdosx(&regs, &regs, &segregs);

    indos_ptr = MK_FP(segregs.es, regs.x.bx);

    if (_osmajor < 3)
        crit_err_ptr = indos_ptr + 1;
    else if (_osmajor == 3 && _osminor == 0)
        crit_err_ptr = indos_ptr - 1;
    else {
        regs.x.ax = GET_CRIT_ERR;
        intdosx(&regs, &regs, &segregs);
        crit_err_ptr = MK_FP(segregs.ds, regs.x.si);
    }
}

int DosBusy(void) {
    if (indos_ptr && crit_err_ptr)
        return (*indos_ptr || *crit_err_ptr);
    else
        return -1;
}

void initTCB() {
    int i;
    for (i = 0; i < NTCB; i++) {
        tcb[i].name[0] = '\0';
        tcb[i].stack = NULL;
        tcb[i].state = FINISHED;
        tcb[i].mq = NULL;
        tcb[i].mutex.value = 1;
        tcb[i].mutex.wq = NULL;
        tcb[i].sm.value = 0;
        tcb[i].sm.wq = NULL;
        tcb[i].next = NULL;
    }
}

void f1(void) {
    long i, j, k;
    for (i = 0; i < 1000; i++) {
        putchar('a');
        for (j = 0; j < 1000; j++)
            for (k = 0; k < 100; k++)
                ;
    }
}

void f2(void) {
    long i, j, k;
    for (i = 0; i < 100; i++) {
        putchar('b');
        for (j = 0; j < 1000; j++)
            for (k = 0; k < 100; k++)
                ;
    }
}

void f3(void) {
    long i, j, k;
    for (i = 0; i < 1000; i++) {
        putchar('c');
        for (j = 0; j < 1000; j++)
            for (k = 0; k < 100; k++)
                ;
    }
}

void f4() {
    int i;
    for (i = 0; i < 10; i++) {
        wait(&mutex);
        n++;
        printf(" %d", n);
        signal(&mutex);
    }
}

void f5() {
    int i;
    for (i = 0; i < 5; i++) {
        wait(&mutex);
        n--;
        printf(" %d ", n);
        signal(&mutex);
    }
}

void prdc() {
    int tmp, i, in = 0;
    for (i = 1; i <= 100; i++) {
        tmp = i * i;
        wait(&empty);
        wait(&mutex);
        intbuf[in] = tmp;
        in = (in + 1) % NBUF;

        printf("In %d\n", tmp);
        signal(&mutex);
        signal(&full);
        sleep(1);
    }
}

void cnsm() {
    int tmp, i, out = 0;
    for (i = 1; i <= 100; i++) {
        wait(&full);
        wait(&mutex);
        tmp = intbuf[out];
        out = (out + 1) % NBUF;

        signal(&mutex);
        signal(&empty);
        printf("Out %d %d\n", i, tmp);
        sleep(1);
    }
}

void sender(void) {
    int i, j;
    char a[10];
    for (i = 0; i < 10; i++) {
        strcpy(a, "message");
        a[7] = '0' + i;
        a[8] = '\0';
        send("receiver", a, strlen(a));
        printf("sender:Message \"%s\"  has been sent\n", a);
    }
    receive("receiver", a);
    if (strcmp(a, "ok") == 0) {
        printf("Sender heard \"ok\" from receiver.\n");
    } else
        printf("Something bad happened.\n");
}

void receiver(void) {
    int i, j, size;
    char b[10];
    for (i = 0; i < 10; i++) {
        b[0] = 0;
        while ((size = receive("sender", b)) == -1)
            ;
        printf("receiver: Message is received--");
        for (j = 0; j < size; j++)
            putchar(b[j]);
        printf("\n");
    }
    printf("Receiver tells sender \"ok\"\n");
    send("sender", "ok", 3);
}

int create(char *name, codeptr code, int stck) {
    struct int_regs far *r;
    int i, id = -1;

    for (i = 0; i < NTCB; i++) {
        if (tcb[i].state == FINISHED) {
            id = i;
            break;
        }
    }

    if (id == -1)
        return -1;
    disable();

    tcb[id].stack = (unsigned char *)malloc(stck);
    r = (struct int_regs *)(tcb[id].stack + stck);
    r--;
    tcb[id].ss = FP_SEG(r);
    tcb[id].sp = FP_OFF(r);
    r->cs = FP_SEG(code);
    r->ip = FP_OFF(code);
    r->es = _DS;
    r->ds = _DS;
    r->flags = 0x200;
    r->seg = FP_SEG(over);
    r->off = FP_OFF(over);
    tcb[id].state = READY;
    strcpy(tcb[id].name, name);
    enable();
    return 0;
}

void interrupt swtch() {
    int loop = 0;
    disable();

    if (swtchtcb <= 0) {
        enable();
        return;
    }

    tcb[current_pid].ss = _SS;
    tcb[current_pid].sp = _SP;

    if (tcb[current_pid].state == RUNNING)
        tcb[current_pid].state = READY;

    while (tcb[++current_pid].state != READY && loop++ < NTCB - 1)
        if (current_pid == NTCB)
            current_pid = 0;

    if (tcb[current_pid].state != READY)
        current_pid = 0;
    _SS = tcb[current_pid].ss;
    _SP = tcb[current_pid].sp;

    tcb[current_pid].state = RUNNING;

    timecount = 0;

    enable();
}

void destroy(int id) {
    disable();
    free(tcb[id].stack);
    tcb[id].stack = NULL;
    tcb[id].state = FINISHED;
    printf("\nProcess %s terminated\n", tcb[id].name);
    enable();
}

void over() {
    destroy(current_pid);
    swtch();
}

int finished() {
    int i;
    for (i = 1; i < NTCB; i++)
        if (tcb[i].state != FINISHED)
            return 0;
    return 1;
}

void free_all(void) {
    int i;

    for (i = 0; i < NTCB; i++) {
        if (tcb[i].stack) {
            tcb[i].name[0] = '\0';
            tcb[i].state = FINISHED;
            free(tcb[i].stack);
            tcb[i].stack = NULL;
        }
    }
}

void interrupt new_int8() {
    (*old_int8)();
    timecount++;
    if (timecount < TL)
        return;
    if (DosBusy())
        return;
    swtch();
}

void wait(semaphore *sem) {
    struct TCB **qp;
    disable();
    sem->value--;
    if (sem->value < 0) {
        qp = &(sem->wq);
        block(qp);
        enable();
        swtch();
    } else {
        enable();
    }
}

void signal(semaphore *sem) {
    struct TCB **qp;
    disable();
    qp = &(sem->wq);
    sem->value++;
    if (sem->value <= 0) {
        wakeupFirst(qp);
    }
    enable();
}

void block(struct TCB **qp) {
    int id;
    struct TCB *tcbtmp;

    id = current_pid;
    tcb[id].state = BLOCKED;

    if ((*qp) == NULL) {
        (*qp) = &tcb[id];
    } else {
        tcbtmp = *qp;
        while (tcbtmp->next != NULL)
            tcbtmp = tcbtmp->next;
        tcbtmp->next = &tcb[id];
    }

    tcb[id].next = NULL;
}

void wakeupFirst(struct TCB **qp) {
    struct TCB *tcbtmp;

    if ((*qp) == NULL)
        return;

    tcbtmp = *qp;
    *qp = (*qp)->next;
    tcbtmp->state = READY;
    tcbtmp->next = NULL;
}

void initBuf() {
    int i;
    for (i = 0; i < NBUF - 1; i++) {
        buf[i].next = &buf[i + 1];
    }
    buf[i].next = NULL;
    freebuf = &buf[0];
}

struct buffer *getbuf() {
    struct buffer *buff;
    buff = freebuf;
    freebuf = freebuf->next;
    return buff;
}

void insert(struct buffer **mq, struct buffer *buff) {
    struct buffer *temp;
    if (buff == NULL) {
        return;
    }
    buff->next = NULL;
    if (*mq == NULL) {
        *mq = buff;
    } else {
        temp = *mq;
        while (temp->next != NULL)
            temp = temp->next;
        temp->next = buff;
    }
}

void send(char *receiver, char *a, int size) {
    struct buffer *buff;
    int i, id = -1;

    for (i = 0; i < NTCB; i++) {
        if (strcmp(receiver, tcb[i].name) == 0) {
            id = i;
            break;
        }
    }
    if (id == -1) {
        printf("Error: Receiver does not exist.\n");
        swtchtcb++;
        return;
    }
    wait(&sfb);
    wait(&mutexfb);
    buff = getbuf();
    signal(&mutexfb);

    buff->id = current_pid;
    buff->size = size;
    buff->next = NULL;
    strcpy(buff->text, a);

    wait(&tcb[id].mutex);
    insert(&tcb[id].mq, buff);
    signal(&tcb[id].mutex);

    signal(&tcb[id].sm);

    if (tcb[id].state == BLOCKED) {
        tcb[id].state = READY;
    }
}

struct buffer *remov(struct buffer **mq, int sender) {
    struct buffer *buff, *p, *q;
    q = NULL;
    p = *mq;
    while ((p->next != NULL) && (p->id != sender)) {
        q = p;
        p = p->next;
    }
    if (p->id == sender) {
        buff = p;
        if (q == NULL)
            *mq = buff->next;
        else
            q->next = buff->next;
        buff->next = NULL;
        return buff;
    } else
        return NULL;
}

int receive(char *sender, char *b) {
    int i, id = -1;
    struct buffer *buff;

    for (i = 0; i < NTCB; i++) {
        if (strcmp(sender, tcb[i].name) == 0) {
            id = i;
            break;
        }
    }
    if (id == -1) {
        printf("Error: Sender does not exist.\n");
        swtchtcb++;
        return -1;
    }
again:
    wait(&tcb[current_pid].sm);
    wait(&tcb[current_pid].mutex);
    buff = remov(&(tcb[current_pid].mq), id);
    signal(&tcb[current_pid].mutex);
    if (buff == NULL) {
        signal(&tcb[current_pid].sm);
        tcb[current_pid].state = BLOCKED;
        swtchtcb++;
        swtch();
        goto again;
    }
    strcpy(b, buff->text);
    wait(&mutexfb);
    insert(&freebuf, buff);
    signal(&mutexfb);
    signal(&sfb);

    return buff->size;
}

void main() {
    int select = -1;

    InitDos();
    initTCB();

    old_int8 = getvect(8);
    strcpy(tcb[0].name, "main");
    tcb[0].state = RUNNING;
    current_pid = 0;

    while (select != 0) {
        do {
            clrscr();
            printf("0. Exit\n");
            printf("1. Exclusively assess\n");
            printf("2. Producer and consumer SYNC problem\n");
            printf("3. Message buffer communication\n");
            scanf("%d", &select);
        } while (select < 0 || select > 7);

        switch (select) {

        case 1:
            n = 0;
            TL = 1;
            create("f4", (codeptr)f4, NSTACK);
            create("f5", (codeptr)f5, NSTACK);
            printf("\ncreate f4, f5\n");
            printf("f4 increase n 1 each time\n");
            printf("f5 decrease n 1 each time\n");
            setvect(8, new_int8);
            swtch();
            getch();
            break;
        case 2:
            TL = 1;
            create("prdc", (codeptr)prdc, NSTACK);
            create("cnsm", (codeptr)cnsm, NSTACK);
            printf("prdc\n");
            printf("cnsm\n");
            getch();
            setvect(8, new_int8);
            swtch();
            getch();
            break;
        case 3:
            initBuf();
            create("sender", (codeptr)sender, NSTACK);
            create("receiver", (codeptr)receiver, NSTACK);
            printf("sending\n");
            getch();
            TL = 1;
            setvect(8, new_int8);
            swtch();
            getch();
            break;
        default:
            select = 0;
        }
        while (!finished())
            ;
        setvect(8, old_int8);
    }

    free_all();

    tcb[0].name[0] = '\0';
    tcb[0].state = FINISHED;

    printf("\nMulti task system terminated.\n");
    getch();
}
