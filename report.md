# 基于 DOS 的多任务系统的实现

## 概要设计

1. 线程的描述
2. 线程的创建和撤销
3. 线程调度
4. 线程的阻塞和唤醒
5. 线程的同步和互斥
6. 线程间的通信

## 主要数据结构

### 线程控制块TCB

用于描述线程，初始时定义一定数量的TCB并预分配空间。TCB的内部标识符即为tcb数组下标，使用*next是因为线程的等待队列的数据结构是链式的。

```c
struct TCB {
    unsigned char *stack;	// 线程堆栈的起始地址
    unsigned ss;			// 堆栈段址
    unsigned sp;			// 堆栈指针
    char state;				// 线程状态
    char name[NTEXT];		// 线程的外部标识符
    struct buffer *mq;		// 接收线程的消息队列的队首指针的指针
    semaphore mutex;		// 对消息缓冲区进行操作的互斥信号量
    semaphore sm;			// 接收线程的消息队列的计数信号量
    struct TCB *next;		// 指向下一个TCB
} tcb[NTCB];
```



### 消息缓冲区

初始化时设置NBUF个空闲的消息缓冲区，并插入到空闲缓冲区队列 freebuf 中。freebuf 是临界资源，申请缓冲区和归还缓冲区的操作都需要互斥进行，“申请”和“归还”操作类似“生产者与消费者”模型，因此设置了一个互斥信号量和一个计数信号量。

```c
struct buffer {
    int id;					// 消息发送者的内部标识
    int size;				// 消息长度
    char text[NTEXT];		// 消息内容
    struct buffer *next;	// 指向下一个消息缓冲区
} buf[NBUF], *freebuf;
```

### 信号量

通过对信号量的P操作和V操作来解决同步与互斥的问题。

```c
typedef struct {
    int value;
    struct TCB *wq;
} semaphore;
```



## 详细设计

1. 发送消息

   buffer 是临界资源，获取一个空的 buffer 和将空 buffer 插入到 空闲缓冲区队列 freebuf 时需要互斥地进行。

   ```c
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
   ```

   ​

2. 接收消息

   接受线程试图从buffer中获取消息时，如果返回值为空，说明消息还没有到，需要先将当前线程设为阻塞状态，再重新进行线程调度，直到收到消息。

   ```c
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
   ```

   ​

## 测试结果

### 互斥

n 是初始值为0的全局变量，线程f4和f5同时争夺n，f4对n做10次减操作，f5对n做5次加操作。

```c
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
```



![屏幕快照 2017-01-02 下午7.29.33](/Users/AnYameng/Desktop/屏幕快照 2017-01-02 下午7.29.33.png)

### 生产者与消费者

 生产者线程prdc向buffer里面添加内容，消费者线程cnsm从buffer里取出内容。buffer的数量是有限的，并且对buffer进行操作需要互斥地进行。

```c
void prdc() {
    int tmp, i, in = 0;
    for (i = 1; i <= 10; i++) {
        tmp = i * i;
        wait(&empty);
        wait(&mutex);
        intbuf[in] = tmp;
        in = (in + 1) % NBUF;

        printf("In %d\n", tmp);
        signal(&mutex);
        signal(&full);
    }
}

void cnsm() {
    int tmp, i, out = 0;
    for (i = 1; i <= 10; i++) {
        wait(&full);
        wait(&mutex);
        tmp = intbuf[out];
        out = (out + 1) % NBUF;

        signal(&mutex);
        signal(&empty);
        printf("Out %d %d\n", i, tmp);
    }
}
```



![屏幕快照 2017-01-05 下午6.41.01](/Users/AnYameng/Desktop/屏幕快照 2017-01-05 下午6.41.01.png)

### 线程通信

线程sender向线程receiver发送消息，receiver在收到消息后向sender发送一条确认消息。

```c
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

```



![屏幕快照 2017-01-02 下午7.27.03](/Users/AnYameng/Desktop/屏幕快照 2017-01-02 下午7.27.03.png)

