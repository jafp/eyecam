
#include "common.h"
#include "broadcast.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define	PORT 				24000
#define SOCKET_ERROR        -1
#define BUFFER_SIZE         100
#define QUEUE_SIZE          5

typedef struct broadcast_packet {
	int l_x, l_y, u_x, u_y;
	int error_lower;
	int error_upper;
	int mass;
	unsigned char * frame;
} broadcast_packet_t;

/**
 * Mutex for locking access to the frame buffer
 */
static pthread_mutex_t frame_buffer_mtx;

/**
 * Condition variable for signaling frame ready
 */
static pthread_cond_t frame_buffer_cv;

/**
 * Server thread
 */
static pthread_t thread;

/**
 * Structure with data to broadcast.
 */
static broadcast_packet_t packet;


static int port;
static int socket_fd;
static int server_socket_fd; 
static int addr_size = sizeof(struct sockaddr_in);
static struct hostent * host;   
static struct sockaddr_in addr; 

/**
 * Signal handler for catching SIGPIPEs and friends.
 * Whenever the connection to the client is broken (the client closes the viewer),
 * and we are in the middle of writing to the socket, a SIGPIPE signal is raised.
 */
static void signal_handler(int signal)
{
	if (signal == SIGPIPE)
	{
		printf("[broadcast] Connection to client lost - too bad\n");
	}
	else
	{
		printf("Caught another signal, wierd?\n");
	}
}

/**
 * Send the given packet over the socket.
 * The fields are sent in the same order as defined in the struct.
 *
 * \param packet Struct with information to send
 */
static int send_packet(const broadcast_packet_t * packet)
{
	if (send(socket_fd, &packet->l_x, sizeof(packet->l_x), 0) < 0)
	{
		return -1;
	}
	if (send(socket_fd, &packet->l_y, sizeof(packet->l_y), 0) < 0)
	{
		return -1;
	}
	if (send(socket_fd, &packet->u_x, sizeof(packet->u_x), 0) < 0)
	{
		return -1;
	}
	if (send(socket_fd, &packet->u_y, sizeof(packet->u_y), 0) < 0)
	{
		return -1;
	}
	if (send(socket_fd, &packet->error_lower, sizeof(packet->error_lower), 0) < 0)
	{
		return -1;
	}
	if (send(socket_fd, &packet->error_upper, sizeof(packet->error_upper), 0) < 0)
	{
		return -1;
	}
	if (send(socket_fd, &packet->mass, sizeof(packet->mass), 0) < 0)
	{
		return -1;
	}
	if (send(socket_fd, packet->frame, IMAGE_PIXELS, 0) < 0)
	{
		return -1;
	}
	return 0;
}


static void * broadcast_thread(void * ptr)
{
	while (1)
	{	
		// Flag to indicate if we should continue waiting for "frame ready" signals
		int ok = 1;

		printf("[broadcast] Waiting for connection\n");
		socket_fd = accept(server_socket_fd, (struct sockaddr*) &addr, (socklen_t *)&addr_size);

		printf("[broadcast] Got connection.\n\n");

		while (ok)
		{
			// Lock mutex and wait for condition variable to be signaled
			pthread_mutex_lock(&frame_buffer_mtx);
			pthread_cond_wait(&frame_buffer_cv, &frame_buffer_mtx);

			// Frame is ready!

			if (send_packet(&packet) < 0)
			{
				perror("[broadcast] send_packet()");

				// Socket is broken. Release mutex and break
				// out of the loop to wait for a new connection.
				//pthread_mutex_unlock(&frame_buffer_mtx);
				ok = 0;
				close(socket_fd);
			}
			else
			{
				// Frame sent successfully
			}

			pthread_mutex_unlock(&frame_buffer_mtx);
		}
	}

	pthread_exit(NULL);
}

/**
 * Initialize the broadcast library
 */
int broadcast_init()
{
	port = PORT;

	// Initialize mutex and condition variable
	pthread_mutex_init(&frame_buffer_mtx, NULL);
	pthread_cond_init(&frame_buffer_cv, NULL);

	// Allocate memory for the broadcast packet
	packet.frame = ((unsigned char *) malloc(IMAGE_PIXELS));

	signal(SIGPIPE, signal_handler);
}

void broadcast_release()
{
	close(socket_fd);
	close(server_socket_fd);
}

void broadcast_send(int l_x, int l_y, int u_x, int u_y, int error_lower, 
	int error_upper, int mass, unsigned char * buffer)
{	
	// Try locking the frame buffer mutex, and return if the lock could not be
	// aquired. This means that the frame is skipped/ignored if the lock cannot
	// be aquired.
	if (pthread_mutex_lock(&frame_buffer_mtx) == 0)
	{	
		// Populate packet with details
		packet.l_x = l_x;
		packet.l_y = l_y;
		packet.u_x = u_x;
		packet.u_y = u_y;
		packet.error_lower = error_lower;
		packet.error_upper = error_upper;
		packet.mass = mass;
		// Copy the image data
		memcpy(packet.frame, buffer, IMAGE_PIXELS);

		// Signal that a packet is ready, and release mutex!
		pthread_cond_signal(&frame_buffer_cv);
		pthread_mutex_unlock(&frame_buffer_mtx);
	}
}

int broadcast_start()
{
	server_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(server_socket_fd == SOCKET_ERROR)
    {
        printf("{broadcast] Could not make a socket\n");
        return 0;
    }

    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    addr.sin_family = AF_INET;

    // Set SO_REUSEADDR to avoid "Address already in use" errors when trying
    // to start the server after an unexpected exit
	int optval = 1;
	setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof optval);

	// Bind to the address
    if(bind(server_socket_fd, (struct sockaddr *) &addr, sizeof(addr)) == SOCKET_ERROR)
    {
        printf("[broadcast] Could not connect to host\n");
        return 0;
    }

    // Finally listen
    getsockname(server_socket_fd, (struct sockaddr *) &addr, (socklen_t *) &addr_size);
    if(listen(server_socket_fd, QUEUE_SIZE) == SOCKET_ERROR)
    {
        printf("[broadcast] Could not listen\n");
        return 0;
    }

	pthread_create(&thread, NULL, broadcast_thread, NULL);

    printf("[broadcast] Opened socket and started listening on port %d\n", port);
	printf("[broadcast] Accept thread started\n");
}
