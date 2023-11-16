#include <stdio.h>
#include <unistd.h>
#include <errno.h>


#ifndef PROXYSERVER_H
#define PROXYSERVER_H

typedef enum scode {
    OK = 200,           // ok
    BAD_REQUEST = 400,  // bad request
    BAD_GATEWAY = 502,  // bad gateway
    SERVER_ERROR = 500, // internal server error
    QUEUE_FULL = 599,   // priority queue is full
    QUEUE_EMPTY = 598   // priority queue is empty
} status_code_t;

#define GETJOBCMD "/GetJob"

/*
 * A simple HTTP library.
 *
 * Usage example:
 *
 *     // Returns NULL if an error was encountered.
 *     struct http_request *request = http_request_parse(fd);
 *
 *     ...
 *
 *     http_start_response(fd, 200);
 *     http_send_header(fd, "Content-type", http_get_mime_type("index.html"));
 *     http_send_header(fd, "Server", "httpserver/1.0");
 *     http_end_headers(fd);
 *     http_send_string(fd, "<html><body><a href='/'>Home</a></body></html>");
 *
 *     close(fd);
 */


/*
 * Functions for parsing an HTTP request.
 */
struct http_request {
    char *method;
    char *path;
    char *delay;
};

/*
 * Functions for sending an HTTP response.
 */
void http_start_response(int fd, int status_code);
void http_send_header(int fd, char *key, char *value);
void http_end_headers(int fd);
void http_send_string(int fd, char *data);
int http_send_data(int fd, char *data, size_t size);
char *http_get_response_message(int status_code);
void http_fatal_error(char *message);

#define LIBHTTP_REQUEST_MAX_SIZE 8192

struct http_request *http_request_parse(int fd); 
char *http_get_response_message(int status_code); 

#endif