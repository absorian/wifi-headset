/*
 * SPDX-License-Identifier: GPL-3.0-only
 */

#include "common.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#define SV_DISCOVERY_PORT 48672

static int s_sock;

void discovery_capture(cl_conn_info_t *cl)
{
	int ret;
	socklen_t socklen;

	char rx_buf[128];

	printf("Listening for broadcast signals\n");

	while (1) {
		socklen = sizeof(cl->addr);
		ret = recvfrom(s_sock, rx_buf, sizeof(rx_buf), 0,
			       (struct sockaddr *)&cl->addr, &socklen);
		if (ret < 0) {
			fprintf(stderr, "recvfrom failed errno=%d\n", errno);
			continue;
		}
		rx_buf[sizeof(rx_buf) - 1] = '\0';

		ret = sscanf(rx_buf,
			     "hdst_conn_req,uid='%llx',name='%" STR(
				     CL_NAME_MAX) "[ a-zA-Z0-9_-]'",
			     &cl->uid, cl->name);
		if (ret != 2) {
			fprintf(stderr, "Failed to parse packet\n", errno);
			continue;
		}

		printf("Found device '%s' at %s:%hu\n", cl->name,
		       inet_ntoa(cl->addr.sin_addr), ntohs(cl->addr.sin_port));
		break;
	}
}

int discovery_respond(const cl_conn_info_t *cl)
{
	int ret;
	char payload[128];
	socklen_t socklen;

	printf("Sending approval back to '%s'\n", cl->name);

	sprintf(payload, "hdst_conn_resp,udp=%hu,tcp=%hu", cl->audio_port,
		cl->control_port);

	socklen = sizeof(cl->addr);
	ret = sendto(s_sock, payload, strlen(payload) + 1, 0,
		     (struct sockaddr *)&cl->addr, socklen);
	if (ret < 0) {
		fprintf(stderr, "sendto failed errno=%d\n", errno);
        return -1;
	}
    return 0;
}

int discovery_open()
{
	int ret;
	struct sockaddr_in bind_addr;

	s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (s_sock < 0) {
		fprintf(stderr, "Failed to open socket errno=%d\n", errno);
		return -1;
	}
	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = INADDR_ANY;
	bind_addr.sin_port = htons(SV_DISCOVERY_PORT);

	ret = bind(s_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
	if (ret < 0) {
		fprintf(stderr, "Failed to bind socket errno=%d\n", errno);
		close(s_sock);
		return -1;
	}

    return 0;
}

void discovery_close()
{
	close(s_sock);
}