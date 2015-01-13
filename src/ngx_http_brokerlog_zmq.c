/******************************************************************************
 * Copyright (c) 2014 by SAPO - PT Comunicações
 *
 *****************************************************************************/

/**
 * @file ngx_http_brokerlog_zmq.c
 * @author Dani Bento <dani@telecom.pt>
 * @date 1 March 2014
 * @brief Brokerlog ZMQ
 *
 * @see http://www.zeromq.org/
 */

#include <ngx_times.h>
#include <ngx_core.h>
#include <string.h>
#include <stdlib.h>

#include <zmq.h>

#include "ngx_http_brokerlog_zmq.h"

/**
 * @brief initialize ZMQ context
 *
 * Each location is owner of a ZMQ context. We should see this effected
 * reflected on the number of threads created by each nginx process.
 *
 * @param ctx A ngx_http_brokerlog_ctx_t pointer representing the actual
 *              location context
 * @return An int representing the OK (0) or error status
 * @note We should redefine this to ngx_int_t with NGX_OK | NGX_ERROR
 */
int
zmq_init_ctx(ngx_http_brokerlog_ctx_t *ctx)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, ctx->log, 0, "ZMQ: zmq_init_ctx()");
    /* each location has it's own context, we need to verify if this is the best
     * solution. We don't want to consume a lot of ZMQ threads to maintain the
     * communication */
    ctx->zmq_context = zmq_init((int) ctx->iothreads);
    if (NULL == ctx->zmq_context) {
        ngx_log_debug(NGX_LOG_DEBUG_HTTP, ctx->log, 0, "ZMQ: zmq_init(%d) fail", ctx->iothreads);
        return -1;
    }
    ctx->ccreated = 1;
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, ctx->log, 0, "ZMQ: zmq_init(%d) success", ctx->iothreads);
    return 0;
}

/**
 * @brief create ZMQ Context
 *
 * Read the actual configuration, verify if we dont have yet a context and
 * initiate it.
 *
 * @param cf A ngx_http_brokerlog_element_conf_t pointer to the location configuration
 * @return An int representing the OK (0) or error status
 * @note We should redefine this to ngx_int_t with NGX_OK | NGX_ERROR
 */
int
zmq_create_ctx(ngx_http_brokerlog_element_conf_t *cf)
{
    int  rc = 0;

    /* TODO: should we create the context structure here? */
    if (NULL == cf || NULL == cf->ctx) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->ctx->log, 0, "ZMQ: zmq_create_ctx() no configuration");
        return 1;
    }
    /* context is already created, return NGX_OK */
    if (1 == cf->ctx->ccreated) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->ctx->log, 0, "ZMQ: zmq_create_ctx() already created");
        return 0;
    }

    /* create location context */
    cf->ctx->iothreads = cf->iothreads;
    rc = zmq_init_ctx(cf->ctx);

    if (rc != 0) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->ctx->log, 0, "ZMQ: zmq_create_ctx() error");
        ngx_log_error(NGX_LOG_ERR, cf->ctx->log, 0, "ZMQ: zmq_create_ctx() error");
        return rc;
    }

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->ctx->log, 0, "ZMQ: zmq_create_ctx() success");
    return 0;
}

/**
 * @brief close ZMQ sockets and term ZMQ context
 *
 * We should close all sockets and term the ZMQ context before we totaly exit
 * nginx.
 *
 * @param ctx A ngx_http_brokerlog_ctx_t pointer to the actual module context
 * @return Nothing
 * @note Should we free the context itself here?
 */
void
zmq_term_ctx(ngx_http_brokerlog_ctx_t *ctx)
{
    /* close and nullify context zmq_socket */
    if (ctx->zmq_socket) {
        zmq_close(ctx->zmq_socket);
        ctx->zmq_socket = NULL;
    }

    /* term and nullify context zmq_context */
    if (ctx->zmq_context) {
        zmq_term(ctx->zmq_context);
        ctx->zmq_context = NULL;
    }

    /* nullify log */
    if (ctx->log) {
        ctx->log = NULL;
    }
    return;
}

/**
 * @brief create a ZMQ Socket
 *
 * Verify if it not exists and create a new socket to be available to write
 * messages.
 *
 * @param cf A ngx_http_brokerlog_element_conf_t pointer to the location configuration
 * @return An int representing the OK (0) or error status
 * @note We should redefine this to ngx_int_t with NGX_OK | NGX_ERROR
 * @warning It's important to look at here and define one socket per worker
 */
int
zmq_create_socket(ngx_http_brokerlog_element_conf_t *cf)
{
    int linger = ZMQ_NGINX_LINGER, rc = 0;
    uint64_t qlen = ZMQ_NGINX_QUEUE_LENGTH;

    char *connection;

    /* create a simple char * to the connection name */
    connection = calloc(cf->server->connection->len + 1, sizeof(char));
    memcpy(connection, cf->server->connection->data, cf->server->connection->len);

    ngx_log_debug(NGX_LOG_DEBUG_HTTP, cf->ctx->log, 0, "ZMQ: zmq_create_socket() to %s", connection);

    /* override the default qlen if cf as any */
    if (cf->qlen != qlen) {
        qlen = (uint64_t) cf->qlen;
    }

    /* verify if we have a context created */
    if (NULL == cf->ctx->zmq_context) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->ctx->log, 0, "ZMQ: zmq_create_socket() no context to create a socket");
        return -1;
    }

    /* verify if we have already a socket associated */
    if (0 == cf->ctx->screated) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->ctx->log, 0, "ZMQ: zmq_create_socket() create socket");
        cf->ctx->zmq_socket = zmq_socket(cf->ctx->zmq_context, ZMQ_PUB);
        /* verify if it was created */
        if (NULL == cf->ctx->zmq_socket) {
            ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->ctx->log, 0, "ZMQ: zmq_create_socket() socket not created");
            ngx_log_error(NGX_LOG_ERR, cf->ctx->log, 0, "ZMQ: zmq_create_socket() socket not created");
            return -1;
        }
        cf->ctx->screated = 1;
    }

    /* set socket option ZMQ_LINGER */
    rc = zmq_setsockopt(cf->ctx->zmq_socket, ZMQ_LINGER, &linger, sizeof(linger));
    if ( rc != 0) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->ctx->log, 0, "ZMQ: zmq_create_socket() error setting ZMQ_LINGER");
        ngx_log_error(NGX_LOG_ERR, cf->ctx->log, 0, "ZMQ: zmq_create_socket() error setting ZMQ_LINGER");
        return -1;
    }

    /* set socket option ZMQ_HWM */
    rc = zmq_setsockopt(cf->ctx->zmq_socket, ZMQ_HWM, &qlen, sizeof(qlen));
    if ( rc != 0) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->ctx->log, 0, "ZMQ: zmq_create_socket() error setting ZMQ_HWM");
        ngx_log_error(NGX_LOG_ERR, cf->ctx->log, 0, "ZMQ: zmq_create_socket() error setting ZMQ_HWM");
        return -1;
    }

    /* open zmq connection to */
    rc = zmq_connect(cf->ctx->zmq_socket, connection);
    if ( rc != 0) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->ctx->log, 0, "ZMQ: zmq_create_socket() error connecting");
        ngx_log_error(NGX_LOG_ERR, cf->ctx->log, 0, "ZMQ: zmq_create_socket() error connecting");
        return -1;
    }
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->ctx->log, 0, "ZMQ: zmq_create_socket() end");

    /* please, clean all your temporary variables */
    free(connection);

    /* if all was OK, we should return 0 */
    return rc;
}

/**
 * @brief serialize a ZMQ message
 *
 * Process all input data and the endpoint and build a message to be sended by ZMQ
 *
 * @param pool A ngx_pool_t pointer to the nginx memory manager
 * @param endpoint A ngx_str_t pointer to the current endpoint configured to use
 * @param data A ngx_str_t pointer with the message compiled to be sended
 * @param output A ngx_str_t pointer which will be pointed to the final message to be sent
 * @return An ngx_int_t with NGX_OK | NGX_ERROR
 * @note It will be cool if we use the *msg and *endp from the nginx memory pool too
 */
ngx_int_t
brokerlog_serialize_zmq(ngx_pool_t *pool, ngx_str_t *endpoint, ngx_str_t *data, ngx_str_t *output) {
    size_t msg_len = 0;
    char *msg;
    char *endp;

    /* the final message sent to broker is composed by endpoint+data
     * eg: endpoint = /stratus/, data = {'num':1}
     * final message /stratus/{'num':1}
     */
    msg_len = endpoint->len + data->len + 1;

    endp = strndup((char *) endpoint->data, endpoint->len);
    if (NULL == endp) {
        return NGX_ERROR;
    }

    /* strncat needs a null char in the end of the string to work well */
    endp[endpoint->len] = '\0';

    msg = ngx_pcalloc(pool, msg_len);

    if (NULL == msg) {
        return NGX_ERROR;
    }

    msg = strncat(msg, endp, endpoint->len);

    if (NULL == msg) {
        free(endp);
        return NGX_ERROR;
    }

    msg = strncat(msg, (const char *) data->data, data->len);

    if (NULL == msg) {
        free(endp);
        return NGX_ERROR;
    }

    output->len = msg_len - 1;  /* the trailing '\0' isn't part of the message */
    output->data = ngx_pcalloc(pool, output->len);

    /* copy the final message to the output data and clean all */
    memcpy(output->data, msg, output->len);

    if (NULL == output->data) {
        free(endp);
        ngx_pfree(pool, msg);
        return NGX_ERROR;
    }

    free(endp);
    ngx_pfree(pool, msg);

    return NGX_OK;
}
