#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <linux/tcp.h>
#include <linux/fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <netinet/in.h>

#include "liburing.h"

#define SERVER_PORT		6000
#define SERVER_IP_ADDR	"0.0.0.0"

const char *http_reply =
	"HTTP/1.1 200 OK\r\n"
	"Content-Length: 2167737\r\n"
	"Content-Type: text/plain\r\n"
	"\r\n";

struct io_sendfile {
	int index;
	int in_fd;
	int out_fd;
	int pipe_in_fd;
	int pipe_out_fd;
	size_t offset;
	size_t len;
	size_t chunk_size;
	size_t bytes_read;
	size_t bytes_sent;
};

void die(const char *msg) {
	fprintf(stderr, "%s", msg);
	exit(EXIT_FAILURE);
}

struct io_sendfile *allocate_sendfile(int in_fd, int out_fd, size_t offset, size_t len)
{
 	struct io_sendfile *iosf;
	int pipes[2];

	iosf = (struct io_sendfile *)malloc(sizeof(struct io_sendfile));
	pipe(pipes);

	// If we try to write more at one time than pipe buffer size the chain will
	// break and -ECANCELED returned.
	int pipesize = fcntl(pipes[1], F_GETPIPE_SZ);
	printf("Pipe default buffer size: %d\n", pipesize);

	// Increase pipe buffer size to reduce number of userspace wakeups.
	if ((pipesize = fcntl(pipes[1], F_SETPIPE_SZ, 1048576)) != 1048576)
		die("Failed to increase pipe buffer size to 1048576 bytes");
	printf("Pipe buffer size increased to: %d\n", pipesize);

	iosf->index = 0;
	iosf->in_fd = in_fd;
	iosf->out_fd = out_fd;
	iosf->pipe_in_fd = pipes[1];
	iosf->pipe_out_fd = pipes[0];
	iosf->offset = offset;
	iosf->chunk_size = pipesize;
	iosf->len = len;
	iosf->bytes_read = 0;
	iosf->bytes_sent = 0;

	return iosf;
}

void free_sendfile(struct io_sendfile *iosf)
{
	close(iosf->pipe_in_fd);
	close(iosf->pipe_out_fd);
	free(iosf);
}

int do_sendfile(struct io_uring *ring, struct io_sendfile *iosf) {
	struct io_uring_cqe *cqe;
	struct io_uring_sqe *sqe;
	int ret = -1;

	// Schedule pairs of SQEs for the whole sendfile transfer.
	size_t to_send = iosf->len;
	size_t offset = 0;
	while (to_send) {
		// Calculate how much to send. Can't be bigger than pipe buffer size.
		size_t chunk = iosf->chunk_size;
		if (chunk > iosf->len - offset)
			chunk = iosf->len - offset;
		to_send -= chunk;

		// Schedule splice from file to pipe.
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "Sendfile: get sqe failed\n");
			return -1;
		}
		io_uring_prep_splice(sqe, iosf->in_fd, offset, iosf->pipe_in_fd, -1, chunk, 0);
		io_uring_sqe_set_data(sqe, iosf);
		sqe->flags |= IOSQE_IO_LINK;

		// Schedule splice from pipe to socket, linked to previous splice.
		sqe = io_uring_get_sqe(ring);
		if (!sqe) {
			fprintf(stderr, "Sendfile: get sqe failed\n");
			return -1;
		}
		io_uring_prep_splice(sqe, iosf->pipe_out_fd, -1, iosf->out_fd, -1, chunk, 0);
		io_uring_sqe_set_data(sqe, iosf);
		// Keep linking SQEs as long as we have more data to send
		if (to_send > 0)
			sqe->flags |= IOSQE_IO_LINK;
		offset += chunk;
	}

	ret = io_uring_submit(ring);
	if (ret < 0) {
		fprintf(stderr, "Sendfile: sqe submit failed: %d\n", ret);
		return -1;
	}
	fprintf(stdout, "Sendfile: submitted %d requests\n", ret);

	while (iosf->bytes_sent < iosf->len) {
		// Wait for SQE completion.
		ret = io_uring_wait_cqe(ring, &cqe);
		if (ret < 0) {
			fprintf(stderr, "Sendfile: wait completion %d\n", ret);
			return ret;
		}
		// Get our data and update counters
		struct io_sendfile *iosf = io_uring_cqe_get_data(cqe);
		iosf->index++;
		/* fprintf(stdout, "Sendfile: got completion for index %d\n", iosf->index); */
		if (cqe->res <= 0) {
			io_uring_cqe_seen(ring, cqe);
			fprintf(stderr, "Sendfile: ceq error result %d\n", cqe->res);
			return cqe->res;
		}
		if (iosf->index % 2 == 1) {
			/* fprintf(stdout, "Sendfile: ceq step 1 result %d\n", cqe->res); */
			iosf->bytes_read += cqe->res;
			iosf->offset += cqe->res;
		} else {
			/* fprintf(stdout, "Sendfile: ceq step 2 result %d\n", cqe->res); */
			iosf->bytes_sent += cqe->res;
			fprintf(stdout, "Sendfile: %lu bytes transferred\n", iosf->bytes_sent);
		}
		io_uring_cqe_seen(ring, cqe);
	}
	fprintf(stdout, "Sendfile: read from file %lu bytes, sent to socket %lu bytes\n", iosf->bytes_read, iosf->bytes_sent);
	ret = iosf->bytes_sent;
	return ret;
}

int main(int argc, char *argv[])
{
	struct io_uring ring;
	struct io_uring_params p = { };
	int ret, file_fd, listen_fd, client_fd;
	struct sockaddr_in server_addr, client_addr;
	socklen_t socklen = sizeof(client_addr);
	struct stat statbuf;

	// Get file size.
    if (stat("file.txt", &statbuf) == -1)
		die("Failed to stat file, exiting\n");
	printf("Got file size %lu\n", statbuf.st_size);

	// Open our file.
	if ((file_fd = open("file.txt", O_RDONLY)) < 1)
		die("Failed to open file, exiting\n");

	// Initialize uring.
	if ((ret = io_uring_queue_init_params(1024, &ring, &p)))
		die("Ring setup failed, exiting\n");

	// Check that we have splice support (kernel v5.7 or newer).
	if (!(p.features & IORING_FEAT_FAST_POLL))
		die("No splice support, exiting\n");
	fprintf(stdout, "Initiated uring with splice support\n");

	// Open listening socket.
	if ((listen_fd = socket(PF_INET, SOCK_STREAM, 0)) < 0)
		die("Failed to create listen socket, exiting\n");

	// Set reuse so we don't have to wait to run our program again.
	int val = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(int)))
		die("Failed to set SO_REUSEADDR, exiting\n");
		
	// Prepare server address.
	memset(&server_addr, 0, sizeof(struct sockaddr_in));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = inet_addr(SERVER_IP_ADDR);
	server_addr.sin_port = htons(SERVER_PORT);

	// Bind listening socket.
	if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in)) == -1)
		die("Failed to bind listen socket, exiting\n");

	// Start to listen.
    if (listen(listen_fd, 5))
		die("Failed to start listen socket, exiting\n");
	fprintf(stdout, "Server is waiting for client connection on port %d\n", SERVER_PORT);

	// Wait for client to connect.
	if ((client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &socklen)) < 0)
		die("Failed to accept client, exiting\n");
	fprintf(stdout, "Server accepted client connection\n");

	/* Ignore reading request from client */

    // Write http header to client to make curl happy.
	if ((ret = write(client_fd, http_reply, strlen(http_reply))) != (ssize_t)strlen(http_reply))
		die("Failed to write http reply header, exiting\n");
	fprintf(stdout, "Wrote %d bytes to client (http response header)\n", ret);

    // Do sendfile
	struct io_sendfile *iosf = allocate_sendfile(file_fd, client_fd, 0, statbuf.st_size);
	if ((ret = do_sendfile(&ring, iosf)) < 0)
		die("Sendfile failed, exiting\n");

	// Log results
	fprintf(stdout, "Server sent %lu bytes to client\n", iosf->bytes_sent);

	// Cleanup
	shutdown(client_fd, SHUT_RDWR);
	free_sendfile(iosf);
	close(listen_fd);
	close(file_fd);

	// Wait a bit before exiting
	sleep(1);

	// Exit
	io_uring_queue_exit(&ring);
	return 0;
}
