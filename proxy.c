/*
 * proxy.c - HTTP Proxy
 *
 * TEAM MEMBERS:
 *     Gabríel Arthúr Pétursson <gabriel13@ru.is>
 *     Gunnar Guðvarðarson <gunnargud13@ru.is>
 *     Kristján Árni Gerhardsson <kristjan14@ru.is>
 */

/*
 * DOCUMENTATION FOR ALL THE THINGS:
 * http://beej.us/guide/bgnet/output/html/multipage/index.html
 * http://www.hea.net/share/ipv6-programming/
 */

#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include "csapp.h"

#define MAX(x, y) ((x) > (y) ? (x) : (y))

/* do not accept headers larger than MAX_HEADER_SIZE */
static const ssize_t MAX_HEADER_SIZE = 16384;

struct string
{
	size_t len;
	const char *str;
};

struct proxy_connection
{
	struct proxy_log *log;
	struct sockaddr_storage client_addr;
	struct sockaddr_storage server_addr;
	socklen_t client_len;
	socklen_t server_len;
	int client_fd;
	int server_fd;
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

	log->file = stdout;
}

static void
proxy_log_free (struct proxy_log *log)
{
	assert (log);

	pthread_mutex_destroy (&log->mutex);
}

/*
 * Adds a request to the log
 *
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), and the size of the response
 * from the server in bytes (size).
 */
static void
proxy_log_add (struct proxy_log *log, const struct sockaddr *sa, socklen_t salen, struct string url, size_t size)
{
	char strtime[32];
	char address[INET6_ADDRSTRLEN];

	assert (log);
	assert (sa);
	assert (salen > 0);

	proxy_log_get_time (strtime, sizeof (strtime));
	proxy_log_get_address (sa, salen, address, sizeof (address));

	fprintf (log->file, "%s: [%s] %.*s %zu\n", strtime, address, (int) url.len, url.str, size);
}

/* A simplified URL parser. Gives host, path and port. */
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

/* Is the HTTP header complete, and if so, what is its size? */
static size_t
proxy_header_complete (const char *header, size_t size)
{
	char *end = memmem (header, size, "\r\n\r\n", 4);

	/* the header is incomplete */
	if (end == NULL)
		return 0;

	return end + 4 - header;
}

/* Extracts a single line from the header and advances the header pointer */
static bool
proxy_header_line (const char **header, size_t *size, const char **line, size_t *line_size)
{
	const char *end = memmem (*header, *size, "\r\n", 2);

	if (end == NULL || end == *header)
		return false;

	*line = *header;
	*line_size = end - *header + 2;
	*header += *line_size;
	*size -= *line_size;

	return true;
}

/* Parses the initial HTTP head entry */
static bool
proxy_header_parse_head (
	const char *line,
	size_t size,
	enum http_method *method,
	const char **location,
	size_t *location_size,
	const char **version,
	size_t *version_size)
{
	const char *space;
	const char *methods[] = {
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

	for (size_t i = 0; i < sizeof (methods) / sizeof (*methods); ++i)
	{
		size_t len = strlen (methods[i]);

		/* terminating \r\n and a space */
		if (size <= len + 3)
			continue;

		/* have we found the correct method? */
		if (memcmp (methods[i], line, len) == 0)
		{
			/* the following byte should be a space */
			if (line[len] != ' ')
				return false;

			/* find the next space */
			space = memchr (&line[len + 1], ' ', size - len - 1);

			if (space == NULL)
				return false;

			*method = i;
			*location = &line[len + 1];
			*location_size = space - *location;
			*version = space + 1;
			*version_size = &line[size - 2] - *version;

			return true;
		}
	}

	return false;
}

/* Working thread */
static void *
proxy_client_work (void *ud)
{
	assert (ud);

	struct proxy_connection *conn = ud;
	ssize_t bytes;
	ssize_t status;
	size_t header_size;
	char buffer[MAX_HEADER_SIZE];

	struct addrinfo hints;
	struct addrinfo *res = NULL;
	struct addrinfo *aip = NULL;
	int gai;

	bytes = 0;

	/* receive until we have the header */
	while (proxy_header_complete (buffer, bytes) == 0 && bytes < sizeof (buffer))
	{
		status = recv (conn->client_fd, buffer + bytes, sizeof (buffer) - bytes, 0);

		if (status == -1)
		{
			perror ("recv");

			goto error;
		}

		/* client disconnected */
		if (status == 0)
			goto error;

		bytes += status;
	}

	header_size = proxy_header_complete (buffer, bytes);

	/* the header was not found */
	if (header_size == 0)
		goto error;

	const char *headptr = &buffer[0];
	const char *line;
	size_t line_size;

	if ( ! proxy_header_line (&headptr, &header_size, &line, &line_size))
		goto error;

	enum http_method method;
	const char *location;
	size_t location_size;
	const char *version;
	size_t version_size;

	if ( ! proxy_header_parse_head (line, line_size, &method, &location, &location_size, &version, &version_size))
		goto error;

	struct string url = { location_size, location };
	struct string host;
	struct string path;
	struct string port;

	if ( ! proxy_url_parse (url, &host, &path, &port))
		goto error;

	/*printf ("--->%.*s<---\n", (int) url.len, url.str);
	printf ("--->%.*s<---\n", (int) host.len, host.str);
	printf ("--->%.*s<---\n", (int) path.len, path.str);
	printf ("--->%.*s<---\n", (int) port.len, port.str);*/

	/* lookup the host's addresses */
	memset (&hints, 0, sizeof (hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;

	{
		char str_host[host.len + 1];
		char str_port[port.len + 1];

		memcpy (str_host, host.str, host.len);
		memcpy (str_port, port.str, port.len);

		str_host[host.len] = 0;
		str_port[port.len] = 0;

		gai = getaddrinfo (str_host, str_port, &hints, &res);
	}

	if (gai != 0)
	{
		fprintf (stderr, "getaddrinfo: %s\n", gai_strerror (gai));
		goto error;
	}

	/* try all addresses until one succeeds */
	for (aip = res; aip; aip = aip->ai_next)
	{
		// char str[1234];
		// proxy_log_get_address (aip->ai_addr, aip->ai_addrlen, str, sizeof (str));

		conn->server_fd = socket (aip->ai_family, aip->ai_socktype, aip->ai_protocol);

		// printf ("!!!!-> %s (%d)\n", str, conn->server_fd);

		if (conn->server_fd == -1)
		{
			/* family and protocol not supported */
			if (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT)
				continue;

			perror ("socket");

			goto error;
		}
		else
		{
			if (connect (conn->server_fd, aip->ai_addr, aip->ai_addrlen) == 0)
			{
				memcpy (&conn->server_addr, aip->ai_addr, aip->ai_addrlen);
				conn->server_len = aip->ai_addrlen;

				break;
			}

			perror ("connect");

			goto error;
		}
	}

	/* send what we have so far to the server */
	ssize_t sent_bytes = 0;

	while (sent_bytes < bytes)
	{
		status = send (conn->server_fd, buffer + sent_bytes, bytes - sent_bytes, MSG_MORE | MSG_NOSIGNAL);

		if (status == -1)
		{
			perror ("send");

			goto error;
		}

		sent_bytes += status;
	}

	/* now we multiplex both sockets */
	while (true)
	{
		char buffer[MAX_HEADER_SIZE];

		fd_set rset;
		FD_ZERO (&rset);
		FD_SET (conn->server_fd, &rset);
		FD_SET (conn->client_fd, &rset);

		fd_set wset;
		FD_ZERO (&wset);
		FD_SET (conn->server_fd, &wset);
		FD_SET (conn->client_fd, &wset);

		if (select(MAX (conn->server_fd, conn->client_fd) + 1, &rset, &wset, NULL, NULL)  == -1)
		{
			perror("select");

			goto error;
		}

		/* receive data from client and send to server */
		if (FD_ISSET (conn->client_fd, &rset) && FD_ISSET (conn->server_fd, &wset))
		{
			bytes = recv (conn->client_fd, buffer, sizeof (buffer), 0);

			if (bytes == -1)
			{
				perror ("recv");

				goto error;
			}

			if (bytes == 0)
				break;

			sent_bytes = 0;

			/* TODO stop blocking! */
			while (sent_bytes < bytes)
			{
				status = send (conn->server_fd, buffer + sent_bytes, bytes - sent_bytes, MSG_MORE | MSG_NOSIGNAL);

				if (status == -1)
				{
					perror ("send");

					goto error;
				}

				sent_bytes += status;
			}
		}

		/* receive data from server and send to client */
		if (FD_ISSET (conn->server_fd, &rset) && FD_ISSET (conn->client_fd, &wset))
		{
			bytes = recv (conn->server_fd, buffer, sizeof (buffer), 0);

			if (bytes == -1)
			{
				perror ("recv");

				goto error;
			}

			if (bytes == 0)
				break;

			sent_bytes = 0;

			/* TODO stop blocking! */
			while (sent_bytes < bytes)
			{
				status = send (conn->client_fd, buffer + sent_bytes, bytes - sent_bytes, MSG_MORE | MSG_NOSIGNAL);

				if (status == -1)
				{
					perror ("send");

					goto error;
				}

				sent_bytes += status;
			}
		}
	}

	proxy_log_add (
		conn->log,
		(struct sockaddr *) &conn->client_addr,
		conn->client_len,
		url,
		0
	);

error:
	if (res)
	{
		freeaddrinfo (res);
	}

	if (conn->client_fd != -1 && close (conn->client_fd) == -1)
	{
		perror ("close");
	}

	if (conn->server_fd != -1 && close (conn->server_fd) == -1)
	{
		perror ("close");
	}

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

	/* Eliminates "Address already in use" error from bind. */
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

	conn->log = &proxy->log;
	conn->client_len = sizeof (conn->client_addr);
	conn->client_fd = accept (proxy->socket, (struct sockaddr *) &conn->client_addr, &conn->client_len);
	conn->server_fd = -1;

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

