/*
 * Copyright 2015-2017 Two Pore Guys, Inc.
 * All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted providing that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <errno.h>
#include <glib.h>
#include <rpc/object.h>
#include <rpc/server.h>
#include "internal.h"

static int rpc_server_accept(rpc_server_t, rpc_connection_t);

static int
rpc_server_accept(rpc_server_t server, rpc_connection_t conn)
{
        struct rpc_dispatch_item *itm;

        if (server->rs_closed)
                return (-1);
        itm = g_malloc0(sizeof(*itm));
        q_rw_lock_writer_lock(&server->rs_connections_rwlock);
	server->rs_connections = g_list_append(server->rs_connections, conn);
        q_rw_lock_writer_unlock(&server->rs_connections_rwlock);
        itm->rd_type = RPC_TYPE_CONNECTION
        itm->rd_item.rd_conn = conn;
        itm->code = (int)RPC_CONNECTION_ARRIVED;
        itm->args = server;
        rpc_context_dispatch(server->rs_context, itm);
	return (0);
}

static void *
rpc_server_worker(void *arg)
{
	rpc_server_t server = arg;

	g_main_context_push_thread_default(server->rs_g_context);
	g_main_loop_run(server->rs_g_loop);
	return (NULL);
}

static gboolean
rpc_server_listen(void *arg)
{
	rpc_server_t server = arg;
	const struct rpc_transport *transport;
	char *scheme;

	g_mutex_lock(&server->rs_mtx);

	scheme = g_uri_parse_scheme(server->rs_uri);
	transport = rpc_find_transport(scheme);
	g_free(scheme);

	if (transport == NULL) {
		errno = ENXIO;
		goto done;
	}

	debugf("selected transport %s", transport->name);
	server->rs_flags = transport->flags;
	transport->listen(server, server->rs_uri, NULL);
	server->rs_operational = true;

done:
	g_cond_signal(&server->rs_cv);
	g_mutex_unlock(&server->rs_mtx);
	return (false);
}

rpc_server_t
rpc_server_create(const char *uri, rpc_context_t context)
{

	rpc_server_t server;

	debugf("creating server");

	server = g_malloc0(sizeof(*server));
	server->rs_uri = uri;
	server->rs_paused = true;
	server->rs_context = context;
	server->rs_accept = &rpc_server_accept;
	server->rs_g_context = g_main_context_new();
	server->rs_g_loop = g_main_loop_new(server->rs_g_context, false);
	server->rs_thread = g_thread_new("librpc server", rpc_server_worker,
	    server);
	g_cond_init(&server->rs_cv);
	g_mutex_init(&server->rs_mtx);

	g_mutex_lock(&server->rs_mtx);
	g_main_context_invoke(server->rs_g_context, rpc_server_listen, server);

	while (!server->rs_operational)
		g_cond_wait(&server->rs_cv, &server->rs_mtx);

        g_rw_lock_writer_lock(&context->rcx_server_rwlock);
	g_ptr_array_add(context->rcx_servers, server);
        g_rw_lock_writer_unlock(&context->rcx_server_rwlock);
	g_mutex_unlock(&server->rs_mtx);
	return (server);
}

void
rpc_server_broadcast_event(rpc_server_t server, const char *path,
    const char *interface, const char *name, rpc_object_t args)
{
	GList *item;

        if (server->rs_closed)
                return;
	for (item = g_list_first(server->rs_connections); item;
	     item = item->next) {
		rpc_connection_t conn = item->data;
		rpc_connection_send_event(conn, path, interface, name,
		    rpc_retain(args));
	}
}

int
rpc_server_dispatch(rpc_server_t server, struct rpc_inbound_call *call)
{
	int ret;
        struct rpc_dispatch_item *itm = g_malloc0(sizeof(*itm));

        itm->rd_type = RPC_TYPE_CALL;
        itm->rd_item.rd_icall = call;
	ret = rpc_context_dispatch(server->rs_context, itm);
	return (ret);
}

rpc_server_t
rpc_server_get_connection_server(rpc_connection_t conn)
{

        return(conn->rco_server);
}

void 
rpc_server_set_event_handler(rpc_server_t server, 
    rpc_server_event_handler_t handler)
{

        if (server->rs_event != NULL):
                Block_release(server->rs_event);
        server->rs_event = Block_copy(handler);
} 

int
rpc_server_connection_change(rpc_server_t server, struct rpc_dispatch_item *itm)
{
        rpc_connection_t conn = itm->rd_item.rd_conn;

        if (server->rs_handler != NULL) 
                server->rs_handler(conn, (enum rpc_server_event_t)itm->rd_code, 
                    itm->args);
        if (itm->code == (int)RPC_CONNECTION_TERMINATED)
                return (rpc_server_remove_connection(server, conn));
        return (0);
}

int 
rpc_server_remove_connection(rpc_server_t server, rpc_connection_t conn)
{
        GList *iter = NULL;
        struct rpc_connection *comp = NULL;
        int ret = -1;

        q_rw_lock_writer_lock(&server->rs_connections_rwlock);
        for (iter = server->rs_connections; iter != NULL; iter = iter->next) {
                comp = iter->data;
                if (comp == conn) {
                        server->rs_connections = 
                            g_list_remove_link(server->rs_connections, iter);
                        ret = 0;
                        g_mutex_lock(&server->rs_mtx);
                        break;
                }
        }

        if (ret == -1) {
                q_rw_lock_writer_unlock(&server->rs_connections_rwlock);
                return (ret);
        }
        if (server->rs_connections == NULL)
                g_cond_signal(&server->rs_cv);
        q_rw_lock_writer_unlock(&server->rs_connections_rwlock);
        g_mutex_unlock(&server->rs_mtx);
        
        return (0);
}

int
rpc_server_close(rpc_server_t server)
{
	struct rpc_connection *conn;
	GList *iter = NULL;
        int ret = 0;
        gboolean present; 

        g_rw_lock_writer_lock(&context->rcx_server_rwlock);
        present = g_ptr_array_remove(context->rcx_server, server);
        g_rw_lock_writer_unlock(&context->rcx_server_rwlock);
        if (!present )
                return (-1);
        server->rs_closed = true;

        if (server->rs_teardown) {
                /* teardown is expected to stop future connections */
                ret = server->rs_teardown(server);
        }

	/* Drop all connections */
        q_rw_lock_reader_lock(&server->rs_connections_rwlock);
        if (server->rs_connections != NULL) {
		for (iter = server->rs_connections; iter != NULL; 
                    iter = iter->next) {
			conn = iter->data;
			if (conn->rco_abort)
				conn->rco_abort(conn->rco_arg);
		}
                g_mutex_lock(&server->rs_mtx);
                q_rw_lock_reader_unlock(&server->rs_connections_rwlock)
                
                while (server->rs_connections != NULL)
                        g_cond_wait(&server->rs_cv, &server->rs_mtx);
                g_mutex_unlock(&server->rs_mtx);
        }
        else 
                q_rw_lock_reader_unlock(&server->rs_connections_rwlock);
        
        /* Now, its safe to stop the listener thread, kill the main loop,
         * reclaim server resources. TODO
         */

	return (ret);
}
