/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * RDP Server Listener
 *
 * Copyright 2011 Vic Lee
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include <winpr/crt.h>
#include <winpr/windows.h>
#include <freerdp/log.h>

#ifndef _WIN32
#include <netdb.h>
#include <unistd.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <net/if.h>
#endif

#if defined(HAVE_AF_VSOCK_H)
#include <ctype.h>
#include <linux/vm_sockets.h>
#endif

#include <winpr/handle.h>

#include "listener.h"
#include "utils.h"

#define TAG FREERDP_TAG("core.listener")

static BOOL freerdp_listener_open_from_vsock(freerdp_listener* instance, const char* bind_address,
                                             UINT16 port)
{
#if defined(HAVE_AF_VSOCK_H)
	rdpListener* listener = (rdpListener*)instance->listener;
	const int sockfd = socket(AF_VSOCK, SOCK_STREAM, 0);
	if (sockfd == -1)
	{
		char ebuffer[256] = { 0 };
		WLog_ERR(TAG, "Error creating socket: %s", winpr_strerror(errno, ebuffer, sizeof(ebuffer)));
		return FALSE;
	}
	const int flags = fcntl(sockfd, F_GETFL, 0);
	if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1)
	{
		char ebuffer[256] = { 0 };
		WLog_ERR(TAG, "Error making socket nonblocking: %s",
		         winpr_strerror(errno, ebuffer, sizeof(ebuffer)));
		closesocket((SOCKET)sockfd);
		return FALSE;
	}
	struct sockaddr_vm addr = { 0 };

	addr.svm_family = AF_VSOCK;
	addr.svm_port = port;

	errno = 0;
	char* ptr = NULL;
	unsigned long val = strtoul(bind_address, &ptr, 10);
	if (errno || (val > UINT32_MAX))
	{
		char ebuffer[256] = { 0 };
		WLog_ERR(TAG, "could not extract port from '%s', value=%ul, error=%s", bind_address, val,
		         winpr_strerror(errno, ebuffer, sizeof(ebuffer)));
		return FALSE;
	}
	addr.svm_cid = val;
	if (bind(sockfd, (struct sockaddr*)&addr, sizeof(struct sockaddr_vm)) == -1)
	{
		char ebuffer[256] = { 0 };
		WLog_ERR(TAG, "Error binding vsock at cid %d port %d: %s", addr.svm_cid, port,
		         winpr_strerror(errno, ebuffer, sizeof(ebuffer)));
		closesocket((SOCKET)sockfd);
		return FALSE;
	}

	if (listen(sockfd, 10) == -1)
	{
		char ebuffer[256] = { 0 };
		WLog_ERR(TAG, "Error listening to socket at cid %d port %d: %s", addr.svm_cid, port,
		         winpr_strerror(errno, ebuffer, sizeof(ebuffer)));
		closesocket((SOCKET)sockfd);
		return FALSE;
	}
	listener->sockfds[listener->num_sockfds] = sockfd;
	listener->events[listener->num_sockfds] = WSACreateEvent();

	if (!listener->events[listener->num_sockfds])
	{
		listener->num_sockfds = 0;
	}

	WSAEventSelect(sockfd, listener->events[listener->num_sockfds], FD_READ | FD_ACCEPT | FD_CLOSE);
	listener->num_sockfds++;

	WLog_INFO(TAG, "Listening on %s:%d", bind_address, port);
	return TRUE;
#else
	WLog_ERR(TAG, "compiled without AF_VSOCK, '%s' not supported", bind_address);
	return FALSE;
#endif
}

static BOOL freerdp_listener_open(freerdp_listener* instance, const char* bind_address, UINT16 port)
{
	int ai_flags = 0;
	int status = 0;
	int sockfd = 0;
	char addr[64];
	void* sin_addr = NULL;
	int option_value = 0;
	struct addrinfo* res = NULL;
	rdpListener* listener = (rdpListener*)instance->listener;
#ifdef _WIN32
	u_long arg;
#endif

	if (!bind_address)
		ai_flags = AI_PASSIVE;

	if (utils_is_vsock(bind_address))
	{
		bind_address = utils_is_vsock(bind_address);
		return freerdp_listener_open_from_vsock(instance, bind_address, port);
	}

	res = freerdp_tcp_resolve_host(bind_address, port, ai_flags);

	if (!res)
		return FALSE;

	for (struct addrinfo* ai = res; ai && (listener->num_sockfds < 5); ai = ai->ai_next)
	{
		if ((ai->ai_family != AF_INET) && (ai->ai_family != AF_INET6))
			continue;

		if (listener->num_sockfds == MAX_LISTENER_HANDLES)
		{
			WLog_ERR(TAG, "too many listening sockets");
			continue;
		}

		sockfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);

		if (sockfd == -1)
		{
			WLog_ERR(TAG, "socket");
			continue;
		}

		option_value = 1;

		if (ai->ai_family == AF_INET)
			sin_addr = &(((struct sockaddr_in*)ai->ai_addr)->sin_addr);
		else
		{
			sin_addr = &(((struct sockaddr_in6*)ai->ai_addr)->sin6_addr);
			if (setsockopt(sockfd, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&option_value,
			               sizeof(option_value)) == -1)
				WLog_ERR(TAG, "setsockopt");
		}

		inet_ntop(ai->ai_family, sin_addr, addr, sizeof(addr));

		if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (void*)&option_value,
		               sizeof(option_value)) == -1)
			WLog_ERR(TAG, "setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR)");

#ifndef _WIN32
		if (fcntl(sockfd, F_SETFL, O_NONBLOCK) != 0)
			WLog_ERR(TAG, "fcntl(sockfd, F_SETFL, O_NONBLOCK)");
#else
		arg = 1;
		ioctlsocket(sockfd, FIONBIO, &arg);
#endif
		status = _bind((SOCKET)sockfd, ai->ai_addr, ai->ai_addrlen);

		if (status != 0)
		{
			closesocket((SOCKET)sockfd);
			continue;
		}

		status = _listen((SOCKET)sockfd, 10);

		if (status != 0)
		{
			WLog_ERR(TAG, "listen");
			closesocket((SOCKET)sockfd);
			continue;
		}

		/* FIXME: these file descriptors do not work on Windows */
		listener->sockfds[listener->num_sockfds] = sockfd;
		listener->events[listener->num_sockfds] = WSACreateEvent();

		if (!listener->events[listener->num_sockfds])
		{
			listener->num_sockfds = 0;
			break;
		}

		WSAEventSelect(sockfd, listener->events[listener->num_sockfds],
		               FD_READ | FD_ACCEPT | FD_CLOSE);
		listener->num_sockfds++;
		WLog_INFO(TAG, "Listening on [%s]:%" PRIu16, addr, port);
	}

	freeaddrinfo(res);
	return (listener->num_sockfds > 0 ? TRUE : FALSE);
}

static BOOL freerdp_listener_open_local(freerdp_listener* instance, const char* path)
{
#ifndef _WIN32
	int status = 0;
	int sockfd = 0;
	struct sockaddr_un addr = { 0 };
	rdpListener* listener = (rdpListener*)instance->listener;
	HANDLE hevent = NULL;

	if (listener->num_sockfds == MAX_LISTENER_HANDLES)
	{
		WLog_ERR(TAG, "too many listening sockets");
		return FALSE;
	}

	sockfd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (sockfd == -1)
	{
		WLog_ERR(TAG, "socket");
		return FALSE;
	}

	int rc = fcntl(sockfd, F_SETFL, O_NONBLOCK);
	if (rc != 0)
	{
		WLog_ERR(TAG, "fcntl(sockfd, F_SETFL, O_NONBLOCK)");
		closesocket((SOCKET)sockfd);
		return FALSE;
	}

	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
	unlink(path);
	status = _bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));

	if (status != 0)
	{
		WLog_ERR(TAG, "bind");
		closesocket((SOCKET)sockfd);
		return FALSE;
	}

	status = _listen(sockfd, 10);

	if (status != 0)
	{
		WLog_ERR(TAG, "listen");
		closesocket((SOCKET)sockfd);
		return FALSE;
	}

	hevent = CreateFileDescriptorEvent(NULL, FALSE, FALSE, sockfd, WINPR_FD_READ);

	if (!hevent)
	{
		WLog_ERR(TAG, "failed to create sockfd event");
		closesocket((SOCKET)sockfd);
		return FALSE;
	}

	listener->sockfds[listener->num_sockfds] = sockfd;
	listener->events[listener->num_sockfds] = hevent;
	listener->num_sockfds++;
	WLog_INFO(TAG, "Listening on socket %s.", addr.sun_path);
	return TRUE;
#else
	return TRUE;
#endif
}

static BOOL freerdp_listener_open_from_socket(freerdp_listener* instance, int fd)
{
#ifndef _WIN32
	rdpListener* listener = (rdpListener*)instance->listener;

	if (listener->num_sockfds == MAX_LISTENER_HANDLES)
	{
		WLog_ERR(TAG, "too many listening sockets");
		return FALSE;
	}

	if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
		return FALSE;

	listener->sockfds[listener->num_sockfds] = fd;
	listener->events[listener->num_sockfds] = WSACreateEvent();

	if (!listener->events[listener->num_sockfds])
		return FALSE;

	WSAEventSelect(fd, listener->events[listener->num_sockfds], FD_READ | FD_ACCEPT | FD_CLOSE);

	listener->num_sockfds++;
	WLog_INFO(TAG, "Listening on socket %d.", fd);
	return TRUE;
#else
	return FALSE;
#endif
}

static void freerdp_listener_close(freerdp_listener* instance)
{
	rdpListener* listener = (rdpListener*)instance->listener;

	for (int i = 0; i < listener->num_sockfds; i++)
	{
		closesocket((SOCKET)listener->sockfds[i]);
		(void)CloseHandle(listener->events[i]);
	}

	listener->num_sockfds = 0;
}

#if defined(WITH_FREERDP_DEPRECATED)
static BOOL freerdp_listener_get_fds(freerdp_listener* instance, void** rfds, int* rcount)
{
	rdpListener* listener = (rdpListener*)instance->listener;

	if (listener->num_sockfds < 1)
		return FALSE;

	for (int index = 0; index < listener->num_sockfds; index++)
	{
		rfds[*rcount] = (void*)(long)(listener->sockfds[index]);
		(*rcount)++;
	}

	return TRUE;
}
#endif

static DWORD freerdp_listener_get_event_handles(freerdp_listener* instance, HANDLE* events,
                                                DWORD nCount)
{
	rdpListener* listener = (rdpListener*)instance->listener;

	if (listener->num_sockfds < 1)
		return 0;

	if (listener->num_sockfds > (INT64)nCount)
		return 0;

	for (int index = 0; index < listener->num_sockfds; index++)
	{
		events[index] = listener->events[index];
	}

	return listener->num_sockfds;
}

BOOL freerdp_peer_set_local_and_hostname(freerdp_peer* client,
                                         const struct sockaddr_storage* peer_addr)
{
	const void* sin_addr = NULL;
	const BYTE localhost6_bytes[] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };

	WINPR_ASSERT(client);
	WINPR_ASSERT(peer_addr);

	if (peer_addr->ss_family == AF_INET)
	{
		const UINT32* usin_addr = sin_addr = &(((const struct sockaddr_in*)peer_addr)->sin_addr);

		if ((*usin_addr) == 0x0100007f)
			client->local = TRUE;
	}
	else if (peer_addr->ss_family == AF_INET6)
	{
		const struct sockaddr_in6* usin_addr = sin_addr =
		    &(((const struct sockaddr_in6*)peer_addr)->sin6_addr);

		if (memcmp(usin_addr, localhost6_bytes, 16) == 0)
			client->local = TRUE;
	}

#ifndef _WIN32
#if defined(HAVE_AF_VSOCK_H)
	else if (peer_addr->ss_family == AF_UNIX || peer_addr->ss_family == AF_VSOCK)
#else
	else if (peer_addr->ss_family == AF_UNIX)
#endif
		client->local = TRUE;
#endif

	if (client->local)
		WLog_INFO(TAG, "Accepting client from localhost");

	if (sin_addr)
		inet_ntop(peer_addr->ss_family, sin_addr, client->hostname, sizeof(client->hostname));

	return TRUE;
}

static BOOL freerdp_check_and_create_client(freerdp_listener* instance, int peer_sockfd,
                                            const struct sockaddr_storage* peer_addr)
{
	WINPR_ASSERT(instance);
	WINPR_ASSERT(peer_sockfd >= 0);
	WINPR_ASSERT(peer_addr);

	const BOOL check = IFCALLRESULT(TRUE, instance->CheckPeerAcceptRestrictions, instance);
	if (!check)
	{
		closesocket((SOCKET)peer_sockfd);
		return TRUE;
	}

	freerdp_peer* client = freerdp_peer_new(peer_sockfd);
	if (!client)
	{
		closesocket((SOCKET)peer_sockfd);
		return FALSE;
	}

	if (!freerdp_peer_set_local_and_hostname(client, peer_addr))
	{
		freerdp_peer_free(client);
		return FALSE;
	}

	const BOOL peer_accepted = IFCALLRESULT(FALSE, instance->PeerAccepted, instance, client);
	if (!peer_accepted)
	{
		WLog_ERR(TAG, "PeerAccepted callback failed");
		freerdp_peer_free(client);
	}

	return TRUE;
}

static BOOL freerdp_listener_check_fds(freerdp_listener* instance)
{
	rdpListener* listener = (rdpListener*)instance->listener;

	if (listener->num_sockfds < 1)
		return FALSE;

	for (int i = 0; i < listener->num_sockfds; i++)
	{
		struct sockaddr_storage peer_addr = { 0 };

		(void)WSAResetEvent(listener->events[i]);
		int peer_addr_size = sizeof(peer_addr);
		SOCKET peer_sockfd =
		    _accept(listener->sockfds[i], (struct sockaddr*)&peer_addr, &peer_addr_size);

		if (peer_sockfd == (SOCKET)-1)
		{
			char buffer[128] = { 0 };
#ifdef _WIN32
			int wsa_error = WSAGetLastError();

			/* No data available */
			if (wsa_error == WSAEWOULDBLOCK)
				continue;

#else

			if (errno == EAGAIN || errno == EWOULDBLOCK)
				continue;

#endif
			WLog_WARN(TAG, "accept failed with %s", winpr_strerror(errno, buffer, sizeof(buffer)));
			return FALSE;
		}

		if (!freerdp_check_and_create_client(instance, (int)peer_sockfd, &peer_addr))
			return FALSE;
	}

	return TRUE;
}

freerdp_listener* freerdp_listener_new(void)
{
	freerdp_listener* instance = NULL;
	rdpListener* listener = NULL;
	instance = (freerdp_listener*)calloc(1, sizeof(freerdp_listener));

	if (!instance)
		return NULL;

	instance->Open = freerdp_listener_open;
	instance->OpenLocal = freerdp_listener_open_local;
	instance->OpenFromSocket = freerdp_listener_open_from_socket;
#if defined(WITH_FREERDP_DEPRECATED)
	instance->GetFileDescriptor = freerdp_listener_get_fds;
#endif
	instance->GetEventHandles = freerdp_listener_get_event_handles;
	instance->CheckFileDescriptor = freerdp_listener_check_fds;
	instance->Close = freerdp_listener_close;
	listener = (rdpListener*)calloc(1, sizeof(rdpListener));

	if (!listener)
	{
		free(instance);
		return NULL;
	}

	listener->instance = instance;
	instance->listener = (void*)listener;
	return instance;
}

void freerdp_listener_free(freerdp_listener* instance)
{
	if (instance)
	{
		free(instance->listener);
		free(instance);
	}
}
