/*
 * Copyright 2011-2013 Alibaba Group Holding Limited. All rights reserved.
 * Use and distribution licensed under the GPL license.
 *
 * Authors: XiaoJinliang <xiaoshi.xjl@taobao.com>
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <genpool.h>
#include <log.h>
#include <list.h>
#include <timer.h>
#include "conn_pool.h"
#include "my_pool.h"
#include "cli_pool.h"
#include "my_buf.h"
#include "mysql_com.h"
#include "my_conf.h"

extern log_t *g_log;
extern struct conf_t g_conf;

static genpool_handler_t *conn_pool;
static uint32_t connid;

static int conn_init(conn_t *c);
static conn_t *conn_alloc(void);
static int conn_release(conn_t *c);

static int read_client_timeout_timer(unsigned long arg);
static int write_mysql_timeout_timer(unsigned long arg);
static int read_mysql_write_client_timeout_timer(unsigned long arg);
static int prepare_mysql_timeout_timer(unsigned long arg);
static int idle_timeout_timer(unsigned long arg);

static struct list_head read_client_head;
static struct list_head write_mysql_head;
static struct list_head read_mysql_write_client_head;
static struct list_head prepare_mysql_head;
static struct list_head idle_head;//接到一个客户端 连接后，将其放到这里

/*
 * fun: init connection pool and timer
 * arg: max connection number
 * ret: success 0, error -1
 *
 */

int conn_pool_init(size_t count)
{
    int res = 0;
    pid_t pid;

    pid = getpid();

	//申请一个给连接用的内存池
    conn_pool = genpool_init(sizeof(conn_t), count);
    if(conn_pool == NULL){
        log(g_log, "genpool init error\n");
        return -1;
    }

    INIT_LIST_HEAD(&read_client_head);
    INIT_LIST_HEAD(&write_mysql_head);
    INIT_LIST_HEAD(&read_mysql_write_client_head);
    INIT_LIST_HEAD(&prepare_mysql_head);
    INIT_LIST_HEAD(&idle_head);

    srand(pid * time(NULL));
    connid = rand();
//注册各种事件的超时函数
    if( (res = timer_register(read_client_timeout_timer, 30, \
                                "read_client_timeout_timer", 1) < 0) ){
        log(g_log, "read_client_timeout_timer register error\n");
        return res;
    }

    if( (res = timer_register(write_mysql_timeout_timer, 30, \
                                "write_mysql_timeout_timer", 1) < 0) ){
        log(g_log, "write_mysql_timeout_timer register error\n");
        return res;
    }

    if( (res = timer_register(read_mysql_write_client_timeout_timer, 30, \
                        "read_mysql_write_client_timeout_timer", 1) < 0) ){
        log(g_log, "read_mysql_write_client_timeout_timer register error\n");
        return res;
    }

    if( (res = timer_register(prepare_mysql_timeout_timer, 30, \
                        "prepare_mysql_timeout_timer", 1) < 0) ){
        log(g_log, "prepare_mysql_timeout_timer register error\n");
        return res;
    }

    if( (res = timer_register(idle_timeout_timer, 30, \
                                        "idle_timeout_timer", 1) < 0) ){
        log(g_log, "idle_timeout_timer register error\n");
        return res;
    }

    return res;
}

/*
 * fun: init connection
 * arg: connection struct pointer
 * ret: success 0, error -1
 *
 */

static int conn_init(conn_t *c)
{
    if(c == NULL){
        log(g_log, "conn_init success\n");
        return -1;
    }

    c->connid = connid++;
    c->cli = NULL;
    c->my = NULL;
    c->state = STATE_UNAVAIL;
    c->state_time = time(NULL);
    bzero(c->curdb, sizeof(c->curdb));
    c->comno = 0;
    bzero(c->arg, sizeof(c->arg));

    gettimeofday(&(c->tv_start), NULL);
    gettimeofday(&(c->tv_end), NULL);

    INIT_LIST_HEAD(&(c->link));

    return buf_init(&(c->buf));
}

/*
 * fun: alloc connection
 * arg: 
 * ret: success return connection, error return NULL
 *
 */

static conn_t *conn_alloc(void)
{
    conn_t *c;

    c = genpool_alloc_page(conn_pool);
    if(c == NULL){
        log(g_log, "genpool alloc page error\n");
        return NULL;
    }

    if(conn_init(c) < 0){
        genpool_release_page(conn_pool, c);
        return NULL;
    }

    return c;
}

/*
 * fun: open connection
 * arg: connection fd, ip and port
 * ret: success return connection, error return NULL
 *
 */

conn_t *conn_open(int fd, uint32_t ip, uint16_t port)
{//分配conn_t和cli_conn_t， 并挂接起来 
    int res = 0;
    conn_t *c;

    if( (c = conn_alloc()) == NULL ){//这个连接其实是个中间搭桥的连接。
        log(g_log, "conn alloc error\n");
        return NULL;
    }

    if( (res = cli_conn_open(c, fd, ip, port)) < 0 ){//将c跟当前的fd连接起来，建立一个客户端连接结构
        log(g_log, "cli conn open error\n");
        conn_release(c);
        return NULL;
    }

    return c;
}

/*
 * fun: release connection
 * arg: connection struct pointer
 * ret: success 0, error -1
 *
 */

static int conn_release(conn_t *c)
{
    if(c == NULL){
        log(g_log, "conn_release error\n");
        return -1;
    }

    c->my = NULL;
    c->cli = NULL;
    buf_reset(&(c->buf));

    return genpool_release_page(conn_pool, c);
}

/*
 * fun: close connection
 * arg: connection struct pointer
 * ret: success 0, error -1
 *
 */

int conn_close(conn_t *c)
{
    int res = 0;

    list_del_init(&(c->link));

    if(c->my){
        if( (res = my_conn_put(c->my, 1)) < 0 ){
            log(g_log, "put my conn error\n");
        }
        c->my = NULL;
    }

    if( (res = cli_conn_close(c->cli)) < 0 ){
        log(g_log, "cli conn close error\n");
    }
    c->cli = NULL;

    if( (res = conn_release(c)) < 0 ){
        log(g_log, "conn release error\n");
    }

    debug(g_log, "conn:%u connection close\n", c->connid);

    return res;
}

/*
 * fun: close connection and close mysql connection
 * arg: connection struct pointer
 * ret: success 0, error -1
 *
 */

int conn_close_with_my(conn_t *c)
{//这个操作很重，会干掉mysql的连接，已经客户端连接等所有数据!!!!
    int res = 0;

    list_del_init(&(c->link));

    if(c->my){
        if( (res = my_conn_close(c->my)) < 0 ){
            log(g_log, "my conn close error\n");
        }
        c->my = NULL;
    }

    if( (res = cli_conn_close(c->cli)) < 0 ){
        log(g_log, "cli conn close error\n");
    }
    c->cli = NULL;

    if( (res = conn_release(c)) < 0 ){
        log(g_log, "conn release error\n");
    }

    debug(g_log, "conn:%u connection close\n", c->connid);

    return res;
}

/*
 * fun: alloc mysql connection for connection
 * arg: connection struct pointer
 * ret: success 0, error -1
 *
 */

int conn_alloc_my_conn(conn_t *c)
{
	//下面为了选一个合适的连接，虽然当前分配了，但可能需要切换
    int dirty = 0;
    my_conn_t *my = c->my;
    cli_conn_t *cli = c->cli;

    if(my != NULL ){
        return 0;
    }

	if( (my = my_slave_conn_get(c, cli->ip, cli->port)) == NULL ){
		return -1;
	} else {
		c->my = my;
	}

    if(dirty){
        my_conn_ctx_set_dirty(my);
    }

    return 0;
}

/*
 * fun: set connection state: reading_client
 * arg: connection struct pointer
 * ret: success 0, error -1
 *
 */

int conn_state_set_reading_client(conn_t *c)
{
    if(c == NULL){
        return -1;
    }

    c->state = STATE_READING_CLIENT;
    c->state_time = time(NULL);

    list_move_tail(&(c->link), &read_client_head);

    debug(g_log, "conn:%d reading client\n", c->connid);

    return 0;
}

/*
 * fun: set connection state: writing mysql 
 * arg: connection struct pointer
 * ret: success 0, error -1
 *
 */

int conn_state_set_writing_mysql(conn_t *c)
{
    if(c == NULL){
        return -1;
    }

    c->state = STATE_WRITING_MYSQL;
    c->state_time = time(NULL);

    list_move_tail(&(c->link), &write_mysql_head);

	debug(g_log, "conn:%d writing mysql\n", c->connid);

    return 0;
}

/*
 * fun: set connection state: read mysql and write client
 * arg: connection struct pointer
 * ret: success 0, error -1
 *
 */

int conn_state_set_read_mysql_write_client(conn_t *c)
{
    if(c == NULL){
        return -1;
    }

    c->state = STATE_READ_MYSQL_WRITE_CLIENT;
    c->state_time = time(NULL);

    list_move_tail(&(c->link), &read_mysql_write_client_head);

    debug(g_log, "conn:%d read mysql write client\n", c->connid);

    return 0;
}

/*
 * fun: set connection state: prepare mysql
 * arg: connection struct pointer
 * ret: success 0, error -1
 *
 */

int conn_state_set_prepare_mysql(conn_t *c)
{
    if(c == NULL){
        return -1;
    }

    c->state = STATE_PREPARE_MYSQL;
    c->state_time = time(NULL);

    list_move_tail(&(c->link), &prepare_mysql_head);

    debug(g_log, "conn:%d prepare to write mysql\n", c->connid);

    return 0;
}

/*
 * fun: set connection state: idle
 * arg: connection struct pointer
 * ret: success 0, error -1
 *
 */

int conn_state_set_idle(conn_t *c)
{
    if(c == NULL){
        return -1;
    }

    c->state = STATE_IDLE;
    c->state_time = time(NULL);

    list_move_tail(&(c->link), &idle_head);

    debug(g_log, "conn:%d connection idle\n", c->connid);

    return 0;
}

/*
 * fun: connection timeout timer
 * arg: max connection, connection timer head, 
 * arg: timeout seconds, extend message
 * ret: success 0, error -1
 *
 */

static int
_conn_state_timeout_timer(unsigned long arg, \
                            struct list_head *head, \
                            int timeout, const char *msg)
{
    int count = 0;
    struct list_head *pos, *n;
    conn_t *c;
    time_t now = time(NULL);

    list_for_each_safe(pos, n, head){
        if(count++ >= arg){
            break;
        }

        c = list_entry(pos, conn_t, link);

        if(now - c->state_time > timeout){
            log(g_log, "conn:%u %s\n", c->connid, msg);
            list_del_init(pos);
            conn_close(c);
        } else {
            break;
        }
    }

    return 0;
}

/*
 * fun: connection timeout timer: read client
 * arg: max connection to be processed
 * ret: success 0, error -1
 *
 */

static int read_client_timeout_timer(unsigned long arg)
{
    struct list_head *head;

    head = &read_client_head;
    return _conn_state_timeout_timer(arg, head, g_conf.read_client_timeout, "read_client_timeout");
}

/*
 * fun: connection timeout timer: write mysql
 * arg: max connection to be processed
 * ret: success 0, error -1
 *
 */

static int write_mysql_timeout_timer(unsigned long arg)
{
    struct list_head *head;

    head = &write_mysql_head;
    return _conn_state_timeout_timer(arg, head, \
                    g_conf.write_mysql_timeout, "write_mysql_timeout");
}

/*
 * fun: connection timeout timer: read mysql and write client
 * arg: max connection to be processed
 * ret: success 0, error -1
 *
 */

static int read_mysql_write_client_timeout_timer(unsigned long arg)
{
    struct list_head *head;

    head = &read_mysql_write_client_head;
    return _conn_state_timeout_timer(arg, head, \
                    g_conf.read_mysql_write_client_timeout, \
                    "read_mysql_write_client_timeout");
}

/*
 * fun: connection timeout timer: prepare mysql
 * arg: max connection to be processed
 * ret: success 0, error -1
 *
 */

static int prepare_mysql_timeout_timer(unsigned long arg)
{
    struct list_head *head;

    head = &prepare_mysql_head;
    return _conn_state_timeout_timer(arg, head, \
                    g_conf.prepare_mysql_timeout, \
                    "prepare_mysql_timeout");
}

/*
 * fun: connection timeout timer: idle
 * arg: max connection to be processed
 * ret: success 0, error -1
 *
 */

static int idle_timeout_timer(unsigned long arg)
{
    struct list_head *head;

    head = &idle_head;
    return _conn_state_timeout_timer(arg, head, \
                    g_conf.idle_timeout, "idle_timeout");

    return 0;
}

int conn_pool_destroy( )
{
	if( conn_pool != NULL){
		genpool_destroy( conn_pool) ;
		conn_pool = NULL ;
	}

    return 0;
}
