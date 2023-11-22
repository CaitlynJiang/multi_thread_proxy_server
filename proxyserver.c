#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <ctype.h>

#include "proxyserver.h"
#include "safequeue.h"


/*
 * Constants
 */
#define RESPONSE_BUFSIZE 10000
#define THREAD_POOL_SIZE 100  // max thread pool size

pthread_t listener_thread_pool[THREAD_POOL_SIZE];
pthread_t worker_thread_pool[THREAD_POOL_SIZE];
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t condition_var = PTHREAD_COND_INITIALIZER;

/*
 * Global configuration variables.
 * Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
int num_listener;
int *listener_ports;
int num_workers;
char *fileserver_ipaddr;
int fileserver_port;
int max_queue_size;
SafeQueue * pq;
struct http_request *req;

void send_error_response(int client_fd, status_code_t err_code, char *err_msg) {
    http_start_response(client_fd, err_code);
    http_send_header(client_fd, "Content-Type", "text/html");
    http_end_headers(client_fd);
    char *buf = malloc(strlen(err_msg) + 2);
    sprintf(buf, "%s\n", err_msg);
    http_send_string(client_fd, buf);
    return;
}


/*
 * forward the client request to the fileserver and
 * forward the fileserver response to the client
 */
void serve_request(int client_fd) {

    // Parse the client request and extract delay
    Struct result;
    result = parseRequest(client_fd);

    // If there's a delay, sleep for the specified amount of time
    if (result.delay > 0) {
        sleep(result.delay);
    }

    // create a fileserver socket
    int fileserver_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fileserver_fd == -1) {
        fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
        exit(errno);
    }

    // create the full fileserver address (address to listen on)
    struct sockaddr_in fileserver_address;
    fileserver_address.sin_addr.s_addr = inet_addr(fileserver_ipaddr);
    fileserver_address.sin_family = AF_INET;                   //address family 
    fileserver_address.sin_port = htons(fileserver_port);      //port

    // connect to the fileserver
    int connection_status = connect(fileserver_fd, (struct sockaddr *)&fileserver_address,
                                    sizeof(fileserver_address));
    if (connection_status < 0) {
        // failed to connect to the fileserver
        printf("Failed to connect to the file server\n");
        send_error_response(client_fd, BAD_GATEWAY, "Bad Gateway");
        return;
    }

    // successfully connected to the file server
    char *buffer = (char *)malloc(RESPONSE_BUFSIZE * sizeof(char));

    // forward the client request to the fileserver
    int bytes_read = read(client_fd, buffer, RESPONSE_BUFSIZE);    // read client_fd to buffer, return 0 on suceed
    int ret = http_send_data(fileserver_fd, buffer, bytes_read);   
    if (ret < 0) {
        printf("Failed to send request to the file server\n");
        send_error_response(client_fd, BAD_GATEWAY, "Bad Gateway");

    } else {
        // forward the fileserver response to the client
        while (1) {
            int bytes_read = recv(fileserver_fd, buffer, RESPONSE_BUFSIZE - 1, 0);   // wait for file sever response
            if (bytes_read <= 0) // fileserver_fd has been closed, break
                break;
            ret = http_send_data(client_fd, buffer, bytes_read);
            if (ret < 0) { // write failed, client_fd has been closed
                break;
            }
        }
    }

    // close the connection to the fileserver
    shutdown(fileserver_fd, SHUT_WR);
    close(fileserver_fd);

    // Free resources and exit
    free(buffer);

    // Free resources and properly close the client connection
    close(client_fd);
}


void *worker_function(void *arg) {
    while (1) {
        int *pclient = NULL;

        pthread_mutex_lock(&mutex);
        do {
            pclient = get_work(pq);
            if (pclient == NULL) {
                pthread_cond_wait(&condition_var, &mutex); // Wait if queue is empty
            }
        } while (pclient == NULL);
        pthread_mutex_unlock(&mutex);

        if (pclient != NULL) {
        serve_request(*pclient);
        }
    }
}


int server_fd;
/*
 * opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void serve_forever(int *server_fd) {

    // create a socket to listen
    *server_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (*server_fd == -1) {
        perror("Failed to create a new socket");
        exit(errno);
    }

    // manipulate options for the socket
    int socket_option = 1;
    if (setsockopt(*server_fd, SOL_SOCKET, SO_REUSEADDR, &socket_option,
                   sizeof(socket_option)) == -1) {
        perror("Failed to set socket options");
        exit(errno);
    }


    int proxy_port = listener_ports[0];
    // create the full address of this proxyserver
    struct sockaddr_in proxy_address;
    memset(&proxy_address, 0, sizeof(proxy_address));
    proxy_address.sin_family = AF_INET;
    proxy_address.sin_addr.s_addr = INADDR_ANY;
    proxy_address.sin_port = htons(proxy_port); // listening port

    // bind the socket to the address and port number specified in
    if (bind(*server_fd, (struct sockaddr *)&proxy_address,
             sizeof(proxy_address)) == -1) {
        perror("Failed to bind on socket");
        exit(errno);
    }

    // starts waiting for the client to request a connection
    if (listen(*server_fd, 1024) == -1) {
        perror("Failed to listen on socket");
        exit(errno);
    }

    printf("Listening on port %d...\n", proxy_port);


    struct sockaddr_in client_address;
    size_t client_address_length = sizeof(client_address);
    int client_fd;
    while (1) {
        client_fd = accept(*server_fd, (struct sockaddr *)&client_address, (socklen_t *)&client_address_length);
        if (client_fd < 0) {
            perror("Error accepting socket");
            continue;
        }

        printf("Accepted connection from %s on port %d\n", inet_ntoa(client_address.sin_addr), client_address.sin_port);

        Struct result;
        result = parseRequest(client_fd);
        
        if (strcmp(GETJOBCMD, result.path) == 0) {

            pthread_mutex_lock(&mutex);
            int *pclient = get_work_nonblocking(pq);  //Attempt to get a job
            pthread_mutex_unlock(&mutex);

            if (pclient == NULL) {
                // Queue is empty
                send_error_response(client_fd, QUEUE_EMPTY, "Priority Queue is Empty");
            } else {
                // Parse the job's request to get its path
                struct http_request *job_request = http_request_parse(*pclient);
                if (job_request) {
                    // Send the job's path back to the client
                    http_start_response(client_fd, OK);
                    http_send_header(client_fd, "Content-Type", "text/plain");
                    http_end_headers(client_fd);
                    http_send_string(client_fd, job_request->path);
                    free(job_request->method);
                    free(job_request->path);
                    free(job_request);
                } else {
                    // Handle parsing error
                    send_error_response(client_fd, SERVER_ERROR, "Error parsing job request");
                }
                free(pclient);
            }
            shutdown(client_fd, SHUT_RDWR);
            close(client_fd);
        } else {

            // Regular job adding
            int *pclient = malloc(sizeof(int));
            *pclient = client_fd;

            pthread_mutex_lock(&mutex);

            if (add_work(pq, pclient, result.priority) == -1) {
                // Queue is full, send error response to client
                send_error_response(client_fd, QUEUE_FULL, "Priority Queue is Full");
                free(pclient);
                shutdown(client_fd, SHUT_RDWR);
                close(client_fd);
            } else {
                pthread_cond_signal(&condition_var);
            }
            pthread_mutex_unlock(&mutex);
        }
    }

    // Close server socket and clean up on shutdown
    shutdown(*server_fd, SHUT_RDWR);
    close(*server_fd);
}

void *listener_function(void *arg) {
    int port = *((int *)arg);
    int server_fd;

    // create a socket to listen
    server_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Failed to create a new socket");
        exit(errno);
    }

    // manipulate options for the socket
    int socket_option = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &socket_option,
                   sizeof(socket_option)) == -1) {
        perror("Failed to set socket options");
        exit(errno);
    }

    int proxy_port = port;    // looped over the ports for listeners (not here)
    // create the full address of this proxyserver
    struct sockaddr_in proxy_address;
    memset(&proxy_address, 0, sizeof(proxy_address));
    proxy_address.sin_family = AF_INET;
    proxy_address.sin_addr.s_addr = INADDR_ANY;
    proxy_address.sin_port = htons(proxy_port); // listening port

    // bind the socket to the address and port number specified in
    if (bind(server_fd, (struct sockaddr *)&proxy_address,
             sizeof(proxy_address)) == -1) {
        perror("Failed to bind on socket");
        exit(errno);
    }

    // starts waiting for the client to request a connection
    if (listen(server_fd, 1024) == -1) {
        perror("Failed to listen on socket");
        exit(errno);
    }

    printf("Listening on port %d...\n", proxy_port);


    struct sockaddr_in client_address;
    size_t client_address_length = sizeof(client_address);
    int client_fd;
    while (1) {
        client_fd = accept(server_fd, (struct sockaddr *)&client_address, (socklen_t *)&client_address_length);
        if (client_fd < 0) {
            perror("Error accepting socket");
            continue;
        }

        printf("Accepted connection from %s on port %d\n", inet_ntoa(client_address.sin_addr), client_address.sin_port);

        Struct result;
        result = parseRequest(client_fd);
        
        if (strcmp(GETJOBCMD, result.path) == 0) {

            pthread_mutex_lock(&mutex);
            int *pclient = get_work_nonblocking(pq);  //Attempt to get a job
            pthread_mutex_unlock(&mutex);

            if (pclient == NULL) {
                // Queue is empty
                send_error_response(client_fd, QUEUE_EMPTY, "Priority Queue is Empty");
            } else {
                // Parse the job's request to get its path
                struct http_request *job_request = http_request_parse(*pclient);
                if (job_request) {
                    // Send the job's path back to the client
                    http_start_response(client_fd, OK);
                    http_send_header(client_fd, "Content-Type", "text/plain");
                    http_end_headers(client_fd);
                    http_send_string(client_fd, job_request->path);
                    free(job_request->method);
                    free(job_request->path);
                    free(job_request);
                } else {
                    // Handle parsing error
                    send_error_response(client_fd, SERVER_ERROR, "Error parsing job request");
                }
                free(pclient);
            }
            shutdown(client_fd, SHUT_RDWR);
            close(client_fd);
        } else {

            // Regular job adding
            int *pclient = malloc(sizeof(int));
            *pclient = client_fd;

            pthread_mutex_lock(&mutex);

            if (add_work(pq, pclient, result.priority) == -1) {
                // Queue is full, send error response to client
                send_error_response(client_fd, QUEUE_FULL, "Priority Queue is Full");
                free(pclient);
                shutdown(client_fd, SHUT_RDWR);
                close(client_fd);
            } else {
                pthread_cond_signal(&condition_var);
            }
            pthread_mutex_unlock(&mutex);
        }
    
    }

    // Close server socket and clean up on shutdown
    shutdown(server_fd, SHUT_RDWR);
    close(server_fd);
}

/*
 * Default settings for in the global configuration variables
 */
void default_settings() {
    num_listener = 1;
    listener_ports = (int *)malloc(num_listener * sizeof(int));
    listener_ports[0] = 8000;

    num_workers = 1;

    fileserver_ipaddr = "127.0.0.1";
    fileserver_port = 3333;

    max_queue_size = 100;
}

void print_settings() {
    printf("\t---- Setting ----\n");
    printf("\t%d listeners [", num_listener);
    for (int i = 0; i < num_listener; i++)
        printf(" %d", listener_ports[i]);
    printf(" ]\n");
    printf("\t%d workers\n", num_workers);
    printf("\tfileserver ipaddr %s port %d\n", fileserver_ipaddr, fileserver_port);
    printf("\tmax queue size  %d\n", max_queue_size);
    printf("\t  ----\t----\t\n");
}

void signal_callback_handler(int signum) {
    printf("Caught signal %d: %s\n", signum, strsignal(signum));
    for (int i = 0; i < num_listener; i++) {
        if (close(server_fd) < 0) perror("Failed to close server_fd (ignoring)\n");
    }
    free(listener_ports);
    exit(0);
}

char *USAGE =
    "Usage: ./proxyserver [-l 1 8000] [-n 1] [-i 127.0.0.1 -p 3333] [-q 100]\n";

void exit_with_usage() {
    fprintf(stderr, "%s", USAGE);
    exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {

    signal(SIGINT, signal_callback_handler);

    /* Default settings */
    default_settings();

    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp("-l", argv[i]) == 0) {
            num_listener = atoi(argv[++i]);
            free(listener_ports);
            listener_ports = (int *)malloc(num_listener * sizeof(int));
            for (int j = 0; j < num_listener; j++) {
                listener_ports[j] = atoi(argv[++i]);
            }
        } else if (strcmp("-w", argv[i]) == 0) {
            num_workers = atoi(argv[++i]);
        } else if (strcmp("-q", argv[i]) == 0) {
            max_queue_size = atoi(argv[++i]);
        } else if (strcmp("-i", argv[i]) == 0) {
            fileserver_ipaddr = argv[++i];
        } else if (strcmp("-p", argv[i]) == 0) {
            fileserver_port = atoi(argv[++i]);
        } else {
            fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
            exit_with_usage();
        }
    }
    print_settings();

    // Initialize the priority queue
    pq = create_queue(max_queue_size);
    if (pq == NULL) {
        fprintf(stderr, "Failed to create the priority queue\n");
        exit(EXIT_FAILURE);
    }

    // Initialize listener thread pool
    int *port;
    for (int i = 0; i < num_listener-1; i++) {
        port = malloc(sizeof(int)); // Allocate memory for the port number
        *port = listener_ports[i+1];  // Copy the port number
        pthread_create(&listener_thread_pool[i], NULL, listener_function, port);
    }

    // Initialize worker thread pool
    for (int i = 0; i < num_workers; i++){
        pthread_create(&worker_thread_pool[i], NULL, worker_function, NULL);
    }

    serve_forever(&server_fd);

    return EXIT_SUCCESS;
}
