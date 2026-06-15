/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"

#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static volatile sig_atomic_t s_run = 1;

void sig_handler(int sig)
{
	s_run = 0;
}

int main()
{
	int ret;
	cl_conn_info_t conn_info;
	struct sigaction act;

	act.sa_handler = &sig_handler;
	act.sa_flags = SA_RESTART;
	sigemptyset(&act.sa_mask);

	ret = sigaction(SIGINT, &act, NULL);
	if (ret) {
		fprintf(stderr, "Sigaction registration failed\n");
		return -1;
	}

	ret = audio_transport_init();
	if (ret) {
		return -1;
	}

	while (s_run) {
		ret = discovery_open();
		if (ret)
			break;

		// blocking
		discovery_capture(&conn_info);

		ret = control_transport_open_conn(&conn_info);
		if (ret)
			break;

		ret = audio_transport_open_conn(&conn_info);
		if (ret)
			break;

		ret = discovery_respond(&conn_info);
		if (ret)
			break;

		// blocking
		ret = control_transport_accept(&conn_info);
		if (ret)
			break;

		discovery_close();
		audio_transport_start();
		printf("Connection successful\n");

		while (s_run) {
			ret = control_transport_alive();
			if (ret) {
				printf("Remote disconnected\n");
				break;
			}
			sleep(1);
		}

		audio_transport_stop();
		audio_transport_close_conn();
		control_transport_close_conn();
	}

	discovery_close();
	control_transport_close_conn();
	audio_transport_close_conn();
	audio_transport_deinit();
	return 0;
}
