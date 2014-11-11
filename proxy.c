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
 */

#include <assert.h>
#include <stdbool.h>

#define MAX_DOMAIN_LENGTH 253
#define MAX_PORT_LENGTH 5
#define DEFAULT_PORT "80"

int listenFd;
int listenPort;

/*
 * Function prototypes
 */
int parse_uri(char *, char *, char *, int  *);
void format_log_entry(char *, struct sockaddr_in *, char *, int);
void processConnection();
void processRequest(int, int *);
void closeConnections(int, int);
ssize_t receiveMessage(int, char *);
void sendMessage(int, char *);
bool getHost(char *, char *, char* );

/*
 * main - Main routine for the proxy program
 */
int main(int argc, char **argv)
{
	if (argc != 2)
	{
		fprintf(stderr, "Usage: %s <port number>\n", argv[0]);
		exit(0);
	}

	listenPort = atoi(argv[1]);

	printf("Server starting, listen port: %d\n", listenPort);

	if ((listenFd = open_listenfd(listenPort)) < 0) /* TODO ipv6 */
	{
		perror("open_listenfd");
		exit(1);
	}

	printf("Server successfully started\n");

	while(1)
	{
		processConnection();
	}

	exit(0);
}

/* Block until a connection is established */
void processConnection()
{
	int client = -1;
	int server = -1;
	struct sockaddr_in clientaddr;
	socklen_t addrlen;

	memset(&clientaddr, 0, sizeof(clientaddr));

	addrlen = sizeof(clientaddr);

	if ((client = accept(listenFd, (struct sockaddr *) &clientaddr, &addrlen)) < 0)
	{
		perror("accept");
		return;
	}

	/* TODO fork */

	printf("Client connected, address: %s\n", inet_ntoa(clientaddr.sin_addr));

	processRequest(client, &server);
	closeConnections(client, server);
}

/* Block while a client is connected */
void processRequest(int client, int *server)
{
	struct addrinfo *res;
	int receivedBytes;
	char host[MAX_DOMAIN_LENGTH], port[MAX_PORT_LENGTH]; /* getHost() */
	char message[MAXLINE]; /* receiveMessage() */

	do
	{
		if ((receivedBytes = receiveMessage(client, message)) == -1)
			return; /* already perror()d */

		if (receivedBytes == 0) /* Client disconnected */
			return;

		if ( ! getHost(message, host, port))
			continue;
	}
	while (receivedBytes == -2);

	printf("Host: %s:%s\n", host, port);

	/* get the address */
	if (getaddrinfo(host, port, NULL, &res) != 0)
	{
		perror("getaddrinfo");
		return;
	}

	/* make a socket */
	if ((*server = socket(res->ai_family, res->ai_socktype, res->ai_protocol)) < 0)
	{
		perror("socket");
		return;
	}

	/* connect! */
	if ((connect(*server, res->ai_addr, res->ai_addrlen)) < 0)
	{
		perror("connect");
		return;
	}
	
	printf("serverFD: %d\n", *server);
	sendMessage(*server, message);

	do
	{
		if ((receivedBytes = receiveMessage(*server, message)) > 0)
			sendMessage(client, message);
			
		/* check if the server has something for us */
		if ((receivedBytes = receiveMessage(client, message)) > 0)
			sendMessage(*server, message); /* pass message on (NO PARSING) */
	}
	while (receivedBytes > 0); /* 0 == client disconnected */
}

ssize_t receiveMessage(int fd, char *message)
{
	ssize_t lineLength;
	struct timeval timeOut;
	fd_set set;

	FD_ZERO(&set);
	FD_SET(fd, &set);

	memset(&timeOut, 0, sizeof(timeOut));
	timeOut.tv_sec = 0;
	timeOut.tv_usec = 500000;

	if (select(fd + 1, &set, NULL, NULL, &timeOut) < 0)
	{
		perror("select");
		return -1;
	}

	if ( ! FD_ISSET(fd, &set))
		return -2; /* not ready to receive */

	if ((lineLength = recv(fd, message, MAXLINE, 0)) < 0)
	{
		perror("recv");
		return -1;
	}

	printf("Received: (%.*s) %lu\n", (int) lineLength, message, (unsigned long int) lineLength);
	fflush(stdout);

	return lineLength;
}

void sendMessage(int fd, char *message)
{
	int messageLength, bytesSent = 0;

	do /* keep sending until it's all gone... */
	{
		messageLength = strlen(message);

		if ((bytesSent += send(fd, message, messageLength, 0)) < 0)
		{
			perror("send");
			return; /* TODO return false ? */
		}

		message += bytesSent;
		printf("Sent: (%s) %d\n", message, bytesSent);
		fflush(stdout);
	}
	while (messageLength);
}

bool getHost(char *line, char *host, char* port)
{
	char *end;

	while (1)
	{
		if (!strncasecmp(line, "host: ", 6))
		{
			line = line + 6;
			break;
		}
		else
		{
			if ((line = strstr(line, "\r\n")))
				line += 2;
			else
				return false;
		}
	}

	/* Extract the host name and port */
	if (*(end = strpbrk(line, ":\r")) == '\r')
		strcpy(port, DEFAULT_PORT);
	else
	{
		size_t portLen = (size_t)(strpbrk(line, "\r") - end - 1);
		strncpy(port, end + 1, portLen);
		port[(int)portLen] = 0;
	}
	
	strncpy(host, line, (size_t)(end - line));
	host[(int)(end - line)] = 0;

	return true;
}

void closeConnections(int client, int server)
{
	close(client);
	close(server);
}

/*
 * parse_uri - URI parser
 *
 * Given a URI from an HTTP proxy GET request (i.e., a URL), extract
 * the host name, path name, and port.  The memory for hostname and
 * pathname must already be allocated and should be at least MAXLINE
 * bytes. Return -1 if there are any problems.
 */
int parse_uri(char *uri, char *hostname, char *pathname, int *port)
{
	char *hostbegin;
	char *hostend;
	char *pathbegin;
	int len;

	if (strncasecmp(uri, "http://", 7) != 0)
	{
		hostname[0] = '\0';
		return -1;
	}

	/* Extract the host name */
	hostbegin = uri + 7;
	hostend = strpbrk(hostbegin, " :/\r\n\0");
	len = hostend - hostbegin;
	strncpy(hostname, hostbegin, len);
	hostname[len] = '\0';

	/* Extract the port number */
	*port = 80; /* default */
	
	if (*hostend == ':')
		*port = atoi(hostend + 1);

	/* Extract the path */
	pathbegin = strchr(hostbegin, '/');
	if (pathbegin == NULL)
	{
		pathname[0] = '\0';
	}
	else
	{
		pathbegin++;
		strcpy(pathname, pathbegin);
	}

	return 0;
}

/*
 * format_log_entry - Create a formatted log entry in logstring.
 *
 * The inputs are the socket address of the requesting client
 * (sockaddr), the URI from the request (uri), and the size in bytes
 * of the response from the server (size).
 */
void format_log_entry(char *logstring, struct sockaddr_in *sockaddr, char *uri, int size)
{
	time_t now;
	char time_str[MAXLINE];
	unsigned long host;
	unsigned char a, b, c, d;

	/* Get a formatted time string */
	now = time(NULL);
	strftime(time_str, MAXLINE, "%a %d %b %Y %H:%M:%S %Z", localtime(&now));

	/*
	 * Convert the IP address in network byte order to dotted decimal
	 * form. Note that we could have used inet_ntoa, but chose not to
	 * because inet_ntoa is a Class 3 thread unsafe function that
	 * returns a pointer to a static variable (Ch 13, CS:APP).
	 */
	host = ntohl(sockaddr->sin_addr.s_addr);
	a = host >> 24;
	b = (host >> 16) & 0xff;
	c = (host >> 8) & 0xff;
	d = host & 0xff;

	/* Return the formatted log entry string */
	sprintf(logstring, "%s: %d.%d.%d.%d %s", time_str, a, b, c, d, uri);
}

