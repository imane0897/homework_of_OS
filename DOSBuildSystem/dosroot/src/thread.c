// 一小份能跑出结果的代码

#include <alloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dos.h>

#define FINISHED      0   			// 表示线程处于终止态或TCB是空白状态
#define RUNNING       1   			// 表示线程处于运行态
#define READY         2   			// 表示线程处于就绪态
#define BLOCKED       3   			// 表示线程处于阻塞态

#define NTCB          10  			// 表示系统允许的最多任务数
#define NBUF          5             // 表示缓冲区数目
#define NTEXT         20            // 最大消息长度
#define TL            3             // 时间片长度，单位是1/18.2s

#define GET_INDOS     0x34
#define GET_CRIT_ERR  0x5d06

typedef int (far *codeptr)(void);   //定义了一个函数指针类型

typedef struct {
	int value;
	struct TCB *wq;
}semaphore;

struct TCB{
    unsigned char *stack;   		// 线程堆栈的起始地址
    unsigned ss;            		// 堆栈段址
    unsigned sp;            		// 堆栈指针
    int state;             			// 线程状态
    char name[10];          		// 线程的外部标识符
    struct buffer *mq;              // 接收线程的消息队列队首指针
    semaphore mutex;	        	// 接收线程的消息队列的互斥信号量
    semaphore sm;                   // 接收线程的消息队列的计数信号量，用于实现同步
	struct TCB *next;				// 链接字段。阻塞状态的线程按阻塞原因排成多个队列
} tcb[NTCB];               			// NTCB是系统允许的最多任务数

struct int_regs {
    unsigned bp, di, si, ds, es, dx, cx, bx, ax, ip, cs, flags, off, seg;
} ;

semaphore mutex = {1, NULL};	    // 空闲缓冲区队列的互斥信号量
semaphore mutexfb = {1, NULL};		// 对缓冲区进行操作的互斥信号量
semaphore sfb = {NBUF, NULL};	    // 空闲缓冲区队列的计数信号量
semaphore full = {0, NULL};

struct buffer {                     // 消息缓冲区的数据结构
	int sender;					    // 消息发送者的标识数
	int size;					    // 消息长度<=NTEXT个字节
	char text[NTEXT];			    // 消息正文
	struct buffer *next;		    // 指向下一个消息缓冲区的指针
} buf[NBUF];

int current;	                   // 记录正在运行的线程的内部标识符
int timecount;	                   // 记录当前线程自上次调度至今运行了多少时间，单位是1/18.2s，约等于55ms
char far *indos_ptr=0;	           // 该指针变量存放 INDOS 标志的地址
char far *crit_err_ptr=0;	       // 该指针变量存放严重错误标志的地址
struct buffer *freebuf;			   // 空闲消息缓冲队列，是临界资源，由NBUF个空闲的消息缓冲区组成

void InitInDos(void);
int DosBusy(void);
void InitTcb(void);
int destroy(int id);
void over();
int create(char *name,codeptr code,int stck);
void interrupt my_swtch(void);
void interrupt (*old_int8)(void);
void interrupt new_int8(void);
int all_threads_finished(void);
void tcb_state(void);
void p(semaphore *sem);
void block(struct TCB **qp);
void wakeup_first(struct TCB **qp);
void send(char *receiver, char *a, int size);
int receive(char *sender, char *b);
void sender();
void receiver();
int f(void);
void f1(void);
void f2(void);

void InitDos(void)
{
	union REGS regs;
	struct SREGS segregs;

	regs.h.ah = GET_INDOS;
	intdosx(&regs, &regs, &segregs);
	indos_ptr = MK_FP(segregs.es, regs.x.bx);

	if(_osmajor < 3)
		crit_err_ptr = indos_ptr + 1;
	else if(_osmajor == 3 && _osminor == 0)
		crit_err_ptr = indos_ptr - 1;
	else
	{
		regs.x.ax = GET_CRIT_ERR;
		intdosx(&regs, &regs, &segregs);
		crit_err_ptr = MK_FP(segregs.ds, regs.x.si);
	}
}

void InitInDos(void) {
	union REGS regs;
	struct SREGS segregs;

    regs.h.ah = GET_INDOS;                      // 获得 INDOS 标志的地址
    intdosx(&regs, &regs, &segregs);	        // Turbo C的库函数，其功能是调用DOS的INT21H中断
    indos_ptr = MK_FP(segregs.es, regs.x.bx);	// MK_FP()是一个宏，做段基址加上偏移地址的运算，即取实际地址

    if (_osmajor < 3) {					        // 获得严重错误标志的地址
    	crit_err_ptr = indos_ptr + 1;
	}
    else if (_osmajor == 3 && _osminor == 0) {
        crit_err_ptr = indos_ptr - 1;
	}
    else {
        regs.x.ax = GET_CRIT_ERR;
        intdosx(&regs, &regs, &segregs);
    	crit_err_ptr = MK_FP(segregs.ds, regs.x.si);
    }
}

int DosBusy(void) {
	if (indos_ptr && crit_err_ptr) {
		return(*indos_ptr || *crit_err_ptr);	// 返回值是1表示DOS忙，返回值是0表示DOS不忙
	}
	return -1;                                  // 返回值是-1表示还没有调用InitInDos()
}

void InitTcb(void) {
	int i;
	for (i = 0; i < NTCB; i++) {
		tcb[i].stack = NULL;
		tcb[i].ss = NULL;
		tcb[i].sp = NULL;
		tcb[i].stack = FINISHED;
        tcb[i].mq = NULL;
        tcb[i].mutex.value = 1;
        tcb[i].mutex.wq = NULL;
        tcb[i].sm.value = 0;
        tcb[i].sm.wq = NULL;
        tcb[i].next = NULL;
		strcpy(tcb[i].name, "\0");
	}
}

int destroy(int id) {
	disable();
	printf("\nDestroying thread %d\n", id);
	if (id < 0 | id >= NTCB) {
		printf("\nFailed to find thread %d.\n", id);
		return -1;
	}
	free(tcb[id].stack);
	tcb[id].ss = NULL;
	tcb[id].sp = NULL;
	tcb[id].state = FINISHED;
	printf("Destroyed thread %d successfully.\n", id);
	enable();
	return 0;
}

void over() {
	destroy(current);
	my_swtch();			//CPU重新进行调度
}

int create(char *name,codeptr code,int stck) {
    int i;
	struct int_regs * regs;
	printf("Creating thread: %s.\n", name);
    for (i = 0; i < NTCB; i++) {
        if (tcb[i].state == FINISHED) {
            tcb[i].stack = malloc(stck);					// 线程私有堆栈的始址
			regs = (struct int_regs *)(tcb[i].stack + stck);
			regs--;
            tcb[i].ss = FP_SEG(regs);				// 线程私有堆栈的段址
            tcb[i].sp = FP_OFF(regs);				// 线程私有堆栈的栈顶指针

            regs->ds = tcb[i].ss;							// 数据段地址
			regs->es = tcb[i].ss;							// 附加段地址
			regs->ip = FP_OFF(code);
			regs->cs = FP_SEG(code);
			regs->flags = 0x200;								// 允许中断的位置置为1
			regs->off = FP_OFF(over);						// 当前线程完场后自动执行over()撤销该线程
			regs->seg = FP_SEG(over);
            strcpy(tcb[i].name, name);
            tcb[i].state = READY;
			printf("Created thread %s successfully.\n", name);
            return i;
        }
    }
	printf("Failed to create thread: %s.\n", name);
    return -1;
}

// my_swtch()主要解决两种原因引起的调度：线程执行完毕或正在执行的线程因等待某事件发生而不能继续执行

void interrupt my_swtch(void) {				// 函数类型说明符interrupt将函数声明为中断处理函数
	int i;
	disable();								// 关中断
	tcb[current].ss = _SS;					// 保存当前线程current的现场
	tcb[current].sp = _SP;
	if (tcb[current].state == RUNNING) {	// 阻塞正在运行的线程；若线程已完成，则保持完成状态
		tcb[current].state = READY;
	}

	printf("Current thread is %d\n", current);
	i = current + 1;
	while (1) {							    // 找到一个新的就绪线程
		// printf("i is %d\n", i);
		if (tcb[i].state == READY) {
			break;
		} else {
			i++;
			i = i % NTCB;
		}
	}

	printf("Next thread is %d\n", i);
	_SS = tcb[i].ss;						// 恢复线程i的现场，并把CPU分配给它
	_SP = tcb[i].sp;
	tcb[i].state = RUNNING;
	current = i;							// 置线程i为当前线程
	timecount = 0;                          // 重新开始计时
	enable();								// 开中断
}

// new_int8()该函数主要解决因时间片到时引起的调度，通过截取时钟中断（int 08）来完成。

void interrupt new_int8(void) {
	(*old_int8)();
	timecount++;
	if (timecount >= TL) {
		if (!DosBusy()) {
			my_swtch();
		}
	}
}

int f(void){
	int i;
    for (i = 0; i < 100; i++) {
        printf("%d:\tHello World\n", i);
    }
    return 0;
}

int all_threads_finished(void) {	// 判断0#线程以外的线程是否全部结束
	int i;
	for (i = 1; i < NTCB; i++) {
		if (tcb[i].state != FINISHED) {
			return 0;
		}
	}
	return 1;
}

void tcb_state(void) {
	int i;
	for (i = 0; i < NTCB; i++) {
		switch(tcb[i].state) {
		case 0:
			printf("The state of tcb[%d](%s) is finished.\n", i, tcb[i].name);
			break;
		case 1:
			printf("The state of tcb[%d](%s) is running.\n", i, tcb[i].name);
			break;
		case 2:
			printf("The state of tcb[%d](%s) is ready.\n", i, tcb[i].name);
			break;
		case 3:
			printf("The state of tcb[%d](%s) is blocked.\n", i, tcb[i].name);
			break;
		}
	}
}

void p(semaphore *sem) {            // 对信号量进项P操作
	struct TCB **qp;			    // 指向TCB链表的二级指针
	disable();
	sem->value = sem->value - 1;
	if(sem->value<0){
		qp = &(sem->wq);	        // 将qp指针指向sem信号量的阻塞队列
		block(qp);
	}
	enable();
}

void v(semaphore *sem) {            // 对信号量进行V操作
	struct TCB **qp;
	disable();
	qp = &(sem->wq);
	sem->value = sem->value+1;
	if(sem->value <= 0) {
		wakeup_first(qp);
	}
	enable();
}

void block(struct TCB **qp) {       // 阻塞线程
	struct TCB *tp;
	disable();

	tp = *qp;
	tcb[current].state = BLOCKED;
	// 将线程插入到指定的阻塞队列未尾，并重新进行CPU调度
	(*qp)->next = NULL;
	if(tp == NULL) {
		tp = &tcb[current];
    }
	else{
		while(tp->next != NULL) {
			tp = tp->next;
        }
		tp->next = &tcb[current];
	}
	enable();
	my_swtch();
}

void wakeup_first(struct TCB **qp) {           // 唤醒线程
	int i;
	struct TCB *tp;
	disable();
	tp = *qp;

	for(i = 1; i < NTCB; i++) {
		if(strcmp(tcb[i].name,(*tp->next).name) == 0) {
			break;
		}
		tcb[i].state = READY;
		enable();
	}
}

void initBuf() {
	int i;
	for(i = 0; i < NBUF - 1; i++){
		buf[i].next = &buf[i+1];
	}
	buf[i].next = NULL;
	freebuf = &buf[0];
}

struct buffer *getbuf() {
	struct buffer *buff;
	buff = freebuf;						// 空闲消息缓冲头
	freebuf = freebuf->next;
	return buff;
}

void insert (struct buffer **mq, struct buffer *buff) {
	struct buffer *temp;
	if(buff == NULL) {
		return;
    }
	buff->next = NULL;
	if(*mq == NULL) {
		*mq = buff;
    } else {
		temp = *mq;
		while(temp->next != NULL) {
			temp = temp->next;
        }
		temp->next = buff;
	}
}

void send(char *receiver, char *a, int size) {
   struct buffer *buff;
   int i, id = -1;

	disable();
   for(i = 0; i < NTCB; i++) {
     if(strcmp(receiver,tcb[i].name) == 0) {
		 id = i;
		 break;
     }
   }

   if(id == -1) {                       // 找不到接收者
     printf("Error:Receiver not exist!\n");
	 enable();
	 return ;
   }

	p(&sfb);			                // sfb为空闲缓冲区队列的计数信号量
 	p(&mutexfb);		                // mutexfb为缓冲区的互斥信号量
	buff = getbuf();	                // 取一缓冲区
    v(&mutexfb);		                // 用完缓冲区,释放互斥信号量

    buff->sender = current;	            // 将发送方的内容加入缓冲区
    buff->size = size;
    buff->next = NULL;
    strcpy(buff->text, a);

    p(&tcb[id].mutex);				    // 互斥使用接收者线程的消息队列
	insert(&(tcb[id].mq), buff);	    // 将消息缓冲区插入消息队列
    v(&tcb[id].mutex);				    // 撤销线程id消息队列互斥信号，接收者线程多了个消息

	v(&tcb[id].sm);					    // 消息队列计数信号量加1
	enable();
}

struct buffer *remov(struct buffer **mq, int sender) {
	struct buffer *buff, *p, *q;
	q = NULL;
	p = *mq;

	while((p->sender != sender) && (p->next != NULL)) {
		q = p;
		p = p->next;
	}
	if(p->sender == sender) {
		buff = p;
		if(q == NULL) {
			*mq = buff->next;
        } else {
			q->next = buff->next;
        }
		buff->next = NULL;
		return buff;
	} else {
		return NULL;
    }
}

int receive(char *sender, char *b) {
	int i, id = -1;
	struct buffer *buff;

	disable();
	for(i = 0; i < NBUF; i++) {
		if(strcmp(sender, tcb[i].name) == 0) {
			id = i;
			break;
		}
	}

	if(id == -1) {
		enable();
		return -1;
	}

	p(&tcb[current].sm);

	p(&tcb[current].mutex);
	buff = remov(&(tcb[current].mq), id);
	v(&tcb[current].mutex);

	if(buff == NULL) {
		v(&tcb[current].sm);
		enable();
		return -1;
	}
	strcpy(b, buff->text);								// b是接收线程的内部接收区

	p(&mutexfb);
	insert(&freebuf, buff);
    v(&mutexfb);

	v(&sfb);											// 空闲缓冲区队列的计数信号量+1
	enable();
	return buff->size;
}
semaphore signal1 = {0, NULL};
semaphore signal2 = {0, NULL};
void sender(void)
{
	int i, j, n = 0;
	char a[10];
	// v(&signal1);
	for(i = 0; i < 5; i++) {
		strcpy(a, "message");
		a[7] = '0' + n;
		a[8] = 0;
		send("receiver", a, strlen(a));
		printf("sender: Message \"%s\"  has been sent\n", a);
		n++;
	}
	// p(&signal2);
	receive("receiver",a);
	if (strcmp(a,"ok") != 0) {
		printf("Not be committed,Message should be resended.\n");
	} else {
		printf("Committed,Communication is finished!\n");
	}
}

void receiver(void) {
	int i, j, size;
	char b[10];
	// p(&signal1);
	for(i = 0; i < 5; i++) {
		b[0] = 0;
		while((size = receive("sender", b)) == -1);
		printf("receiver: Message is received: \t");
		for(j = 0; j < size; j++) {
			putchar(b[j]);
		}
		putchar('\n');
	}
	// v(&signal2);
	strcpy(b, "ok");
	send("sender", b, 3);
}

void f1(void) {
	int i, j, k;
    for(i = 0; i < 1000; i++) {
        putchar('a');
		delay(1);
    }
}

void f2(void) {
	int i, j, k;
    for(i = 0; i < 1000; i++) {
        putchar('b');
        delay(1);
    }
}

int main() {
	// InitInDos();
	InitDos();
	InitTcb();
	old_int8 = getvect(8);						// 获取并保存系统原来的 INT 08H 的中断服务程序的入口地址

	// 创建0#线程
    strcpy(tcb[0].name, "main");
	tcb[0].ss = _SS;
	tcb[0].sp = _SP;
	tcb[0].state = RUNNING;

    current = 0;

	clrscr();
	initBuf();
	create("sender",(codeptr)sender, 1024);
	create("receiver",(codeptr)receiver, 1024);
	printf("sending\n");
	getch();
    // create("f1",(codeptr)f1,1024);
    // create("f2",(codeptr)f2,1024);
    // tcb_state();

    // 为时钟中断设置新的中断服务程序，启动多个线程的并发执行
    setvect(8, new_int8);
	// putchar('x');
    while(!all_threads_finished());
    // 终止多任务系统
    tcb[0].state = FINISHED;
    setvect(8, old_int8);

    // tcb_state();
    printf("\nMulti_task system terminated.\n");
	return 0;
}
