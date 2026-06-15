/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#pragma once

#include <netinet/in.h>

#define _STR(x) #x
#define STR(x) _STR(x)

#define CL_NAME_MAX 32

typedef struct {
	char name[CL_NAME_MAX + 1];
	uint64_t uid;
	struct sockaddr_in addr;

	uint16_t audio_port;
	uint16_t control_port;
} cl_conn_info_t;

int discovery_open();
void discovery_close();
void discovery_capture(cl_conn_info_t *cl);
int discovery_respond(const cl_conn_info_t *conn_info);

int control_transport_open_conn(cl_conn_info_t *conn_info);
void control_transport_close_conn();
int control_transport_accept(const cl_conn_info_t *conn_info);
int control_transport_alive();

int audio_transport_init();
void audio_transport_deinit();
int audio_transport_open_conn(cl_conn_info_t *conn_info);
void audio_transport_close_conn();
void audio_transport_start();
void audio_transport_stop();
