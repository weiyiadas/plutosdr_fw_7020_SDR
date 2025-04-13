// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2014 Analog Devices, Inc.
 * Author: Paul Cercueil <paul.cercueil@analog.com>
 */

#include "../debug.h"
#include "../iio.h"
#include "../iio-config.h"
#include "dns-sd.h"
#include "ops.h"
#include "thread-pool.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#if WITH_ZSTD
#include <zstd.h>
#endif

#define MY_NAME "iiod"

#define _STRINGIFY(x) #x
#define STRINGIFY(x) _STRINGIFY(x)

#ifdef HAVE_IPV6
#define IP_ADDR_LEN (INET6_ADDRSTRLEN + 1 + IF_NAMESIZE)
#else
#define IP_ADDR_LEN (INET_ADDRSTRLEN + 1 + IF_NAMESIZE)
#endif

static int start_iiod(const char *uri, const char *ffs_mountpoint,
		      const char *uart_params, bool debug, bool interactive,
		      bool use_aio, uint16_t port, unsigned int nb_pipes,
		      int ep0_fd);

struct client_data {
	int fd;
	bool debug;
	struct iio_context *ctx;
	const void *xml_zstd;
	size_t xml_zstd_len;
};

bool server_demux;

struct thread_pool *main_thread_pool;


static struct sockaddr_in sockaddr = {
	.sin_family = AF_INET,
};

#ifdef HAVE_IPV6
static struct sockaddr_in6 sockaddr6 = {
	.sin6_family = AF_INET6,
	.sin6_addr = IN6ADDR_ANY_INIT,
};
#endif /* HAVE_IPV6 */

static const struct option options[] = {
	  {"help", no_argument, 0, 'h'},
	  {"version", no_argument, 0, 'V'},
	  {"debug", no_argument, 0, 'd'},
	  {"demux", no_argument, 0, 'D'},
	  {"interactive", no_argument, 0, 'i'},
	  {"aio", no_argument, 0, 'a'},
	  {"ffs", required_argument, 0, 'F'},
	  {"nb-pipes", required_argument, 0, 'n'},
	  {"serial", required_argument, 0, 's'},
	  {"port", required_argument, 0, 'p'},
	  {"uri", required_argument, 0, 'u'},
	  {0, 0, 0, 0},
};

static const char *options_descriptions[] = {
	"Show this help and quit.",
	"Display the version of this program.",
	"Use alternative (incompatible) debug interface.",
	"Demux channels directly on the server.",
	"Run " MY_NAME " in the controlling terminal.",
	"Use asynchronous I/O.",
	"Use the given FunctionFS mountpoint to serve over USB",
	"Specify the number of USB pipes (ep couples) to use",
	"Run " MY_NAME " on the specified UART.",
	"Port to listen on (default = " STRINGIFY(IIOD_PORT) ").",
	("Use the context at the provided URI."
		"\n\t\t\teg: 'ip:192.168.2.1', 'ip:pluto.local', or 'ip:'"
		"\n\t\t\t    'usb:1.2.3', or 'usb:'"
		"\n\t\t\t    'serial:/dev/ttyUSB0,115200,8n1'"
		"\n\t\t\t    'local:' (default)"),
};

static void usage(void)
{
	unsigned int i;

	printf("Usage:\n\t" MY_NAME " [OPTIONS ...]\n\nOptions:\n");
	for (i = 0; options[i].name; i++)
		printf("\t-%c, --%s\n\t\t\t%s\n",
					options[i].val, options[i].name,
					options_descriptions[i]);
}

static void client_thd(struct thread_pool *pool, void *d)
{
	struct client_data *cdata = d;

	interpreter(cdata->ctx, cdata->fd, cdata->fd, cdata->debug,
			true, false, false, pool,
			cdata->xml_zstd, cdata->xml_zstd_len);

	IIO_INFO("Client exited\n");
	close(cdata->fd);
	free(cdata);
}

static void set_handler(int signal, void (*handler)(int))
{
	struct sigaction sig;
	sigaction(signal, NULL, &sig);
	sig.sa_handler = handler;
	sigaction(signal, &sig, NULL);
}

static void sig_handler(int sig)
{
	thread_pool_stop(main_thread_pool);
}

static bool restart_usr1;

static void sig_handler_usr1(int sig)
{
	restart_usr1 = true;
	thread_pool_stop(main_thread_pool);
}

static int main_interactive(struct iio_context *ctx, bool verbose, bool use_aio,
			    const void *xml_zstd, size_t xml_zstd_len)
{
	int flags;

	if (!use_aio) {
		flags = fcntl(STDIN_FILENO, F_GETFL);
		if (flags >= 0)
			flags = fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
		if (flags < 0) {
			char err_str[1024];
			iio_strerror(errno, err_str, sizeof(err_str));
			IIO_ERROR("Could not get/set O_NONBLOCK on STDIN_FILENO"
					" %s\n", err_str);
		}

		flags = fcntl(STDOUT_FILENO, F_GETFL);
		if (flags >= 0)
			flags = fcntl(STDOUT_FILENO, F_SETFL, flags | O_NONBLOCK);
		if (flags < 0) {
			char err_str[1024];
			iio_strerror(errno, err_str, sizeof(err_str));
			IIO_ERROR("Could not get/set O_NONBLOCK on STDOUT_FILENO"
					" %s\n", err_str);
		}
	}

	interpreter(ctx, STDIN_FILENO, STDOUT_FILENO, verbose,
			false, false, use_aio, main_thread_pool,
			xml_zstd, xml_zstd_len);
	return EXIT_SUCCESS;
}

static int main_server(struct iio_context *ctx, bool debug,
		       const void *xml_zstd, size_t xml_zstd_len,
		       uint16_t port)
{
	int ret, fd = -1, yes = 1,
	    keepalive_time = 10,
	    keepalive_intvl = 10,
	    keepalive_probes = 6;
	struct pollfd pfd[2];
	char err_str[1024];
	bool ipv6;

	IIO_INFO("Starting IIO Daemon version %u.%u.%s\n",
			LIBIIO_VERSION_MAJOR, LIBIIO_VERSION_MINOR,
			LIBIIO_VERSION_GIT);

	sockaddr.sin_port = htons(port);
	sockaddr.sin_addr.s_addr = htonl(INADDR_ANY);

#ifdef HAVE_IPV6
	sockaddr6.sin6_port = htons(port);

	fd = socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
#endif
	ipv6 = (fd >= 0);
	if (!ipv6)
		fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		iio_strerror(errno, err_str, sizeof(err_str));
		IIO_ERROR("Unable to create socket: %s\n", err_str);
		return EXIT_FAILURE;
	}

	ret = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	if (ret < 0) {
		iio_strerror(errno, err_str, sizeof(err_str));
		IIO_WARNING("setsockopt SO_REUSEADDR : %s\n", err_str);
	}

#ifdef HAVE_IPV6
	if (ipv6)
		ret = bind(fd, (struct sockaddr *) &sockaddr6,
				sizeof(sockaddr6));
#endif
	if (!ipv6)
		ret = bind(fd, (struct sockaddr *) &sockaddr, sizeof(sockaddr));
	if (ret < 0) {
		iio_strerror(errno, err_str, sizeof(err_str));
		IIO_ERROR("Bind failed: %s\n", err_str);
		goto err_close_socket;
	}

	/* if port == 0, the OS will return something in the ephemeral port range
	 * which we need to find, to pass to avahi
	 */
	if (!port) {
		struct sockaddr_in sin;
		socklen_t len = sizeof(sin);
		if (getsockname(fd, (struct sockaddr *)&sin, &len) == -1) {
			iio_strerror(errno, err_str, sizeof(err_str));
			IIO_ERROR("getsockname failed : %s\n", err_str);
			goto err_close_socket;
		}
		port = ntohs(sin.sin_port);
		/* we don't use sockaddr or sockaddr6 anymore, so ignore those */
	}

	if (ipv6)
		IIO_INFO("IPv6 support enabled\n");

	if (listen(fd, 16) < 0) {
		iio_strerror(errno, err_str, sizeof(err_str));
		IIO_ERROR("Unable to mark as passive socket: %s\n", err_str);
		goto err_close_socket;
	}

	if (HAVE_AVAHI)
		start_avahi(main_thread_pool, port);

	pfd[0].fd = fd;
	pfd[0].events = POLLIN;
	pfd[0].revents = 0;
	pfd[1].fd = thread_pool_get_poll_fd(main_thread_pool);
	pfd[1].events = POLLIN;
	pfd[1].revents = 0;

	while (true) {
		struct client_data *cdata;
#ifdef HAVE_IPV6
		struct sockaddr_in6 caddr;
#else
		struct sockaddr_in caddr;
#endif

		socklen_t addr_len = sizeof(caddr);
		int new;

		poll_nointr(pfd, 2);

		if (pfd[1].revents & POLLIN) /* STOP event */
			break;

		new = accept4(fd, (struct sockaddr *) &caddr, &addr_len,
			SOCK_NONBLOCK);
		if (new == -1) {
			if (errno == EAGAIN || errno == EINTR)
				continue;
			iio_strerror(errno, err_str, sizeof(err_str));
			IIO_ERROR("Failed to create connection socket: %s\n",
				err_str);
			continue;
		}

		cdata = malloc(sizeof(*cdata));
		if (!cdata) {
			IIO_WARNING("Unable to allocate memory for client\n");
			close(new);
			continue;
		}

		/* Configure the socket to send keep-alive packets every 10s,
		 * and disconnect the client if no reply was received for one
		 * minute. */
		ret = setsockopt(new, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
		if (ret < 0) {
			iio_strerror(errno, err_str, sizeof(err_str));
			IIO_WARNING("setsockopt SO_KEEPALIVE : %s", err_str);
		}
		ret = setsockopt(new, IPPROTO_TCP, TCP_KEEPCNT, &keepalive_probes,
				sizeof(keepalive_probes));
		if (ret < 0) {
			iio_strerror(errno, err_str, sizeof(err_str));
			IIO_WARNING("setsockopt TCP_KEEPCNT : %s", err_str);
		}
		ret = setsockopt(new, IPPROTO_TCP, TCP_KEEPIDLE, &keepalive_time,
				sizeof(keepalive_time));
		if (ret < 0) {
			iio_strerror(errno, err_str, sizeof(err_str));
			IIO_WARNING("setsockopt TCP_KEEPIDLE : %s", err_str);
		}
		ret = setsockopt(new, IPPROTO_TCP, TCP_KEEPINTVL, &keepalive_intvl,
				sizeof(keepalive_intvl));
		if (ret < 0) {
			iio_strerror(errno, err_str, sizeof(err_str));
			IIO_WARNING("setsockopt TCP_KEEPINTVL : %s", err_str);
		}
		ret = setsockopt(new, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
		if (ret < 0) {
			iio_strerror(errno, err_str, sizeof(err_str));
			IIO_WARNING("setsockopt TCP_NODELAY : %s", err_str);
		}

		cdata->fd = new;
		cdata->ctx = ctx;
		cdata->debug = debug;
		cdata->xml_zstd = xml_zstd;
		cdata->xml_zstd_len = xml_zstd_len;

		if (LOG_LEVEL >= Info_L) {
			struct sockaddr_in *caddr4 = (struct sockaddr_in *)&caddr;
			char ipaddr[IP_ADDR_LEN];
			int zone = 0;
			void *addr;
			char *ptr;

			if (!ipv6 || caddr4->sin_family == AF_INET) {
				addr = &caddr4->sin_addr;
#ifdef HAVE_IPV6
			} else {
				addr = &caddr.sin6_addr;
				zone = caddr.sin6_scope_id;
#endif
			}

			if (!inet_ntop(caddr4->sin_family, addr, ipaddr, sizeof(ipaddr) - 1)) {
				iio_strerror(errno, err_str, sizeof(err_str));
				IIO_ERROR("Error during inet_ntop: %s\n", err_str);
			} else {
				ipaddr[IP_ADDR_LEN - 1] = '\0';

				if (zone) {
					ptr = &ipaddr[strnlen(ipaddr, IP_ADDR_LEN)];

					if (if_indextoname(zone, ptr + 1))
						*ptr = '%';
				}

				if (!strncmp(ipaddr, "::ffff:", sizeof("::ffff:") - 1))
					ptr = &ipaddr[sizeof("::ffff:") - 1];
				else
					ptr = ipaddr;

				IIO_INFO("New client connected from %s\n", ptr);
			}
		}

		ret = thread_pool_add_thread(main_thread_pool, client_thd, cdata, "net_client_thd");
		if (ret) {
			iio_strerror(ret, err_str, sizeof(err_str));
			IIO_ERROR("Failed to create new client thread: %s\n",
				err_str);
			close(new);
			free(cdata);
		}
	}

	IIO_DEBUG("Cleaning up\n");
	if (HAVE_AVAHI)
		stop_avahi();
	close(fd);
	return EXIT_SUCCESS;

err_close_socket:
	close(fd);
	return EXIT_FAILURE;
}

static void *get_xml_zstd_data(const struct iio_context *ctx, size_t *out_len)
{
#if WITH_ZSTD
	const char *xml = iio_context_get_xml(ctx);
	size_t len, xml_len = strlen(xml);
	void *buf;
	size_t ret;

	len = ZSTD_compressBound(xml_len);
	buf = malloc(len);
	if (!buf)
		return NULL;

	ret = ZSTD_compress(buf, len, xml, xml_len, 3);
	if (ZSTD_isError(ret)) {
		IIO_WARNING("Unable to compress XML string: %s\n",
			    ZSTD_getErrorName(xml_len));
		free(buf);
		return NULL;
	}

	*out_len = ret;

	return buf;
#else
	return NULL;
#endif
}

int main(int argc, char **argv)
{
	bool debug = false, interactive = false, use_aio = false;
	long nb_pipes = 3, val;
	char *end;
	const char *arg = "local:";
	int c, option_index = 0;
	char *ffs_mountpoint = NULL;
	char *uart_params = NULL;
	char err_str[1024];
	uint16_t port = IIOD_PORT;
	int ret, ep0_fd = 0;

	while ((c = getopt_long(argc, argv, "+hVdDiaF:n:s:p:u:",
					options, &option_index)) != -1) {
		switch (c) {
		case 'd':
			debug = true;
			break;
		case 'D':
			server_demux = true;
			break;
		case 'i':
			interactive = true;
			break;
		case 'a':
			if (!WITH_AIO) {
				IIO_ERROR("IIOD was not compiled with AIO support.\n");
				return EXIT_FAILURE;
			}

			use_aio = true;
			break;
		case 'F':
			if (!WITH_IIOD_USBD) {
				IIO_ERROR("IIOD was not compiled with USB support.\n");
				return EXIT_FAILURE;
			}

			ffs_mountpoint = optarg;
			break;
		case 'n':
			if (!WITH_IIOD_USBD) {
				IIO_ERROR("IIOD was not compiled with USB support.\n");
				return EXIT_FAILURE;
			}

			errno = 0;
			nb_pipes = strtol(optarg, &end, 10);
			if (optarg == end || nb_pipes < 1 || errno == ERANGE) {
				IIO_ERROR("--nb-pipes: Invalid parameter\n");
				return EXIT_FAILURE;
			}
			break;
		case 's':
			if (!WITH_IIOD_SERIAL) {
				IIO_ERROR("IIOD was not compiled with serial support.\n");
				return EXIT_FAILURE;

			}

			uart_params = optarg;
			break;
		case 'p':
			val = strtoul(optarg, &end, 10);
			if (optarg == end || (end && *end != '\0') || val > 0xFFFF || val < 0) {
				IIO_ERROR("IIOD invalid port number\n");
				return EXIT_FAILURE;
			}
			port = (uint16_t)val;
			break;
		case 'u':
			arg = optarg;
			break;
		case 'h':
			usage();
			return EXIT_SUCCESS;
		case 'V':
			printf("%u.%u\n", LIBIIO_VERSION_MAJOR,
					LIBIIO_VERSION_MINOR);
			return EXIT_SUCCESS;
		case '?':
			return EXIT_FAILURE;
		}
	}

	main_thread_pool = thread_pool_new();
	if (!main_thread_pool) {
		iio_strerror(errno, err_str, sizeof(err_str));
		IIO_ERROR("Unable to create thread pool: %s\n", err_str);
		return EXIT_FAILURE;
	}

	if (WITH_IIOD_USBD && ffs_mountpoint) {
		ret = init_usb_daemon(ffs_mountpoint, nb_pipes);
		if (ret < 0) {
			iio_strerror(errno, err_str, sizeof(err_str));
			IIO_ERROR("Unable to init USB: %s\n", err_str);

			thread_pool_destroy(main_thread_pool);
			return EXIT_FAILURE;
		}

		ep0_fd = ret;
	}

	set_handler(SIGHUP, sig_handler);
	set_handler(SIGPIPE, sig_handler);
	set_handler(SIGINT, sig_handler);
	set_handler(SIGTERM, sig_handler);
	set_handler(SIGUSR1, sig_handler_usr1);

	do {
		thread_pool_restart(main_thread_pool);
		restart_usr1 = false;

		ret = start_iiod(arg, ffs_mountpoint, uart_params, debug,
				 interactive, use_aio, port, nb_pipes, ep0_fd);
	} while (!ret && restart_usr1);

	thread_pool_destroy(main_thread_pool);

	if (WITH_IIOD_USBD && ffs_mountpoint)
		close(ep0_fd);

	return ret;
}

static int start_iiod(const char *uri, const char *ffs_mountpoint,
		      const char *uart_params, bool debug, bool interactive,
		      bool use_aio, uint16_t port, unsigned int nb_pipes,
		      int ep0_fd)
{
	struct iio_context *ctx;
	char err_str[1024];
	void *xml_zstd;
	size_t xml_zstd_len = 0;
	int ret;

	ctx = iio_create_context_from_uri(uri);
	if (!ctx) {
		iio_strerror(errno, err_str, sizeof(err_str));
		IIO_ERROR("Unable to create local context: %s\n", err_str);
		return EXIT_FAILURE;
	}

	xml_zstd = get_xml_zstd_data(ctx, &xml_zstd_len);

	if (WITH_IIOD_USBD && ffs_mountpoint) {
		/* We pass use_aio == true directly, this is ensured to be true
		 * by the CMake script. */
		ret = start_usb_daemon(ctx, ffs_mountpoint,
				debug, true, (unsigned int) nb_pipes, ep0_fd,
				main_thread_pool, xml_zstd, xml_zstd_len);
		if (ret) {
			iio_strerror(-ret, err_str, sizeof(err_str));
			IIO_ERROR("Unable to start USB daemon: %s\n", err_str);
			ret = EXIT_FAILURE;
			goto out_free_xml_data;
		}
	}

	if (WITH_IIOD_SERIAL && uart_params) {
		ret = start_serial_daemon(ctx, uart_params,
					  debug, main_thread_pool,
					  xml_zstd, xml_zstd_len);
		if (ret) {
			iio_strerror(-ret, err_str, sizeof(err_str));
			IIO_ERROR("Unable to start serial daemon: %s\n", err_str);
			ret = EXIT_FAILURE;
			goto out_thread_pool_stop;
		}
	}

	if (interactive)
		ret = main_interactive(ctx, debug, use_aio, xml_zstd, xml_zstd_len);
	else
		ret = main_server(ctx, debug, xml_zstd, xml_zstd_len, port);

out_thread_pool_stop:
	/*
	 * In case we got here through an error in the main thread make sure all
	 * the worker threads are signaled to shutdown.
	 */
	thread_pool_stop_and_wait(main_thread_pool);
out_free_xml_data:
	free(xml_zstd);
	iio_context_destroy(ctx);

	return ret;
}
