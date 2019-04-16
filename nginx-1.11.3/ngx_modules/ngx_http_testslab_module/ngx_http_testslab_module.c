
/*
 * Copyright (C) Hui Xie
 * Copyright (C) My, Inc.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    // 对应于ngx_rbtree_node_t最后一个data成员
    u_char                  rbtree_node_data;

    // 按先后顺序把所有访问节点串起来，方便淘汰过期结点
    ngx_queue_t             queue;

    // 上一次成功访问该URL的时间，精确到毫秒
    ngx_msec_t              last;

    // 客户端IP地址与URL组合而成的字符串长度
    u_short                 len;

    // 以字符串保存客户端IP地址与URL
    u_char                  data[1];
} ngx_http_testslab_node_t;


//ngx_http_testslab_shm_t保存在共享内存中
typedef struct {
    //红黑数用于快速检索
    ngx_rbtree_t            rbtree;

    //使用Nginx红黑树必须定义的哨兵结点
    ngx_rbtree_node_t       sentinel;

    //所有操作记录构成的淘汰链表
    ngx_queue_t             queue;
} ngx_http_testslab_shm_t;


typedef struct {
    //共享内存大小
    ssize_t         shmsize;

    //两次成功访问所必须间隔的时间
    ngx_int_t       interval;

    //操作共享内存一定需要ngx_slab_pool_t结构体
    //这个结构体也在共享内存中
    ngx_slab_pool_t        *shpool;

    //指向共享内存中的ngx_http_testslab_shm_t结构体
    ngx_http_testslab_shm_t*  sh;

} ngx_http_testslab_conf_t;


static void ngx_http_testslab_expire(ngx_http_request_t *r, ngx_http_testslab_conf_t *conf);
static ngx_int_t ngx_http_testslab_handler(ngx_http_request_t *r);
static char * ngx_http_testslab_createmem(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_testslab_shm_init(ngx_shm_zone_t *shm_zone, void *data);
static ngx_int_t ngx_http_testslab_init(ngx_conf_t *cf);
static void ngx_http_testslab_rbtree_insert_value(ngx_rbtree_node_t *temp, ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static void * ngx_http_testslab_create_main_conf(ngx_conf_t *cf);
static ngx_int_t ngx_http_testslab_lookup(ngx_http_request_t *r,
                                          ngx_http_testslab_conf_t *conf, ngx_uint_t hash, u_char* data, size_t len);

static ngx_command_t  ngx_http_testslab_commands[] = {

    { ngx_string("test_slab"),
      NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE2,
      ngx_http_testslab_createmem,
      0,
      0,
      NULL },

    ngx_null_command  
};



static ngx_http_module_t ngx_http_testslab_module_ctx = {
    NULL,
    ngx_http_testslab_init,

    ngx_http_testslab_create_main_conf,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};


ngx_module_t ngx_http_testslab_module = {
    NGX_MODULE_V1,
    &ngx_http_testslab_module_ctx,
    ngx_http_testslab_commands,
    NGX_HTTP_MODULE,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NGX_MODULE_V1_PADDING
};


static char *
ngx_http_testslab_createmem(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_str_t                   *value;
    ngx_shm_zone_t              *shm_zone;

    //con参数为ngx_http_testslab_create_main_conf创建的结构体
    ngx_http_testslab_conf_t *mconf = (ngx_http_testslab_conf_t *)conf;

    //这块共享内存的名字
    ngx_str_t name = ngx_string("test_slab_shm");

    //渠道test_slab配置项后的参数数组
    value = cf->args->elts;

    //获取两成功访问的时间间隔
    mconf->interval = 1000*ngx_atoi(value[1].data, value[1].len);
    if (mconf->interval == NGX_ERROR || mconf->interval == 0) {
        //约定设置为-1就关闭模块的限速功能
        mconf->interval = -1;
        return "invalid value";
    }

    //获取共享内存的大小
    mconf->shmsize = ngx_parse_size(&value[2]);
    if (mconf->shmsize == NGX_ERROR || mconf->shmsize == 0) {
        mconf->interval = -1;
        return "invalid value";
    }

    //要求Nginx准备分配共享内存
    shm_zone = ngx_shared_memory_add(cf, &name, mconf->shmsize, 
                                     &ngx_http_testslab_module);
    if (shm_zone == NULL) {
        mconf->interval = -1;

        return NGX_CONF_ERROR;
    }

    // 设置共享内存分配成功厚的回调方法
    shm_zone->init = ngx_http_testslab_shm_init;

    //设置init回调时可以由data中获取ngx_http_testslab_conf_t配置结构体
    shm_zone->data = mconf;

    return NGX_CONF_OK;
}


static ngx_int_t
ngx_http_testslab_shm_init(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_http_testslab_conf_t  *conf;
    //data成员可能为空，也可能时上次ngx_http_testslab_shm_init执行完成后的shm_zone->data
    ngx_http_testslab_conf_t  *oconf = data;
    size_t                     len;

    //shm_zone->data存储着本次初始化cycle时创建的ngx_http_testslab_conf_t配置结构体
    conf = (ngx_http_testslab_conf_t *)shm_zone->data;

    //判断是否为reload配置项后导致的初始化共享内存
    if (oconf) {
        //本次初始化的共享内存不是新创建的
        //此时，data成员里就是上次创建的ngx_http_testslab_conf_t
        conf->sh = oconf->sh;
        conf->shpool = oconf->shpool;
        return NGX_OK;
    } 

    conf->shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    conf->sh = ngx_slab_alloc(conf->shpool, sizeof(ngx_http_testslab_shm_t));
    if (conf->sh == NULL) {
        return NGX_ERROR;
    }
    conf->shpool->data = conf->sh;

    // 初始化红黑树
    ngx_rbtree_init(&conf->sh->rbtree, &conf->sh->sentinel,
                    ngx_http_testslab_rbtree_insert_value);
    ngx_queue_init(&conf->sh->queue);

    len = sizeof(" in testslab \"\"") + shm_zone->shm.name.len;

    conf->shpool->log_ctx = ngx_slab_alloc(conf->shpool, len);
    if (conf->shpool->log_ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_sprintf(conf->shpool->log_ctx, " in testslab \"%V\"%Z", &shm_zone->shm.name);

    return NGX_OK;
}


static ngx_int_t
ngx_http_testslab_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_testslab_handler;

    return NGX_OK;
}

static ngx_int_t
ngx_http_testslab_handler(ngx_http_request_t *r)
{
    size_t                     len;
    uint32_t                   hash;
    ngx_int_t                  rc;
    ngx_http_testslab_conf_t  *conf;

    conf = ngx_http_get_module_main_conf(r, ngx_http_testslab_module);
    rc = NGX_DECLINED;

    if (conf->interval == -1)
        return rc;
    
    len = r->connection->addr_text.len + r->uri.len;
    u_char* data = ngx_palloc(r->pool, len);
    ngx_memcpy(data, r->uri.data, r->uri.len);
    ngx_memcpy(data+r->uri.len, r->connection->addr_text.data, r->connection->addr_text.len);

    hash = ngx_crc32_short(data, len);

    //多进程同时操作同一共享内存，需要枷锁
    ngx_shmtx_lock(&conf->shpool->mutex);

    rc = ngx_http_testslab_lookup(r, conf, hash, data, len);

    ngx_shmtx_unlock(&conf->shpool->mutex);

    return rc;
}


static void
ngx_http_testslab_rbtree_insert_value(ngx_rbtree_node_t *temp,
                                ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t           **p;
    ngx_http_testslab_node_t    *lrn, *lrnt;

    for ( ;; ) {
        if (node->key < temp->key) {
            p = &temp->left;
        } else if (node->key > temp->key) {
            p = &temp->right;
        } else {
            lrn = (ngx_http_testslab_node_t *) &node->data;
            lrnt = (ngx_http_testslab_node_t *) &temp->data;

            p = (ngx_memn2cmp(lrn->data, lrnt->data, lrn->len, lrnt->len) < 0) ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}

static void *
ngx_http_testslab_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_testslab_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_testslab_conf_t));
    if (conf == NULL) {
        return NULL;
    }
    conf->interval = -1;
    conf->shmsize = -1;

    return conf;
}


static ngx_int_t
ngx_http_testslab_lookup(ngx_http_request_t *r,
                ngx_http_testslab_conf_t *conf, ngx_uint_t hash, u_char *data, size_t len)
{
    size_t                      size;
    ngx_int_t                   rc;
    ngx_time_t                 *tp;
    ngx_msec_t                  now;
    ngx_msec_int_t              ms;
    ngx_rbtree_node_t          *node, *sentinel;
    ngx_http_testslab_node_t   *lr;

    tp = ngx_timeofday();
    now = (ngx_msec_t) (tp->sec * 1000 + tp->msec);

    node = conf->sh->rbtree.root;
    sentinel = conf->sh->rbtree.sentinel;
    ngx_log_debug2(NGX_LOG_ERR, r->connection->log, 0, "####### interval: %d, shsize: %d", conf->interval, conf->shmsize);
    while (node != sentinel) {

        if (hash < node->key) {
            node = node->left;
            continue;
        }

        if (hash > node->key) {
            node = node->right;
            continue;
        }

        lr = (ngx_http_testslab_node_t *) &node->data;
        rc = ngx_memn2cmp(data, lr->data, len, (size_t) lr->len);

        if (rc == 0) {
            ms = (ngx_msec_int_t) (now - lr->last);
            
            if (ms > conf->interval) {
                lr->last = now;

                ngx_queue_remove(&lr->queue);
                ngx_queue_insert_head(&conf->sh->queue, &lr->queue);

                return NGX_DECLINED;
            } else {
                return NGX_HTTP_FORBIDDEN;
            }
        }

        node = (rc < 0) ? node->left : node->right;
    }

    // new node
    size = offsetof(ngx_rbtree_node_t, data)
           + offsetof(ngx_http_testslab_node_t, data)
           + len;
    ngx_http_testslab_expire(r, conf);

    node = ngx_slab_alloc_locked(conf->shpool, size);
    if (node == NULL) {
        return NGX_ERROR;
    }

    node->key = hash;
    
    lr = (ngx_http_testslab_node_t *) &node->data;

    lr->last = now;

    lr->len = (u_char) len;
    ngx_memcpy(lr->data, data, len);

    ngx_rbtree_insert(&conf->sh->rbtree, node);

    ngx_queue_insert_head(&conf->sh->queue, &lr->queue);

    return NGX_DECLINED;
}


static void
ngx_http_testslab_expire(ngx_http_request_t *r, ngx_http_testslab_conf_t *conf)
{
    ngx_time_t                 *tp;
    ngx_msec_t                  now;
    ngx_queue_t                *q;
    ngx_msec_int_t              ms;
    ngx_rbtree_node_t          *node;
    ngx_http_testslab_node_t   *lr;

    tp = ngx_timeofday();

    now = (ngx_msec_t) (tp->sec * 1000 + tp->msec);

    while(1) {
        if (ngx_queue_empty(&conf->sh->queue)) {
            return;
        }

        q = ngx_queue_last(&conf->sh->queue);

        lr = ngx_queue_data(q, ngx_http_testslab_node_t, queue);

        node = (ngx_rbtree_node_t *) ((u_char *) lr - offsetof(ngx_rbtree_node_t, data));

        ms = (ngx_msec_int_t) (now - lr->last);

        if (ms < conf->interval) {
            return;
        }

        ngx_queue_remove(q);
        ngx_rbtree_delete(&conf->sh->rbtree, node);

        ngx_slab_free_locked(conf->shpool, node);
    }
}