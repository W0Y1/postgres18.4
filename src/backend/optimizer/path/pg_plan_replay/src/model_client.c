/* PostgreSQL 后端与本地模型服务之间的 Unix socket 协议客户端。 */
#include "postgres.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

#include "model_client.h"

#define PLAN_REPLAY_MODEL_RESPONSE_MAX (16 * 1024 * 1024)

static void
model_set_error(char **error_message, const char *message)
{
	if (error_message != NULL)
		*error_message = pstrdup(message);
}

static int
model_wait_socket(int fd, short events, TimestampTz started, int timeout_ms)
{
	struct pollfd descriptor;
	long elapsed;
	int remaining;
	int result;

	elapsed = TimestampDifferenceMilliseconds(started, GetCurrentTimestamp());
	if (elapsed >= timeout_ms)
		return 0;
	remaining = timeout_ms - (int) elapsed;
	descriptor.fd = fd;
	descriptor.events = events;
	descriptor.revents = 0;
	do
	{
		result = poll(&descriptor, 1, remaining);
		if (result < 0 && errno == EINTR)
			CHECK_FOR_INTERRUPTS();
	} while (result < 0 && errno == EINTR);
	if (result <= 0)
		return 0;
	/* A one-request service may close after writing, yielding POLLIN|POLLHUP. */
	if ((descriptor.revents & events) != 0)
		return 1;
	if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
		return 0;
	return (descriptor.revents & events) != 0;
}

static int
model_connect(const char *socket_path, TimestampTz started, int timeout_ms)
{
	struct sockaddr_un address;
	int fd;
	int flags;
	int socket_error = 0;
	socklen_t error_length = sizeof(socket_error);

	if (socket_path == NULL || socket_path[0] == '\0' ||
		strlen(socket_path) >= sizeof(address.sun_path))
		return -1;
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		goto failed;
	MemSet(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strlcpy(address.sun_path, socket_path, sizeof(address.sun_path));
	if (connect(fd, (struct sockaddr *) &address, sizeof(address)) < 0)
	{
		if (errno != EINPROGRESS ||
			!model_wait_socket(fd, POLLOUT, started, timeout_ms))
			goto failed;
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) < 0 ||
			socket_error != 0)
			goto failed;
	}
	return fd;

failed:
	close(fd);
	return -1;
}

static bool
model_send_all(int fd, const char *data, int length,
			   TimestampTz started, int timeout_ms)
{
	int offset = 0;

	while (offset < length)
	{
		ssize_t written;

		if (!model_wait_socket(fd, POLLOUT, started, timeout_ms))
			return false;
		written = send(fd, data + offset, length - offset, MSG_NOSIGNAL);
		if (written < 0 &&
			(errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			continue;
		if (written <= 0)
			return false;
		offset += (int) written;
	}
	return true;
}

static bool
model_read_response(int fd, StringInfo response,
					TimestampTz started, int timeout_ms)
{
	char buffer[8192];

	while (response->len < PLAN_REPLAY_MODEL_RESPONSE_MAX)
	{
		ssize_t count;
		char *newline;

		if (!model_wait_socket(fd, POLLIN, started, timeout_ms))
			return false;
		count = recv(fd, buffer, sizeof(buffer), 0);
		if (count <= 0)
			return false;
		appendBinaryStringInfo(response, buffer, (int) count);
		newline = memchr(response->data, '\n', response->len);
		if (newline != NULL)
		{
			*newline = '\0';
			response->len = newline - response->data;
			return true;
		}
	}
	return false;
}

/*
 * V1 deliberately maps predictions by array order.  It has no request ID,
 * Path ID, or per-item identity checks; only the minimum shape checks needed
 * to avoid applying an incomplete or non-numeric result are retained.
 */
static bool
model_parse_costs(char *response, int expected_count, Cost *costs)
{
	char *cursor = strstr(response, "\"costs\"");
	int index;

	if (cursor == NULL || (cursor = strchr(cursor, '[')) == NULL)
		return false;
	cursor++;
	for (index = 0; index < expected_count; index++)
	{
		char *end = NULL;
		double value;

		while (isspace((unsigned char) *cursor) || *cursor == ',')
			cursor++;
		errno = 0;
		value = strtod(cursor, &end);
		if (end == cursor || errno != 0 || !isfinite(value) || value <= 0)
			return false;
		costs[index] = (Cost) value;
		cursor = end;
	}
	while (isspace((unsigned char) *cursor))
		cursor++;
	return *cursor == ']';
}

bool
plan_replay_model_predict(const char *socket_path, int timeout_ms,
						  const char *request, int request_length,
						  int expected_count, Cost *costs,
						  char **error_message)
{
	StringInfoData response;
	TimestampTz started = GetCurrentTimestamp();
	int fd = -1;
	bool ok = false;

	if (error_message != NULL)
		*error_message = NULL;
	initStringInfo(&response);
	fd = model_connect(socket_path, started, timeout_ms);
	if (fd < 0)
	{
		model_set_error(error_message, "could not connect to model service");
		goto done;
	}
	if (!model_send_all(fd, request, request_length, started, timeout_ms))
	{
		model_set_error(error_message, "could not send model request before timeout");
		goto done;
	}
	if (!model_read_response(fd, &response, started, timeout_ms))
	{
		model_set_error(error_message, "could not read model response before timeout");
		goto done;
	}
	if (!model_parse_costs(response.data, expected_count, costs))
	{
		model_set_error(error_message, "model response does not contain the expected ordered costs array");
		goto done;
	}
	ok = true;

done:
	if (fd >= 0)
		close(fd);
	pfree(response.data);
	return ok;
}
