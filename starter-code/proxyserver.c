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

#include "proxyserver.h"
#include "safequeue.h"

/*
 * Constants
 */
#define RESPONSE_BUFSIZE 10000

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

    // create a fileserver socket
    int fileserver_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (fileserver_fd == -1) {
        fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
        exit(errno);
    }

    // create the full fileserver address
    struct sockaddr_in fileserver_address;
    fileserver_address.sin_addr.s_addr = inet_addr(fileserver_ipaddr);
    fileserver_address.sin_family = AF_INET;
    fileserver_address.sin_port = htons(fileserver_port);

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
    int bytes_read = read(client_fd, buffer, RESPONSE_BUFSIZE);
    int ret = http_send_data(fileserver_fd, buffer, bytes_read);
    if (ret < 0) {
        printf("Failed to send request to the file server\n");
        send_error_response(client_fd, BAD_GATEWAY, "Bad Gateway");

    } else {
        // forward the fileserver response to the client
        while (1) {
            int bytes_read = recv(fileserver_fd, buffer, RESPONSE_BUFSIZE - 1, 0);
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
}


int server_fd;

// attempt for step 1:

typedef struct {
    Safequeue *pq;
    int ind;
    int server_fd;
} ThreadParams;

struct http_request *parse_http_request(int client_fd) {
    char buffer[1024];
    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        return NULL; // Error in receiving or connection closed
    }

    buffer[bytes_read] = '\0'; // Null-terminate the string

    struct http_request *request = malloc(sizeof(struct http_request));
    if (request == NULL) {
        return NULL; // Memory allocation failed
    }

    // Initialize fields to NULL
    request->method = NULL;
    request->path = NULL;
    request->delay = NULL;

    // Parse the request (simple parsing for demonstration)
    char *method = strtok(buffer, " ");
    char *path = strtok(NULL, " ");
    // You can add more parsing logic here as needed

    // Copy parsed values into the struct
    if (method) request->method = strdup(method);
    if (path) request->path = strdup(path);

    // Extract delay from path if present
    // Example: /path?delay=10
    char *delay_param = strstr(request->path, "delay=");
    if (delay_param) {
        delay_param += 6; // Skip past 'delay='
        request->delay = strdup(delay_param);
        // Modify the path to remove the delay parameter
        char *question_mark = strchr(request->path, '?');
        if (question_mark) *question_mark = '\0'; // Truncate path at '?'
    }

    return request;
}

// helper function: handle requst in one listening port
void *serve_thread(void *arg) {
    ThreadParams *thread = (ThreadParams *)arg;

        // create a socket to listen
        thread->server_fd = socket(PF_INET, SOCK_STREAM, 0);
        if (thread->server_fd == -1) {
            perror("Failed to create a new socket");
            exit(errno);
        }

        // manipulate options for the socket
        int socket_option = 1;
        if (setsockopt(thread->server_fd, SOL_SOCKET, SO_REUSEADDR, &socket_option,
                sizeof(socket_option)) == -1) {
            perror("Failed to set socket options");
            exit(errno);
        } 

        // each poxy_port listens to one tread
        int proxy_port = listener_ports[thread->ind];
        // create the full address of this proxyserver
        struct sockaddr_in proxy_address;
        memset(&proxy_address, 0, sizeof(proxy_address));
        proxy_address.sin_family = AF_INET;
        proxy_address.sin_addr.s_addr = INADDR_ANY;
        proxy_address.sin_port = htons(proxy_port); // listening port

        // bind the socket to the address and port number specified in
        if (bind(thread->server_fd, (struct sockaddr *)&proxy_address,
                sizeof(proxy_address)) == -1) {
            perror("Failed to bind on socket");
            exit(errno);
        }

        // starts waiting for the client to request a connection
        if (listen(thread->server_fd, 1024) == -1) {
            perror("Failed to listen on socket");
            exit(errno);
        }

        printf("Listening on port %d...\n", proxy_port);

        // waiting for clients to connect
        struct sockaddr_in client_address;
        size_t client_address_length = sizeof(client_address);
        int client_fd;
        while (1) {
            client_fd = accept(thread->server_fd,
                            (struct sockaddr *)&client_address,
                            (socklen_t *)&client_address_length);
            if (client_fd < 0) {
                perror("Error accepting socket");
                continue;
            }

            printf("Accepted connection from %s on port %d\n",
                inet_ntoa(client_address.sin_addr),
                client_address.sin_port);
            
            // Process the request
            struct http_request *request = parse_http_request(client_fd);
            if (request != NULL) {
                if (add_work(thread->pq, request) == -1) {
                    send_error_response(client_fd, QUEUE_FULL, "Queue Full");
                }
            } else {
                // Handle parsing error or invalid request
                send_error_response(client_fd, BAD_REQUEST, "Bad Request");
            }

            serve_request(client_fd);

            // TODO: implement GETJOB request - handled by proxy server directly

            // close the connection to the client
            shutdown(client_fd, SHUT_WR);
            close(client_fd);
        }

    shutdown(thread->server_fd, SHUT_RDWR);
    close(thread->server_fd);
    return NULL;
}

void serve_forever(int *server_fd) {

    Safequeue *pq = create_queue(max_queue_size);
    pthread_t threads[num_listener];

    for (int i = 0; i < num_listener; i++) {
        ThreadParams *thread = malloc(sizeof(ThreadParams)); 
        thread->pq = pq;
        thread->ind = i; 
        thread->server_fd = socket(PF_INET, SOCK_STREAM, 0);

        if (pthread_create(&threads[i], NULL, serve_thread, thread) != 0) {
            perror("Failed to create listener thread");
            free(thread); 
            continue; 
        }

        free(thread);
    }

    // wait for threads to finish
    // for (int i = 0; i < num_listener; i++) {
    //     pthread_join(threads[i], NULL);
    // }

    // clean up priority queue, implement destroy_pq?
}


/*
 * opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
 /*
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
        client_fd = accept(*server_fd,
                           (struct sockaddr *)&client_address,
                           (socklen_t *)&client_address_length);
        if (client_fd < 0) {
            perror("Error accepting socket");
            continue;
        }

        printf("Accepted connection from %s on port %d\n",
               inet_ntoa(client_address.sin_addr),
               client_address.sin_port);

        serve_request(client_fd);

        // close the connection to the client
        shutdown(client_fd, SHUT_WR);
        close(client_fd);
    }

    shutdown(*server_fd, SHUT_RDWR);
    close(*server_fd);
}
*/

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

    // TODO: enable multi-threading
    // pthread_t threads[num_listener];
    // for (int i = 0; i < num_listener; i++) {
    //     pthread_create(&threads[i], NULL, serve_forever, (void *)&listener_ports[i]);
    // }

    // // Join threads or handle them appropriately
    // for (int i = 0; i < num_listener; i++) {
    //     pthread_join(threads[i], NULL);
    // }

    serve_forever(&server_fd);

    return EXIT_SUCCESS;
}



// additional function definitions

void http_start_response(int fd, int status_code) {
    dprintf(fd, "HTTP/1.0 %d %s\r\n", status_code,
            http_get_response_message(status_code));
}

void http_send_header(int fd, char *key, char *value) {
    dprintf(fd, "%s: %s\r\n", key, value);
}

void http_end_headers(int fd) {
    dprintf(fd, "\r\n");
}

void http_send_string(int fd, char *data) {
    http_send_data(fd, data, strlen(data));
}

int http_send_data(int fd, char *data, size_t size) {
    ssize_t bytes_sent;
    while (size > 0) {
        bytes_sent = write(fd, data, size);
        if (bytes_sent < 0)
            return -1; // Indicates a failure
        size -= bytes_sent;
        data += bytes_sent;
    }
    return 0; // Indicate success
}

void http_fatal_error(char *message) {
    fprintf(stderr, "%s\n", message);
    exit(ENOBUFS);
}

struct http_request *http_request_parse(int fd) {
    struct http_request *request = malloc(sizeof(struct http_request));
    if (!request) http_fatal_error("Malloc failed");

    char *read_buffer = malloc(LIBHTTP_REQUEST_MAX_SIZE + 1);
    if (!read_buffer) http_fatal_error("Malloc failed");

    int bytes_read = read(fd, read_buffer, LIBHTTP_REQUEST_MAX_SIZE);
    read_buffer[bytes_read] = '\0'; /* Always null-terminate. */

    char *read_start, *read_end;
    size_t read_size;

    do {
        /* Read in the HTTP method: "[A-Z]*" */
        read_start = read_end = read_buffer;
        while (*read_end >= 'A' && *read_end <= 'Z') {
            printf("%c", *read_end);
            read_end++;
        }
        read_size = read_end - read_start;
        if (read_size == 0) break;
        request->method = malloc(read_size + 1);
        memcpy(request->method, read_start, read_size);
        request->method[read_size] = '\0';
        printf("parsed method %s\n", request->method);

        /* Read in a space character. */
        read_start = read_end;
        if (*read_end != ' ') break;
        read_end++;

        /* Read in the path: "[^ \n]*" */
        read_start = read_end;
        while (*read_end != '\0' && *read_end != ' ' && *read_end != '\n')
            read_end++;
        read_size = read_end - read_start;
        if (read_size == 0) break;
        request->path = malloc(read_size + 1);
        memcpy(request->path, read_start, read_size);
        request->path[read_size] = '\0';
        printf("parsed path %s\n", request->path);

        /* Read in HTTP version and rest of request line: ".*" */
        read_start = read_end;
        while (*read_end != '\0' && *read_end != '\n')
            read_end++;
        if (*read_end != '\n') break;
        read_end++;

        free(read_buffer);
        return request;
    } while (0);

    /* An error occurred. */
    free(request);
    free(read_buffer);
    return NULL;
}

char *http_get_response_message(int status_code) {
    switch (status_code) {
    case 100:
        return "Continue";
    case 200:
        return "OK";
    case 301:
        return "Moved Permanently";
    case 302:
        return "Found";
    case 304:
        return "Not Modified";
    case 400:
        return "Bad Request";
    case 401:
        return "Unauthorized";
    case 403:
        return "Forbidden";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    default:
        return "Internal Server Error";
    }
}