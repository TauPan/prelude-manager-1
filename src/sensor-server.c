/*****
*
* Copyright (C) 2001, 2002, 2004 Yoann Vandoorselaere <yoann@mandrakesoft.com>
* All Rights Reserved
*
* This file is part of the Prelude program.
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2, or (at your option)
* any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; see the file COPYING.  If not, write to
* the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
*
*****/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <assert.h>

#include <libprelude/prelude.h>
#include <libprelude/prelude-log.h>
#include <libprelude/prelude-message-id.h>
#include <libprelude/prelude-ident.h>
#include <libprelude/prelude-extract.h>
#include <libprelude/prelude-connection.h>
#include <libprelude/prelude-connection-mgr.h>
#include <libprelude/prelude-option-wide.h>

#include "server-logic.h"
#include "server-generic.h"
#include "sensor-server.h"
#include "idmef-message-scheduler.h"
#include "manager-options.h"
#include "reverse-relaying.h"

typedef struct {
        SERVER_GENERIC_OBJECT;
        prelude_list_t list;
        
        idmef_queue_t *queue;
        prelude_connection_t *cnx;
        prelude_list_t write_msg_list;
        
        prelude_connection_capability_t capability;
} sensor_fd_t;


extern prelude_client_t *manager_client;


static PRELUDE_LIST_HEAD(sensors_cnx_list);
static pthread_mutex_t sensors_list_mutex = PTHREAD_MUTEX_INITIALIZER;




static sensor_fd_t *search_cnx(prelude_list_t *head, uint64_t analyzerid) 
{
        sensor_fd_t *cnx;
        prelude_list_t *tmp;

        prelude_list_for_each(tmp, head) {
                cnx = prelude_list_entry(tmp, sensor_fd_t, list);

                if ( cnx->ident == analyzerid )
                        return cnx;
        }

        return NULL;
}




static int forward_message_to_analyzerid(sensor_fd_t *cnx, uint64_t analyzerid, prelude_msg_t *msg) 
{
        int ret;
        sensor_fd_t *analyzer;
        
        pthread_mutex_lock(&sensors_list_mutex);

        analyzer = search_cnx(&sensors_cnx_list, analyzerid);
        if ( ! analyzer ) {
                pthread_mutex_unlock(&sensors_list_mutex);
                return -1;
        }
        
        pthread_mutex_unlock(&sensors_list_mutex);

        /*
         * It is not permited for an analyzer to contact another analyzer
         * that is a parent (not a children) of him. Except in case of
         * option reply.
         */
        if ( analyzer->cnx && prelude_msg_get_tag(msg) != PRELUDE_MSG_OPTION_REPLY )
                return -1;
        
        ret = prelude_msg_write(msg, analyzer->fd);
        if ( ret < 0 ) {
                if ( prelude_error_get_code(ret) == PRELUDE_ERROR_EAGAIN ) {
                        prelude_linked_object_add_tail((prelude_linked_object_t *) msg,
                                                       &analyzer->write_msg_list);

                        server_logic_notify_write_enable((server_logic_client_t *) analyzer);
                        return 0;
                }

                return -1;
        }
        
        return 0;
}



static int get_msg_target_ident(sensor_fd_t *client, prelude_msg_t *msg,
                                uint64_t **target_ptr, uint32_t *hop_ptr, int direction)
{
        int ret;
        void *buf;
        uint8_t tag;
        uint32_t len;
        uint32_t hop, tmp, target_len = 0;

        *target_ptr = NULL;
        
        while ( prelude_msg_get(msg, &tag, &len, &buf) == 0 ) {

                if ( tag == PRELUDE_MSG_OPTION_TARGET_ID ) {
                                                
                        if ( (len % sizeof(uint64_t)) != 0 || len < 2 * sizeof(uint64_t) )
                                return -1;
                        
                        target_len = len;
                        *target_ptr = buf;
                }

                if ( tag != PRELUDE_MSG_OPTION_HOP )
                        continue;

                if ( ! *target_ptr )
                        return -1;
                        
                ret = prelude_extract_uint32_safe(&hop, buf, len);
                if ( ret < 0 )
                        break;

                hop = (direction == PRELUDE_MSG_OPTION_REQUEST) ? hop + 1 : hop - 1;
                                
                if ( hop == (target_len / sizeof(uint64_t)) ) {                        
                        *hop_ptr = (hop - 1);
                        return 0; /* we are the target */
                }
                
                tmp = htonl(hop);
                memcpy(buf, &tmp, sizeof(tmp));
                                
                if ( hop >= (target_len / sizeof(uint64_t)) )
                        break;

                *hop_ptr = hop;                
                return 0;
        }

        server_generic_log_client((server_generic_client_t *) client,
                                  "message does not carry a valid target: closing connection.\n");

        return -1;
}



static int send_unreachable_message(sensor_fd_t *client, uint64_t *ident_list, uint32_t hop)
{
        ssize_t ret;
        prelude_msg_t *msg;
        char error[] = "Destination unreachable";

        /*
         * cancel the hop increment done previously.
         * this function is only supposed to be called for failed request (not failed reply).
         */
        hop--;
        
        ret = prelude_msg_new(&msg, 3,
                              sizeof(error) +
                              sizeof(uint32_t) +
                              hop * sizeof(uint64_t), PRELUDE_MSG_OPTION_REPLY, 0);
        if ( ret < 0 )
                return -1;

        prelude_msg_set(msg, PRELUDE_MSG_OPTION_ERROR, sizeof(error), error);
        prelude_msg_set(msg, PRELUDE_MSG_OPTION_TARGET_ID, hop * sizeof(uint64_t), ident_list);
        prelude_msg_set(msg, PRELUDE_MSG_OPTION_HOP, sizeof(hop), &hop);
        
        do {
                ret = prelude_msg_write(msg, client->fd);
        } while ( ret < 0 && prelude_error_get_code(ret) == PRELUDE_ERROR_EAGAIN );
        
        prelude_msg_destroy(msg);
        
        return 0;
}



static int request_sensor_option(sensor_fd_t *client, prelude_msg_t *msg) 
{
        int ret;
        uint64_t ident;
        uint32_t target_hop;
        uint64_t *target_route;
        prelude_client_profile_t *cp = prelude_client_get_profile(manager_client);
        
        ret = get_msg_target_ident(client, msg, &target_route,
                                   &target_hop, PRELUDE_MSG_OPTION_REQUEST);
        if ( ret < 0 )
                return -1;

        ident = prelude_extract_uint64(&target_route[target_hop]);
                
        if ( ident == prelude_client_profile_get_analyzerid(cp) ) {
                prelude_msg_recycle(msg);
                return prelude_option_process_request(manager_client, client->fd, msg);
        }
        
        ret = forward_message_to_analyzerid(client, ident, msg);
        if ( ret < 0 )
                send_unreachable_message(client, target_route, target_hop);
        
        return 0;
}




static int reply_sensor_option(sensor_fd_t *client, prelude_msg_t *msg) 
{
        int ret;
        uint32_t target_hop;
        uint64_t *target_route, ident;
        
        ret = get_msg_target_ident(client, msg, &target_route, &target_hop, PRELUDE_MSG_OPTION_REPLY);
        if ( ret < 0 ) 
                return -1;

        ident = prelude_extract_uint64(&target_route[target_hop]);
                
        /*
         * The one replying the option doesn't care about client presence or not.
         */
        forward_message_to_analyzerid(client, ident, msg);

        return 0;
}




static int handle_declare_ident(sensor_fd_t *cnx, void *buf, uint32_t blen) 
{
        int ret;

        ret = prelude_extract_uint64_safe(&cnx->ident, buf, blen);
        if ( ret < 0 )
                return -1;
        
        server_generic_log_client((server_generic_client_t *) cnx,
                                  "declared ident 0x%" PRIx64 ".\n", cnx->ident);
                
        return 0;
}




static int handle_declare_parent_relay(sensor_fd_t *cnx) 
{
        int state, ret;
        prelude_connection_t *pc;

        if ( ! cnx->ident )
                return -1;
        
        pc = reverse_relay_search_receiver(cnx->ident);
        if ( pc ) {
                /*
                 * This reverse relay is already known:
                 * Associate the new FD with it, and tell connection-mgr, the connection is alive.
                 */
                prelude_connection_set_fd(pc, cnx->fd);
        } else {
                /*
                 * First time a child relay with this address connect here.
                 * Add it to the manager list. Type of the created connection is -parent-
                 * because *we* are sending the alert to the child.
                 */
                ret = prelude_connection_new(&pc, cnx->addr, cnx->port);
                if ( ret < 0 ) {
                        log(LOG_INFO, "%s: creating connection for %s:%d: %s.\n",
                            prelude_strsource(ret), prelude_strerror(ret));
                        return -1;
                }
                
                prelude_connection_set_peer_analyzerid(pc, cnx->ident);
                prelude_connection_set_fd(pc, cnx->fd);
                reverse_relay_add_receiver(pc);
        }
        
        state = prelude_connection_get_state(pc);
        state |= PRELUDE_CONNECTION_ESTABLISHED;
        state &= ~PRELUDE_CONNECTION_OWN_FD;

        prelude_connection_set_state(pc, state);
        
        server_generic_log_client((server_generic_client_t *) cnx,
                                  "client declared to be a parent relay (we relay to him).\n");
        
        reverse_relay_tell_receiver_alive(pc);

        cnx->cnx = pc;
                
        return 0;
}




static int handle_declare_client(sensor_fd_t *cnx) 
{
        cnx->queue = idmef_message_scheduler_queue_new();
        if ( ! cnx->queue )
                return -1;
        
        pthread_mutex_lock(&sensors_list_mutex);
        prelude_list_add_tail(&cnx->list, &sensors_cnx_list);
        pthread_mutex_unlock(&sensors_list_mutex);
        
        return 0;
}




static int read_connection_type(sensor_fd_t *cnx, prelude_msg_t *msg) 
{
        void *buf;
        uint8_t tag;
        int ret = -1;  
        uint32_t dlen;
        
        ret = prelude_msg_get(msg, &tag, &dlen, &buf);
        if ( ret < 0 ) {
                server_generic_log_client((server_generic_client_t *) cnx, "error decoding message - %s: %s.\n",
                                          prelude_strsource(ret), prelude_strerror(ret));
                return -1;
        }
        
        cnx->capability = tag;
        
        if ( tag & PRELUDE_CONNECTION_CAPABILITY_RECV_IDMEF ) {
                
                ret = handle_declare_parent_relay(cnx);
                if ( ret < 0 )
                        return ret;
        }
        
        ret = handle_declare_client(cnx);
        if ( ret < 0 )
                return ret;
        
        return 0;
}




static int read_ident_message(sensor_fd_t *cnx, prelude_msg_t *msg) 
{
        int ret;        
        void *buf;
        uint8_t tag;
        uint32_t dlen;

        ret = prelude_msg_get(msg, &tag, &dlen, &buf);
        if ( ret < 0 ) {
                server_generic_log_client((server_generic_client_t *) cnx,
                                          "error decoding message - %s:%s.\n",
                                          prelude_strsource(ret), prelude_strerror(ret));
                return -1;
        }
        
        switch (tag) {
                
        case PRELUDE_MSG_ID_DECLARE:
                ret = handle_declare_ident(cnx, buf, dlen);
                break;
                
        default:
                server_generic_log_client((server_generic_client_t *) cnx, "unknow ID tag: %d.\n", tag);
                ret = -1;
                break;
        }
        
        return ret;
}




static int read_connection_cb(server_generic_client_t *client)
{
        int ret;
        uint8_t tag;
        prelude_msg_t *msg;
        sensor_fd_t *cnx = (sensor_fd_t *) client;
        
        ret = prelude_msg_read(&cnx->msg, cnx->fd);        
        if ( ret < 0 ) {
		if ( prelude_error_get_code(ret) == PRELUDE_ERROR_EAGAIN )
                        return 0;

                if ( prelude_error_get_code(ret) != PRELUDE_ERROR_EOF )
                        server_generic_log_client((server_generic_client_t *) cnx, "%s: %s\n",
                                                  prelude_strsource(ret), prelude_strerror(ret));

                return -1;
        }
        
        msg = cnx->msg;
        cnx->msg = NULL;
        
        tag = prelude_msg_get_tag(msg);
        
        /*
         * If we get there, we have a whole message.
         */        
        switch ( tag ) {

        case PRELUDE_MSG_IDMEF:
                ret = idmef_message_schedule(cnx->queue, msg);
                break;
                
        case PRELUDE_MSG_ID:
                ret = (cnx->ident) ? -1 : read_ident_message(cnx, msg);
                break;

        case PRELUDE_MSG_CONNECTION_CAPABILITY:
                ret = (cnx->capability) ? -1 : read_connection_type(cnx, msg);
                if ( ret == -2 ) {
                        prelude_msg_destroy(msg);
                        return ret;
                }
                
                break;
                
        case PRELUDE_MSG_OPTION_REQUEST:
                ret = request_sensor_option(cnx, msg);
                break;

        case PRELUDE_MSG_OPTION_REPLY:
                ret = reply_sensor_option(cnx, msg);
                break;
                
        default:
                ret = -1;
                break;
        }

        if ( ret < 0 ) 
                server_generic_log_client((server_generic_client_t *) cnx,
                                          "invalid message sent by the client.\n");

        if ( tag != PRELUDE_MSG_IDMEF )
                prelude_msg_destroy(msg);

        return (ret < 0) ? ret : read_connection_cb(client);
}



static int write_connection_cb(server_generic_client_t *ptr)
{
        int ret;
        prelude_list_t *tmp;
        prelude_msg_t *cur = NULL;
        sensor_fd_t *client = (sensor_fd_t *) ptr;
        
        assert(! prelude_list_empty(&client->write_msg_list));

        prelude_list_for_each(tmp, &client->write_msg_list) {
                cur = prelude_linked_object_get_object(tmp, prelude_msg_t);
                break;
        }

        ret = prelude_msg_write(cur, client->fd);
        if ( ret < 0 ) {
                if ( prelude_error_get_code(ret) == PRELUDE_ERROR_EAGAIN )
                        return 0;

                return -1;
        }

        prelude_linked_object_del((prelude_linked_object_t *) cur);

        if ( prelude_list_empty(&client->write_msg_list) )
                server_logic_notify_write_disable((server_logic_client_t *) client);
        
        return 0;
}



static void close_connection_cb(server_generic_client_t *ptr) 
{
        sensor_fd_t *cnx = (sensor_fd_t *) ptr;
        
        if ( cnx->cnx ) {
                cnx->fd = NULL;
                prelude_connection_close(cnx->cnx);
                reverse_relay_tell_dead(cnx->cnx, cnx->capability);
        }
        
        if ( ! prelude_list_empty(&cnx->list) ) {
                pthread_mutex_lock(&sensors_list_mutex);
                prelude_list_del(&cnx->list);
                pthread_mutex_unlock(&sensors_list_mutex);
        }

        /*
         * If cnx->msg is not NULL, it mean the sensor
         * closed the connection without finishing to send
         * a message. Destroy the unfinished message.
         */
        if ( cnx->msg )
                prelude_msg_destroy(cnx->msg);

        if ( cnx->queue )
                idmef_message_scheduler_queue_destroy(cnx->queue);
}




static int accept_connection_cb(server_generic_client_t *ptr) 
{
        sensor_fd_t *fd = (sensor_fd_t *) ptr;

        PRELUDE_INIT_LIST_HEAD(&fd->list);
        
        return 0;
}



server_generic_t *sensor_server_new(void) 
{
        server_generic_t *server;
        
        server = server_generic_new(sizeof(sensor_fd_t), accept_connection_cb,
                                    read_connection_cb, write_connection_cb, close_connection_cb);
        if ( ! server ) {
                log(LOG_ERR, "error creating a generic server.\n");
                return NULL;
        }
                
        return server;
}



void sensor_server_stop(server_generic_t *server) 
{
        server_generic_stop(server);
}




int sensor_server_add_client(server_generic_t *server, prelude_connection_t *cnx) 
{
        sensor_fd_t *cdata;
        
        cdata = calloc(1, sizeof(*cdata));
        if ( ! cdata ) {
                log(LOG_ERR, "memory exhausted.\n");
                return -1;
        }
        
        cdata->queue = idmef_message_scheduler_queue_new();
        if ( ! cdata->queue ) {
                free(cdata);
                return -1;
        }

        cdata->port = prelude_connection_get_dport(cnx);
        cdata->addr = strdup(prelude_connection_get_daddr(cnx));
        cdata->state |= SERVER_GENERIC_CLIENT_STATE_ACCEPTED;
        cdata->fd = prelude_connection_get_fd(cnx);
                
        cdata->cnx = cnx;
        cdata->client_type = "parent-manager";
        cdata->ident = prelude_connection_get_peer_analyzerid(cnx);

        prelude_list_add(&cdata->list, &sensors_cnx_list);
        
        server_generic_process_requests(server, (server_generic_client_t *) cdata);
        
        return 0;
}
