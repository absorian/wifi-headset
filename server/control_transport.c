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

static int s_sock = -1;
static int s_cl_sock = -1;

int control_transport_alive()
{
    if (s_sock < 0 || s_cl_sock < 0) return -1;

    int ret = recv(s_sock, NULL, 0, MSG_DONTWAIT | MSG_PEEK);

    if (ret == 0) {
        // disconnected
        return -1;
    }
    return 0;
}

int control_transport_accept(const cl_conn_info_t *conn_info)
{
    int ret;
    socklen_t socklen;
    struct sockaddr_in client;

    if (s_sock < 0 || s_cl_sock >= 0) return 0;

    while (1) {
        ret = listen(s_sock, 1);
        if (ret != 0) {
            fprintf(stderr, "Socket listen failed errno=%d\n", errno);
            return -1;
        }

        socklen = sizeof(client);
        s_cl_sock = accept(s_sock, (struct sockaddr *)&client, &socklen);
        if (s_cl_sock == -1) {
            fprintf(stderr, "Socket accept failed errno=%d\n", errno);
            return -1;
        }

        if (memcmp(&client.sin_addr, &conn_info->addr.sin_addr,
                sizeof(client.sin_addr))) {
            fprintf(stderr, "Socket wrong accept errno=%d\n", errno);
            close(s_cl_sock);
            s_cl_sock = -1;
            continue; 
        }

        break;
    }

    return 0;
}

int control_transport_open_conn(cl_conn_info_t *conn_info)
{
	int ret;
	struct sockaddr_in addr;
	socklen_t socklen;

	s_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (s_sock < 0) {
		fprintf(stderr, "Failed to open socket errno=%d\n", errno);
		return -1;
	}

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port = htons(0);
	ret = bind(s_sock, (struct sockaddr *)&addr, sizeof(addr));
	if (ret < 0) {
		fprintf(stderr, "Failed to bind socket errno=%d\n", errno);
		close(s_sock);
        s_sock = -1;
		return -1;
	}

	socklen = sizeof(addr);
	ret = getsockname(s_sock, (struct sockaddr *)&addr, &socklen);
	if (ret < 0) {
		fprintf(stderr, "Failed to getsockname errno=%d\n", errno);
		close(s_sock);
        s_sock = -1;
		return -1;
	}

	// Set the selected port to send back to cl
	conn_info->control_port = ntohs(addr.sin_port);

	return 0;
}

void control_transport_close_conn()
{
	close(s_cl_sock);
	close(s_sock);
    s_cl_sock = -1;
    s_sock = -1;
}
