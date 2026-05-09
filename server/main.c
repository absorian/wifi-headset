/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"

#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

int main()
{
	int ret;
	int i = 0;
	cl_conn_info_t conn_info;

	ret = discovery_open();
	if (ret) {
		return -1;
	}

	// blocking
	discovery_capture(&conn_info);
	
	ret = control_transport_open_conn(&conn_info);
	if (ret) {
		discovery_close();
		return -1;
	}

	ret = discovery_respond(&conn_info);
	if (ret) {
		discovery_close();
		control_transport_close_conn();
		return -1;
	}

	// blocking
	ret = control_transport_accept(&conn_info);
	if (ret) {
		discovery_close();
		control_transport_close_conn();
		return -1;
	}

	printf("Connection successful\n");
	discovery_close();

	while (i < 5) {
		ret = control_transport_alive();
		if (ret < 0) {
			printf("Remote disconnected\n");
			control_transport_close_conn();
			return -1;
		}
		sleep(1);
		i++;
	}

	printf("Closing connection\n");
	control_transport_close_conn();
	return 0;
}