#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>

#define BUF_LEN		0x80
int write_consume_time=300000;

#define min(x,y) ({ 			\
		typeof(x) _x = (x);	\
		typeof(y) _y = (y);	\
		(void) (&_x == &_y);	\
		_x < _y ? _x : _y; })

static uint32_t fill_data = 1;

static pthread_mutex_t  read_mutex_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t read_condition = PTHREAD_COND_INITIALIZER;


struct buff_str{
	char 	*addr;
	struct buff_str *next_buf_addr;
	uint32_t read_enable;
	uint32_t length;
	uint32_t offset;
	uint32_t index;
};

struct pingpong_buffer {

	struct buff_str buf1;
	struct buff_str buf2;
	struct buff_str buf3;
	struct buff_str *current_read;
	int read_switch;

	struct buff_str *current_write;
	int write_switch;
};

void print_mem(void * addr, uint32_t count, uint32_t size)
{
	int i;
	uint8_t * addr8 = addr;
	uint16_t * addr16 = addr;
	uint32_t * addr32 = addr;
	uint32_t g_paddr;
	
	g_paddr = (uint32_t)addr;

	switch (size)
	{
	case 1:
		for (i = 0; i < count; i++) {
			if ( (i % 16) == 0 )
				printf("\n0x%08X: ", g_paddr);
			printf(" %02X", addr8[i]);
			g_paddr++;
		}
		break;
	case 2:
		for (i = 0; i < count; i++) {
			if ( (i % 8) == 0 )
				printf("\n0x%08X: ", g_paddr);
			printf(" %04X", addr16[i]);
			g_paddr += 2;
		}
		break;
	case 4:
		for (i = 0; i < count; i++) {
			if ( (i % 4) == 0 )
				printf("\n0x%08X: ", g_paddr);
			printf(" %08X", addr32[i]);
			g_paddr += 4;
		}
		break;
	default:
		printf("error value!\n");
		break;
	}
	printf("\n\n");
}

/* return valule: bytes have read, or goto pthread_cond_wait if not read enable */ 
uint32_t read_from_pingpong_buf(struct pingpong_buffer *pp_buf, const uint32_t count)
{
	uint32_t ret = 0;
	uint32_t len;
	
	struct buff_str *buf;

	if (pp_buf->read_switch) {
		pp_buf->current_read = pp_buf->current_read->next_buf_addr;		
		pp_buf->read_switch = 0;
	}

	buf = pp_buf->current_read;

	pthread_mutex_lock(&read_mutex_lock);
	
	while(!buf->read_enable) {
		printf("read wait by %d......\n", buf->index);
		pthread_cond_wait(&read_condition, &read_mutex_lock);
	}
	pthread_mutex_unlock(&read_mutex_lock);

	len = min(count, buf->length - buf->offset);
	printf("\n******************** read from :%d************************", buf->index);

	/* instead read() for test */
	print_mem(buf->addr, len, 1);

	ret = len;
	buf->offset += ret;
	
	pthread_mutex_lock(&read_mutex_lock);
	/* get to the buffer end, switch to next buffer */
	if(buf->offset == buf->length) { //当offset到达length处时该可读buff已经读完
		buf->read_enable = 0;
		buf->offset = 0;
		pp_buf->read_switch = 1;
	}
	pthread_mutex_unlock(&read_mutex_lock);

	return ret;
}


uint32_t write_to_pingpong_buf(struct pingpong_buffer *pp_buf, const uint32_t count)
{
	struct buff_str *buf;

	if (pp_buf->write_switch) {		
		pp_buf->current_write = pp_buf->current_write->next_buf_addr;		
		pp_buf->write_switch = 0;
	}

	buf = pp_buf->current_write;
	printf("\n******************** write to :%d************************\n", buf->index);
	/* use memset instead write operations for test */
	memset(buf->addr, fill_data, BUF_LEN);
	fill_data++;
	

	/* when write finished, enable write */
	pthread_mutex_lock(&read_mutex_lock);
	buf->read_enable = 1;
	pp_buf->write_switch = 1;
	pthread_mutex_unlock(&read_mutex_lock);
	pthread_cond_broadcast(&read_condition);	//唤醒阻塞在读等待队列的读线程,要用广播不然部分读线程不能出来

	return BUF_LEN;
}

void * pingpong_read(void *arg)
{
	uint32_t ret;
	struct pingpong_buffer *pp_buf; 
	int inc = 0, time=0;

	pp_buf = (struct pingpong_buffer *)arg;
	pp_buf->current_read->index = 1;
	while (1) {
		read_from_pingpong_buf(pp_buf, BUF_LEN);
		//模拟一个时间动态改变的耗时操作--小于或等于缓存个数倍write_consume_time,不然会丢帧.
		usleep(write_consume_time + inc -write_consume_time/2);
		inc =0;
		if(++time %10 == 0)
			inc = 3*write_consume_time +write_consume_time/2;
	}
}

void * pingpong_write(void *arg)
{
	uint32_t ret;
	struct pingpong_buffer *pp_buf; 

	pp_buf = (struct pingpong_buffer *)arg;
	pp_buf->current_write->index = 1;

	while (1) {
		write_to_pingpong_buf(pp_buf, BUF_LEN);		
		usleep(write_consume_time);

	}
}

int pingpong_init(struct pingpong_buffer *pp_buf)
{
	char *mem1, *mem2, *mem3;
	struct buff_str *buf1, *buf2, *buf3;

	buf1 = &pp_buf->buf1;
	buf2 = &pp_buf->buf2;
	buf3 = &pp_buf->buf3;

	mem1 = malloc(BUF_LEN);
	if(mem1 == NULL) {
		perror("pingpong_init()");
		return -ENOMEM;
	}

	mem2 = malloc(BUF_LEN);
	if(mem2 == NULL) {
		perror("pingpong_init()");
		free(mem1);
		return -ENOMEM;
	}
	mem3 = malloc(BUF_LEN);
	if(mem3 == NULL) {
		perror("pingpong_init()");
		free(mem2);
		return -ENOMEM;
	}

	buf1->addr = mem1;
	buf1->next_buf_addr = buf2;
	buf1->read_enable = 0;
	buf1->length = BUF_LEN;
	buf1->offset = 0;
	buf1->index = 1;


	buf2->addr = mem2;
	buf2->next_buf_addr = buf3;
	buf2->read_enable = 0;
	buf2->length = BUF_LEN;
	buf2->offset = 0;
	buf2->index = 2;

	buf3->addr = mem3;
	buf3->next_buf_addr = buf1;
	buf3->read_enable = 0;
	buf3->length = BUF_LEN;
	buf3->offset = 0;
	buf3->index = 3;

	pp_buf->current_read = buf1;
	pp_buf->current_read->index = 1;
	pp_buf->read_switch = 0;

	pp_buf->current_write = buf1;
	pp_buf->current_write->index = 1;

	pp_buf->write_switch = 0;

	return 0;
}

void pingpong_free(struct pingpong_buffer *pp_buf)
{
	if (pp_buf->buf1.addr) {
		free(pp_buf->buf1.addr);
		pp_buf->buf1.addr = NULL;
		pp_buf->buf1.next_buf_addr = NULL;
	}

	if (pp_buf->buf2.addr) {
		free(pp_buf->buf2.addr);
		pp_buf->buf2.addr = NULL;
		pp_buf->buf2.next_buf_addr = NULL;
	}

	if (pp_buf->buf3.addr) {
		free(pp_buf->buf3.addr);
		pp_buf->buf3.addr = NULL;
		pp_buf->buf3.next_buf_addr = NULL;
	}
}

int main()
{
	int err;
	pthread_t read_pid, write_pid;
	struct pingpong_buffer pp_buf;

	err = pingpong_init(&pp_buf);
	if (err < 0) {
		exit(1);
	}
	printf("init ok\n");
	err = pthread_create(&read_pid, NULL, pingpong_read, &pp_buf);
	if (err) { 
		perror("pthread_create()");
		pingpong_free(&pp_buf);
		exit(1);
	}
	err = pthread_create(&write_pid, NULL, pingpong_write, &pp_buf);
	if (err) {
		perror("pthread_create()");
		pthread_cancel(read_pid);
		pingpong_free(&pp_buf);
		exit(1);
	}

	pthread_join(read_pid, NULL);
	pthread_join(write_pid, NULL);

	pingpong_free(&pp_buf);

	exit(0);
}
