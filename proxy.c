/*
 * proxy.c - HTTP Proxy
 *
 * This program depends on ISO 9899:1999. If using gcc, compile with -std=c99
 *
 * TEAM MEMBERS:
 *     Gabríel Arthúr Pétursson <gabriel13@ru.is>
 *     Gunnar Guðvarðarson <gunnargud13@ru.is>
 *     Kristján Árni Gerhardsson <kristjan14@ru.is>
 *
 * When the proxy receives a new connection, it accepts it and spawns a new
 * thread to process it.
 *
 * The thread then waits until a whole HTTP header has been received. It is
 * then processed and forwarded to the server (unless it's a CONNECT request,
 * they are treated differently).
 *
 * The client and server sockets are monitored for data. Once data arrives,
 * it is stored in one of the appropriate buffers and later processed.
 *
 * The buffers are ring (circular) buffers. Data can be moved between them.
 * Data can also be inserted directly into them, such as when sending custom
 * HTTP header entities.
 *
 * This proxy fully supports IPv6.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#define BUFFER_SIZE (16384)

struct string
{
	size_t len;
	const char *str;
};

struct mutable_string
{
	size_t len;
	char *str;
};

/* a ring buffer */
struct proxy_buffer
{
	size_t size;
	size_t pos;
	char buf[BUFFER_SIZE];
};

struct proxy_connection
{
	struct proxy_log *log;

	/* network addresses */
	struct sockaddr_storage client_addr;
	struct sockaddr_storage server_addr;
	socklen_t client_len;
	socklen_t server_len;

	/* file descriptors */
	int client_fd;
	int server_fd;

	/* proxy state */
	uint64_t client_rx;
	bool client_connected;
	bool server_connected;
	bool server_expect_header; /* is a header expected? */

	/* client buffers */
	struct proxy_buffer crecv_buffer;
	struct proxy_buffer csend_buffer;

	/* server buffers */
	struct proxy_buffer srecv_buffer;
	struct proxy_buffer ssend_buffer;
};

struct proxy_log
{
	pthread_mutex_t mutex;
	FILE *file;
};

struct proxy
{
	struct proxy_log log;
	pthread_attr_t attr;
	int socket;
};

enum http_method
{
	OPTIONS,
	GET,
	HEAD,
	POST,
	PUT,
	DELETE,
	TRACE,
	CONNECT,
	PATCH,
};

static const char *str_http_method[] = {
	"OPTIONS",
	"GET",
	"HEAD",
	"POST",
	"PUT",
	"DELETE",
	"TRACE",
	"CONNECT",
	"PATCH",
};

/* string functions */
static int proxy_string_compare (const struct string pstr, const char *cstr);

/* log functions */
static void proxy_log_get_time (char *strtime, size_t length);
static void proxy_log_get_address (const struct sockaddr *sa, socklen_t salen, char *addr, size_t addrlen);
static void proxy_log_init (struct proxy_log *log);
static void proxy_log_free (struct proxy_log *log);
static void proxy_log_add (struct proxy_log *log, const struct sockaddr *sa, socklen_t salen, struct string url, uint64_t size);

/* url functions */
static bool proxy_url_parse (const struct string url, struct string *restrict host, struct string *restrict path, struct string *restrict port);
static bool proxy_url_parse_plain (const struct string url, struct string *restrict host, struct string *restrict port);

/* buffer functions */
static bool proxy_buffer_add (struct proxy_buffer *buffer, const struct string data);
static bool proxy_buffer_add_cstr (struct proxy_buffer *buffer, const char *str);
static size_t proxy_buffer_available (const struct proxy_buffer *buffer);
static bool proxy_buffer_commit (struct proxy_buffer *buffer, size_t bytes);
static size_t proxy_buffer_move (struct proxy_buffer *dest, struct proxy_buffer *src, size_t bytes);
static struct string proxy_buffer_read_consecutive (const struct proxy_buffer *buffer);
static bool proxy_buffer_remove (struct proxy_buffer *buffer, size_t bytes);
static void proxy_buffer_shift (struct proxy_buffer *buffer);
static size_t proxy_buffer_size (struct proxy_buffer *buffer);
static struct mutable_string proxy_buffer_write_consecutive (struct proxy_buffer *buffer);

/* header functions */
static size_t proxy_header_complete (struct proxy_buffer *buffer);
static bool proxy_header_line (struct string *header, struct string *line);
static bool proxy_header_parse_head (const struct string line, enum http_method *method, struct string *location, struct string *version);
static bool proxy_header_parse_line (const struct string line, struct string *key, struct string *value);

/* client functions */
static int proxy_client_connect (const struct string node, const struct string service, struct sockaddr_storage *addr, socklen_t *addrlen);
static bool proxy_client_process (struct proxy_connection *conn);
static void *proxy_client_work (void *ud);

/* proxy functions */
static int proxy_init (struct proxy *proxy, int port);
static void proxy_free (struct proxy *proxy);
static void proxy_accept (struct proxy *proxy);

/* main function */
int main (int argc, char **argv);

/* Compares a Proxy string with a C string in a case-insensitive manner */
static int
proxy_string_compare (const struct string pstr, const char *cstr)
{
	size_t clen;
	int cmp;

	assert (pstr.str);
	assert (cstr);

	clen = strlen (cstr);

	cmp = strncasecmp (pstr.str, cstr, MIN (pstr.len, clen));

	if (cmp != 0)
		return cmp;

	if (pstr.len < clen)
		return -1;

	if (pstr.len > clen)
		return 1;

	return 0;
}

/*
 * Gets and formats the current time
 *
 * If the current time cannot be represented, or 'length' is of insufficient
 * size, it is filled with the string "(NULL)".
 */
static void
proxy_log_get_time (char *strtime, size_t length)
{
	time_t t;
	struct tm tm;

	assert (strtime);
	assert (length > 0);

	t = time (NULL);

	if (gmtime_r (&t, &tm) == NULL)
	{
		strncpy (strtime, "(NULL)", length);
		strtime[length - 1] = 0;
	}
	else
	{
		if (strftime (strtime, length, "%FT%TZ", &tm) == 0)
		{
			strncpy (strtime, "(NULL)", length);
			strtime[length - 1] = 0;
		}
	}
}

/*
 * Formats the IP address 'sa' into human-friendly form
 *
 * If the address cannot be formatted, or 'addrlen' is of insufficient size,
 * it is filled with the string "(NULL)".
 */
static void
proxy_log_get_address (const struct sockaddr *sa, socklen_t salen, char *addr, size_t addrlen)
{
	int status;

	assert (sa);
	assert (salen > 0);
	assert (addr);
	assert (addrlen > 0);

	status = getnameinfo (sa, salen, addr, addrlen, NULL, 0, NI_NUMERICHOST | NI_NUMERICSERV);

	if (status != 0)
	{
		fprintf (stderr, "getnameinfo: %s\n", gai_strerror(status));

		strncpy (addr, "(NULL)", addrlen);
		addr[addrlen - 1] = 0;
	}
}

/* Initializes the log struct */
static void
proxy_log_init (struct proxy_log *log)
{
	assert (log);

	pthread_mutex_init (&log->mutex, NULL);

	log->file = fopen ("proxy.log", "ab");

	if (log->file == NULL)
	{
		perror ("fopen");

		log->file = stdout;
	}
}

/* Destroy the log struct */
static void
proxy_log_free (struct proxy_log *log)
{
	assert (log);

	pthread_mutex_destroy (&log->mutex);

	if (fclose (log->file) != 0)
	{
		perror ("fclose");
	}
}

/*
 * Adds a request to the log
 *
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), and the size of the response
 * from the server in bytes (size).
 */
static void
proxy_log_add (struct proxy_log *log, const struct sockaddr *sa, socklen_t salen, struct string url, uint64_t size)
{
	char strtime[32];
	char address[INET6_ADDRSTRLEN];

	assert (log);
	assert (sa);
	assert (salen > 0);
	assert (url.str);

	proxy_log_get_time (strtime, sizeof (strtime));
	proxy_log_get_address (sa, salen, address, sizeof (address));

	pthread_mutex_lock (&log->mutex);

	fprintf (log->file, "%s: [%s] %.*s %" PRIu64 "\n", strtime, address, (int) url.len, url.str, size);
	fflush (log->file);

	pthread_mutex_unlock (&log->mutex);
}

/*
 * A simplified URL parser. Gives host, path and port.
 */
static bool
proxy_url_parse (
	const struct string url,
	struct string *restrict host,
	struct string *restrict path,
	struct string *restrict port)
{
	const char *slash;
	const char *colon;

	assert (url.str);
	assert (host);
	assert (path);
	assert (port);

	/* prefix + leading slash */
	if (url.len < 7 + 1)
		return false;

	if (memcmp (url.str, "http://", 7) != 0)
		return false;

	/* find the leading slash */
	slash = memchr (&url.str[7], '/', url.len - 7);

	if (slash == NULL)
		return false;

	/* find the last colon in the host */
	colon = memrchr (&url.str[7], ':', slash - &url.str[7]);

	/* no colon means we do not need to do further processing */
	if (colon == NULL)
	{
		host->len = slash - &url.str[7];
		host->str = &url.str[7];

		port->len = 2;
		port->str = "80";
	}
	else
	{
		/* do we have an IPv6 address? */
		if (url.str[7] == '[')
		{
			/* does the colon immediately follow the IPv6 address? */
			if (colon[-1] == ']')
			{
				host->len = colon - &url.str[7] - 2;
				host->str = &url.str[8];

				port->len = slash - colon - 1;
				port->str = colon + 1;
			}
			else
			{
				/* matching closing bracket */
				if (slash[-1] != ']')
					return false;

				host->len = slash - &url.str[7] - 2;
				host->str = &url.str[8];

				port->len = 2;
				port->str = "80";
			}
		}
		else
		{
			host->len = colon - &url.str[7];
			host->str = &url.str[7];

			port->len = slash - colon - 1;
			port->str = colon + 1;
		}
	}

	path->len = url.len - (slash - url.str);
	path->str = slash;

	return true;
}

/* A more simplistic URL parser. Parses simple host:port strings. */
static bool
proxy_url_parse_plain (
	const struct string url,
	struct string *host,
	struct string *port)
{
	const char *colon;

	assert (url.str);
	assert (host);
	assert (port);

	/* find the colon */
	colon = memchr (url.str, ':', url.len);

	if (colon == NULL)
		return false;

	host->len = colon - url.str;
	host->str = url.str;

	port->len = &url.str[url.len] - colon - 1;
	port->str = colon + 1;

	if (host->len == 0 || port->len == 0)
		return false;

	/* do we have an IPv6 address? */
	if (host->str[0] == '[')
	{
		/* matching closing bracket */
		if (host->str[host->len - 1] != ']')
			return false;

		host->len -= 2;
		host->str += 1;
	}

	return true;
}

/* Append data to the buffer */
static bool
proxy_buffer_add (struct proxy_buffer *buffer, const struct string data)
{
	size_t length;

	assert (buffer);
	assert (data.str);

	/* do we have sufficient storage space? */
	if (data.len + buffer->size > sizeof (buffer->buf))
		return false;

	length = MIN (data.len, sizeof (buffer->buf) - buffer->pos);

	memcpy (&buffer->buf[buffer->pos], data.str, length);
	memcpy (buffer->buf, &data.str[length], data.len - length);

	buffer->pos += data.len;
	buffer->pos %= sizeof (buffer->buf);
	buffer->size += data.len;

	return true;
}

/* Append C string to the buffer */
static bool
proxy_buffer_add_cstr (struct proxy_buffer *buffer, const char *str)
{
	size_t length;

	assert (str);

	length = strlen (str);

	return proxy_buffer_add (buffer, (struct string) { length, str });
}

/* Number of bytes available for addition to the buffer */
static size_t
proxy_buffer_available (const struct proxy_buffer *buffer)
{
	assert (buffer);

	return sizeof (buffer->buf) - buffer->size;
}

/*
 * Adds 'bytes' bytes to the size of buffer, assuming the data is already there
 *
 * Generally used in combination with 'proxy_buffer_write_consecutive'.
 */
static bool
proxy_buffer_commit (struct proxy_buffer *buffer, size_t bytes)
{
	assert (buffer);

	if (buffer->size + bytes > sizeof (buffer->buf))
		return false;

	buffer->size += bytes;
	buffer->pos += bytes;
	buffer->pos %= sizeof (buffer->buf);

	return true;
}

/*
 * Moves 'bytes' bytes from source to destination buffer.
 *
 * If less than 'bytes' bytes are available in the source, or less than 'bytes'
 * bytes fit in the destination, the number of remaining bytes is returned
 * after the copy.
 */
static size_t
proxy_buffer_move (struct proxy_buffer *dest, struct proxy_buffer *src, size_t bytes)
{
	size_t length;
	struct string src_data;
	struct mutable_string dest_data;

	assert (dest);
	assert (src);

	while (true)
	{
		src_data = proxy_buffer_read_consecutive (src);
		dest_data = proxy_buffer_write_consecutive (dest);

		length = MIN (bytes, MIN (dest_data.len, src_data.len));

		if (length == 0)
			break;

		memcpy (dest_data.str, src_data.str, length);

		proxy_buffer_remove (src, length);
		proxy_buffer_commit (dest, length);

		bytes -= length;
	}

	return bytes;
}

/*
 * Returns the entire buffer, but stops early if it's not consecutive
 *
 * The contents of the returned string are invalidated as soon as any other
 * buffer operations are performed on the buffer.
 *
 * Generally used in combination with 'proxy_buffer_remove'.
 */
static struct string
proxy_buffer_read_consecutive (const struct proxy_buffer *buffer)
{
	assert (buffer);

	size_t begin = (buffer->pos + sizeof (buffer->buf) - buffer->size) % sizeof (buffer->buf);

	return (struct string) {
		MIN (buffer->size, sizeof (buffer->buf) - begin),
		&buffer->buf[begin]
	};
}

/* Remove bytes from the beginning of the buffer */
static bool
proxy_buffer_remove (struct proxy_buffer *buffer, size_t bytes)
{
	assert (buffer);

	if (bytes > buffer->size)
		return false;

	buffer->size -= bytes;

	return true;
}

/* Shift the buffer so its first byte begins physically first in memory */
static void
proxy_buffer_shift (struct proxy_buffer *buffer)
{
	size_t begin;
	size_t length;
	char aux[sizeof (buffer->buf)];

	assert (buffer);

	/* already shifted */
	if (buffer->pos % sizeof (buffer->buf) == buffer->size)
		return;

	begin = (buffer->pos + sizeof (buffer->buf) - buffer->size) % sizeof (buffer->buf);
	length = MIN (buffer->size, sizeof (buffer->buf) - begin);

	memcpy (aux, &buffer->buf[begin], length);
	memcpy (&aux[length], buffer->buf, buffer->size - length);
	memcpy (buffer->buf, aux, buffer->size);

	buffer->pos = buffer->size;
	buffer->pos %= sizeof (buffer->buf);
}

/* Number of bytes in the buffer */
static size_t
proxy_buffer_size (struct proxy_buffer *buffer)
{
	assert (buffer);

	return buffer->size;
}

/*
 * Returns a memory region located past the end of the current position in the
 * buffer, suitable for appending data.
 *
 * When finished writing, call 'proxy_buffer_commit' with the number of bytes
 * written.
 */
static struct mutable_string
proxy_buffer_write_consecutive (struct proxy_buffer *buffer)
{
	assert (buffer);

	return (struct mutable_string) {
		MIN (sizeof (buffer->buf) - buffer->size , sizeof (buffer->buf) - buffer->pos),
		&buffer->buf[buffer->pos],
	};
}

/* Is the HTTP header complete, and if so, what is its size? */
static size_t
proxy_header_complete (struct proxy_buffer *buffer)
{
	struct string data;

	assert (buffer);

	proxy_buffer_shift (buffer);
	data = proxy_buffer_read_consecutive (buffer);

	char *end = memmem (data.str, data.len, "\r\n\r\n", 4);

	/* the header is incomplete */
	if (end == NULL)
		return 0;

	return end + 4 - data.str;
}

/* Extracts a single line from the header and advances the header pointer */
static bool
proxy_header_line (struct string *header, struct string *line)
{
	assert (header);
	assert (header->str);
	assert (line);

	const char *end = memmem (header->str, header->len, "\r\n", 2);

	if (end == NULL)
		return false;

	line->len = end - header->str + 2;
	line->str = header->str;
	header->str += line->len;
	header->len -= line->len;

	return true;
}

/* Parses the initial HTTP head entry */
static bool
proxy_header_parse_head (
	const struct string line,
	enum http_method *method,
	struct string *location,
	struct string *version)
{
	const char *space;

	assert (line.str);
	assert (method);
	assert (location);
	assert (version);

	for (size_t i = 0; i < sizeof (str_http_method) / sizeof (*str_http_method); ++i)
	{
		size_t len = strlen (str_http_method[i]);

		/* terminating \r\n and a space */
		if (line.len <= len + 3)
			continue;

		/* have we found the correct method? */
		if (memcmp (str_http_method[i], line.str, len) == 0)
		{
			/* the following byte should be a space */
			if (line.str[len] != ' ')
				return false;

			/* find the next space */
			space = memchr (&line.str[len + 1], ' ', line.len - len - 1);

			if (space == NULL)
				return false;

			*method = i;
			location->len = space - &line.str[len + 1];
			location->str = &line.str[len + 1];
			version->len = &line.str[line.len - 2] - (space + 1);
			version->str = space + 1;

			return true;
		}
	}

	return false;
}

/*
 * Parses a header line to get the key and value of that line
*/
static bool
proxy_header_parse_line (
	const struct string line,
	struct string *key,
	struct string *value)
{
	char *colon;

	assert (line.str);
	assert (key);
	assert (value);

	/* key starts at start of line.str but ends at ':' */
	colon = memchr (line.str, ':', line.len);

	if (colon == NULL)
		return false; /* no ':', header line is invalid */

	key->len = colon - line.str;
	key->str = line.str;

	if (colon[1] != ' ')
		return false;

	/* value starts 1 characters after the ':' and ends before the '\r' */
	value->len = &line.str[line.len] - colon - 4;
	value->str = colon + 2;

	return true;
}

/*
 * Resolve and connect to the given node and service
 */
static int
proxy_client_connect (
	const struct string node,
	const struct string service,
	struct sockaddr_storage *addr,
	socklen_t *addrlen)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *aip = NULL;
	int sock = -1;
	int gai;

	char str_node[node.len + 1];
	char str_service[service.len + 1];

	assert (node.str);
	assert (service.str);
	assert (addr);
	assert (addrlen);

	memset (&hints, 0, sizeof (hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;

	memcpy (str_node, node.str, node.len);
	memcpy (str_service, service.str, service.len);

	str_node[node.len] = 0;
	str_service[service.len] = 0;

	gai = getaddrinfo (str_node, str_service, &hints, &res);

	if (gai != 0)
	{
		fprintf (stderr, "getaddrinfo: %s\n", gai_strerror (gai));
		fprintf (stderr, "name: %zu '%s'\n", node.len, str_node);
		fprintf (stderr, "service: %zu '%s'\n", service.len, str_service);

		return -1;
	}

	/* try all addresses until one succeeds */
	for (aip = res; aip; aip = aip->ai_next)
	{
		sock = socket (aip->ai_family, aip->ai_socktype, aip->ai_protocol);

		if (sock == -1)
		{
			/* family and protocol not supported */
			if (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT)
				continue;

			perror ("socket");

			break;
		}
		else
		{
			if (connect (sock, aip->ai_addr, aip->ai_addrlen) == 0)
			{
				memcpy (addr, aip->ai_addr, aip->ai_addrlen);
				*addrlen = aip->ai_addrlen;

				break;
			}

			perror ("connect");

			if (close (sock) == -1)
			{
				perror ("close");
			}

			sock = -1;

			break;
		}
	}

	freeaddrinfo (res);

	return sock;
}

/*
 * Examines the buffers and performs the appropriate computation on them,
 * such as passing data along.
 */
static bool
proxy_client_process (struct proxy_connection *conn)
{
	size_t header_size;

	assert (conn);

	/* client to server */
	proxy_buffer_move (&conn->ssend_buffer, &conn->crecv_buffer, BUFFER_SIZE);

	if (conn->server_expect_header)
	{
		header_size = proxy_header_complete (&conn->srecv_buffer);

		/* header is incomplete */
		if (header_size == 0)
			return proxy_buffer_available (&conn->srecv_buffer) > 0;

		/* pass the headers on, mostly unmodified */
		if (proxy_buffer_move (&conn->csend_buffer, &conn->srecv_buffer, header_size - 2) != 0)
			return false;

		/* we do not support keep-alives */
		if ( ! proxy_buffer_add_cstr (&conn->csend_buffer, "Proxy-Connection: close\r\n"))
			return false;

		conn->server_expect_header = false;
	}
	else
	{
		/* server to client */
		proxy_buffer_move (&conn->csend_buffer, &conn->srecv_buffer, BUFFER_SIZE);
	}

	return true;
}

/* Working thread */
static void *
proxy_client_work (void *ud)
{
	/* connection */
	struct proxy_connection *conn = ud;

	/* header parsing */
	size_t header_size;
	struct string header;
	struct string line;

	/* header information */
	enum http_method method;
	struct string location;
	struct string version;
	struct string host;
	struct string path;
	struct string port;
	char *mem = NULL;

	struct string key;
	struct string value;

	/* multiplexing */
	fd_set rset;
	fd_set wset;

	assert (ud);

	/* receive until we have the header */
	while (proxy_header_complete (&conn->crecv_buffer) == 0 &&
	       proxy_buffer_available (&conn->crecv_buffer) > 0)
	{
		struct mutable_string data = proxy_buffer_write_consecutive (&conn->crecv_buffer);
		ssize_t bytes_read = recv (conn->client_fd, data.str, data.len, 0);

		if (bytes_read == -1)
		{
			perror ("recv");

			goto error;
		}

		/* client disconnected */
		if (bytes_read == 0)
			goto error;

		proxy_buffer_commit (&conn->crecv_buffer, bytes_read);
	}

	header_size = proxy_header_complete (&conn->crecv_buffer);

	/* the header was not found */
	if (header_size == 0)
		goto error;

	header = proxy_buffer_read_consecutive (&conn->crecv_buffer);
	header.len = header_size;

	if ( ! proxy_header_line (&header, &line))
		goto error;

	if ( ! proxy_header_parse_head (line, &method, &location, &version))
		goto error;

	/* we only support HTTP/1.1 clients */
	if (proxy_string_compare (version, "HTTP/1.1") != 0)
		goto error;

	/* CONNECT requests are essentially plain TCP tunnels */
	if (method == CONNECT)
	{
		if ( ! proxy_url_parse_plain (location, &host, &port))
			goto error;

		conn->server_expect_header = false;
	}
	else
	{
		if ( ! proxy_url_parse (location, &host, &path, &port))
			goto error;

		/* send the request */
		proxy_buffer_add_cstr (&conn->ssend_buffer, str_http_method[method]);
		proxy_buffer_add_cstr (&conn->ssend_buffer, " ");
		proxy_buffer_add (&conn->ssend_buffer, path);
		proxy_buffer_add_cstr (&conn->ssend_buffer, " ");
		proxy_buffer_add (&conn->ssend_buffer, version);
		proxy_buffer_add_cstr (&conn->ssend_buffer, "\r\n");

		/* we do not want a keep-alive session with the server */
		proxy_buffer_add_cstr (&conn->ssend_buffer, "Connection: close\r\n");

		while (true)
		{
			if ( ! proxy_header_line (&header, &line))
				goto error;

			/* end of header */
			if (line.len == 2)
				break;

			if ( ! proxy_header_parse_line (line, &key, &value))
				goto error;

			if (proxy_string_compare (key, "Proxy-Connection") == 0)
			{
				/* drop this header */
			}
			else if (proxy_string_compare (key, "Connection") == 0)
			{
				/* drop this header */
			}
			else
			{
				/* forward the header */
				proxy_buffer_add (&conn->ssend_buffer, key);
				proxy_buffer_add_cstr (&conn->ssend_buffer, ": ");
				proxy_buffer_add (&conn->ssend_buffer, value);
				proxy_buffer_add_cstr (&conn->ssend_buffer, "\r\n");
			}
		}

		/* end of header */
		proxy_buffer_add_cstr (&conn->ssend_buffer, "\r\n");
	}

	/* move the location out of the client buffer, we need it later and it
	 * will get overwritten if left in the buffer */
	mem = malloc (location.len);

	if (mem == NULL)
		goto error;

	memcpy (mem, location.str, location.len);
	location.str = mem;

	/* remove the header from the receive buffer */
	proxy_buffer_remove (&conn->crecv_buffer, header_size);

	conn->server_fd = proxy_client_connect (host, port, &conn->server_addr, &conn->server_len);

	if (conn->server_fd == -1)
		goto error;

	conn->server_connected = true;

	if (method == CONNECT)
	{
		proxy_buffer_add_cstr (&conn->csend_buffer, "HTTP/1.1 200 Connection established\r\n");
		proxy_buffer_add_cstr (&conn->csend_buffer, "Proxy-Connection: close\r\n");
		proxy_buffer_add_cstr (&conn->csend_buffer, "Connection: close\r\n");
		proxy_buffer_add_cstr (&conn->csend_buffer, "\r\n");
	}

	/* multiplex both sockets */
	while (true)
	{
		FD_ZERO (&rset);
		FD_ZERO (&wset);

		/* if the server is connected, client isn't, there's no point in
		 * continuing. */
		if (conn->server_connected && ! conn->client_connected)
			break;

		/* if the client is connected, server isn't, and we have nothing to
		 * send to the client, disconnect it. */
		if (conn->client_connected && ! conn->server_connected &&
		    proxy_buffer_size (&conn->csend_buffer) == 0)
			break;

		if (conn->server_connected && proxy_buffer_available (&conn->srecv_buffer) > 0)
		{
			FD_SET (conn->server_fd, &rset);
		}

		if (conn->client_connected && proxy_buffer_available (&conn->crecv_buffer) > 0)
		{
			FD_SET (conn->client_fd, &rset);
		}

		if (conn->server_connected && proxy_buffer_size (&conn->ssend_buffer) > 0)
		{
			FD_SET (conn->server_fd, &wset);
		}

		if (conn->client_connected && proxy_buffer_size (&conn->csend_buffer) > 0)
		{
			FD_SET (conn->client_fd, &wset);
		}

		if ( ! FD_ISSET (conn->server_fd, &rset) &&
		     ! FD_ISSET (conn->client_fd, &rset) &&
		     ! FD_ISSET (conn->server_fd, &wset) &&
		     ! FD_ISSET (conn->client_fd, &wset))
			break;

		if (select(MAX (conn->server_fd, conn->client_fd) + 1, &rset, &wset, NULL, NULL)  == -1)
		{
			perror("select");

			goto error;
		}

		if (conn->server_connected && FD_ISSET (conn->server_fd, &rset))
		{
			struct mutable_string data = proxy_buffer_write_consecutive (&conn->srecv_buffer);
			ssize_t bytes_read = recv (conn->server_fd, data.str, data.len, 0);

			if (bytes_read == -1)
			{
				perror ("recv");

				goto error;
			}

			if (bytes_read == 0)
			{
				conn->server_connected = false;
			}

			conn->client_rx += bytes_read;
			proxy_buffer_commit (&conn->srecv_buffer, bytes_read);
		}

		if (conn->client_connected && FD_ISSET (conn->client_fd, &rset))
		{
			struct mutable_string data = proxy_buffer_write_consecutive (&conn->crecv_buffer);
			ssize_t bytes_read = recv (conn->client_fd, data.str, data.len, 0);

			if (bytes_read == -1)
			{
				perror ("recv");

				goto error;
			}

			if (bytes_read == 0)
			{
				conn->client_connected = false;
			}

			proxy_buffer_commit (&conn->crecv_buffer, bytes_read);
		}

		if (conn->server_connected && FD_ISSET (conn->server_fd, &wset))
		{
			struct string data = proxy_buffer_read_consecutive (&conn->ssend_buffer);
			ssize_t bytes_sent = send (conn->server_fd, data.str, data.len, 0);

			if (bytes_sent == -1)
			{
				perror ("send");

				goto error;
			}

			proxy_buffer_remove (&conn->ssend_buffer, bytes_sent);
		}

		if (conn->client_connected && FD_ISSET (conn->client_fd, &wset))
		{
			struct string data = proxy_buffer_read_consecutive (&conn->csend_buffer);
			ssize_t bytes_sent = send (conn->client_fd, data.str, data.len, 0);

			if (bytes_sent == -1)
			{
				perror ("send");

				goto error;
			}

			proxy_buffer_remove (&conn->csend_buffer, bytes_sent);
		}

		if ( ! proxy_client_process (conn))
			goto error;
	}

	proxy_log_add (
		conn->log,
		(struct sockaddr *) &conn->client_addr,
		conn->client_len,
		location,
		conn->client_rx
	);

error:
	if (conn->client_fd != -1 && close (conn->client_fd) == -1)
	{
		perror ("close");
	}

	if (conn->server_fd != -1 && close (conn->server_fd) == -1)
	{
		perror ("close");
	}

	free (mem);
	free (conn);

	return NULL;
}

/*
 * Initializes the proxy
 *
 * Sets up the proxy and a listening socket, making the proxy ready to accept
 * connections.
 */
static int
proxy_init (struct proxy *proxy, int port)
{
	struct sockaddr_in6 addr;

	proxy_log_init (&proxy->log);

	if (pthread_attr_init (&proxy->attr) != 0)
	{
		proxy_log_free (&proxy->log);

		return -1;
	}

	if (pthread_attr_setdetachstate (&proxy->attr, PTHREAD_CREATE_DETACHED) != 0)
	{
		proxy_log_free (&proxy->log);

		if (pthread_attr_destroy (&proxy->attr) != 0)
		{
			fprintf (stderr, "pthread_attr_destroy failed\n");
		}

		return -1;
	}

	proxy->socket = socket (PF_INET6, SOCK_STREAM, 0);

	if (proxy->socket == -1)
	{
		perror ("socket");

		proxy_log_free (&proxy->log);

		if (pthread_attr_destroy (&proxy->attr) != 0)
		{
			fprintf (stderr, "pthread_attr_destroy failed\n");
		}

		return -1;
	}

	/* eliminate "address already in use" error from bind */
	int optval = 1;
	if (setsockopt (proxy->socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof (optval)) == -1)
	{
		perror("setsockopt");

		proxy_log_free (&proxy->log);

		if (pthread_attr_destroy (&proxy->attr) != 0)
		{
			fprintf (stderr, "pthread_attr_destroy failed\n");
		}

		if (close (proxy->socket) == -1)
		{
			perror ("close");
		}

		return -1;
	}

	memset (&addr, 0, sizeof (addr));

	addr.sin6_family = AF_INET6;
	addr.sin6_port = htons (port);
	addr.sin6_addr = in6addr_any;

	if (bind (proxy->socket, (struct sockaddr *) &addr, sizeof (addr)) == -1)
	{
		perror ("bind");

		proxy_log_free (&proxy->log);

		if (pthread_attr_destroy (&proxy->attr) != 0)
		{
			fprintf (stderr, "pthread_attr_destroy failed\n");
		}

		if (close (proxy->socket) == -1)
		{
			perror ("close");
		}

		return -1;
	}

	if (listen (proxy->socket, SOMAXCONN) == -1)
	{
		perror ("listen");

		proxy_log_free (&proxy->log);

		if (pthread_attr_destroy (&proxy->attr) != 0)
		{
			fprintf (stderr, "pthread_attr_destroy failed\n");
		}

		if (close (proxy->socket) == -1)
		{
			perror ("close");
		}

		return -1;
	}

	return 0;
}

/* Terminates the proxy */
static void
proxy_free (struct proxy *proxy)
{
	proxy_log_free (&proxy->log);

	if (pthread_attr_destroy (&proxy->attr) != 0)
	{
		fprintf (stderr, "pthread_attr_destroy failed\n");
	}

	if (close (proxy->socket) == -1)
	{
		perror ("close");
	}
}

/* Accepts a single connection and spawns a thread to process it */
static void
proxy_accept (struct proxy *proxy)
{
	struct proxy_connection *conn;
	pthread_t client_thread;

	conn = malloc (sizeof (*conn));

	if (conn == NULL)
	{
		perror ("malloc");

		return;
	}

	memset (conn, 0, sizeof (*conn));

	conn->log = &proxy->log;
	conn->client_len = sizeof (conn->client_addr);
	conn->client_fd = accept (proxy->socket, (struct sockaddr *) &conn->client_addr, &conn->client_len);
	conn->server_fd = -1;
	conn->client_connected = true;
	conn->server_expect_header = true;

	if (conn->client_fd == -1)
	{
		perror ("accept");

		free (conn);

		return;
	}

	if (pthread_create (&client_thread, &proxy->attr, proxy_client_work, conn) != 0)
	{
		fprintf (stderr, "pthread_create failed\n");

		if (close (conn->client_fd) == -1)
		{
			perror ("close");

			return;
		}

		free (conn);
	}
}

int main(int argc, char **argv)
{
	struct proxy proxy;

	/* Check arguments */
	if (argc != 2)
	{
		fprintf (stderr, "Usage: %s <port number>\n", argv[0]);

		return EXIT_FAILURE;
	}

	if (proxy_init (&proxy, atoi (argv[1])) != 0)
	{
		return EXIT_FAILURE;
	}

	while (1)
	{
		proxy_accept (&proxy);
	}

	proxy_free (&proxy);

	return EXIT_SUCCESS;
}

