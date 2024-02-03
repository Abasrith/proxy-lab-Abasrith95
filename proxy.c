/**
 * @file proxy.c
 * @brief A web proxy application with capablities to serve multiple clients
 * with contents that are either retrived from a content host server or that are
 * locally cached
 *
 * Description: Implemented a web proxy application capable of handling
 * concurrent client requests through multi-threading and caching of web pages.
 *
 *
 * @author Abhishek Basrithaya <abasrith@andrew.cmu.edu>
 */

#include "cache.h"
#include "csapp.h"
#include "http_parser.h"
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Debug macros, which can be enabled by adding -DDEBUG in the Makefile
 * Use these if you find them useful, or delete them if not
 */
#ifdef DEBUG
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_printf(...) fprintf(stderr, __VA_ARGS__)
#else
#define dbg_assert(...)
#define dbg_printf(...)
#endif
/* General defines */
#define DEFAULT_PORT_NUM 80
#define CACHE_USED 1

/* Typedef for convenience */
typedef struct sockaddr SA;

/* Header strings, formats and keys */
static const char *user_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:3.10.0) Gecko/20191101 "
    "Firefox/63.0.1\r\n";
static const char *conn_hdr = "Connection: close\r\n";
static const char *prox_hdr = "Proxy-Connection: close\r\n";
static const char *endof_hdr = "\r\n";

static const char *host_hdr_format = "Host: %s\r\n";
static const char *requestlint_hdr_format = "GET %s HTTP/1.0\r\n";

static const char *connection_key = "Connection";
static const char *user_agent_key = "User-Agent";
static const char *proxy_connection_key = "Proxy-Connection";
static const char *host_key = "Host";

/* Function prototyping */
void clientRequestHandler(int connfd);
void create_server_http_request(char *server_http_request, char *hostname,
                                char *path, int port, rio_t *client_rio);
void *threadHandler(void *vargp);
void clienterror(int fd, const char *errnum, const char *shortmsg,
                 const char *longmsg);
void Pthread_create(pthread_t *tidp, pthread_attr_t *attrp,
                    void *(*routine)(void *), void *argp);
void Pthread_detach(pthread_t tid);
void posix_error(int code, char *msg);

#if CACHE_USED
extern Cache cache;
#endif
/**
 * @brief sigpipe signal handler
 *
 *
 * @param[in]   sig                 signal input
 *
 * @return      void
 */
void sigpipe_handler(int sig) {
    sio_printf("SIGPIPE handled\n");
    return;
}

/**
 * @brief main
 *
 *
 * @param[in]   argc                Number of arguments passed
 * @param[in]   **argv              Array of arguments
 *
 * @return      void
 */
int main(int argc, char **argv) {
    int listenfd, *connfd = NULL;
    socklen_t clientlen;
    char hostname[MAXLINE], port[MAXLINE];
    pthread_t tid;
    struct sockaddr_storage clientaddr;

    /* assign signal handler for Broken pipe */
    Signal(SIGPIPE, sigpipe_handler);

    if (argc != 2) {
        fprintf(stderr, "usage :%s <port> \n", argv[0]);
        exit(1);
    }

    listenfd = open_listenfd(argv[1]);
    if (listenfd < 0) {
        fprintf(stderr, "Failed to listen on port: %s\n", argv[1]);
        exit(1);
    }
#if CACHE_USED
    /* Initialise cache here */
    cache_init();
#endif

    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = malloc(sizeof(int));
        if (connfd == NULL) {
            fprintf(stderr, "Failed to malloc: %ls\n", connfd);
            exit(1);
        }
        *connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        if (*connfd < 0) {
            fprintf(stderr, "Failed to accept request on port: %s\n", argv[1]);
            exit(1);
        }
        /*print accepted message*/
        getnameinfo((SA *)&clientaddr, clientlen, hostname, MAXLINE, port,
                    MAXLINE, 0);
        sio_printf("Accepted connection from (%s %s).\n", hostname, port);

        /*handle the client transaction by spawning threads */
        Pthread_create(&tid, NULL, threadHandler, (void *)connfd);
    }
    /* never reach this position */
    return 0;
}
/**
 * @brief thread handler
 *
 *
 * @param[in]   vargp                argument passed to thread handler
 *
 * @return      void
 */
void *threadHandler(void *vargp) {
    int connfd = *((int *)vargp);
    Pthread_detach(pthread_self());
    free(vargp);
    clientRequestHandler(connfd);
    close(connfd);
    return NULL;
}

/**
 * @brief handle the client HTTP transaction by parsing requests, error
 * handling, cache search and relay, sending new requests to end server and
 * caching the reponses into an LRU cache, after which the reponses are
 * forwarded to client.
 *
 *
 * @param[in]   connfd                client side connection fd
 *
 * @return      void
 */
void clientRequestHandler(int connfd) {
    int serverfd; /*the server file descriptor*/

    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char server_http_request[MAXLINE];

    /*store the request line arguments*/
    char hostname[MAXLINE], path[MAXLINE];
    int port = DEFAULT_PORT_NUM;
    parser_state parseClientLineState;
    parser_t *parseClientLine = parser_new();

    /*rio is client's rio, server_rio is server's rio */
    rio_t rio, server_rio;
    /* Initialise client I/O */
    rio_readinitb(&rio, connfd);

    /* Read client I/O */
    if (rio_readlineb(&rio, buf, MAXLINE) <= 0) {
        /* Freeing parcer variable */
        parser_free(parseClientLine);
        return;
    }
    /*parse request line */
    if (sscanf(buf, "%s %s HTTP/1.%c", method, uri, version) != 3 ||
        (*version != '0' && *version != '1')) {
        clienterror(connfd, "400", "Bad Request",
                    "Proxy received a malformed request");
        /* Freeing parcer variable */
        parser_free(parseClientLine);
        return;
    }

    /* Returning on non GET methods */
    if (strcmp(method, "GET") != 0) {
        clienterror(connfd, "501", "Not Implemented",
                    "Proxy does not implement this method");
        /* Freeing parcer variable */
        parser_free(parseClientLine);
        return;
    }

#if CACHE_USED
    /*search for url in cache */
    cache_block *reqCachePtr = NULL;
    /*in cache then return the cache content*/
    if ((reqCachePtr = cache_find(uri)) != NULL) {
        /* Critical section reference has to be incremented by this point */
        rio_writen(connfd, reqCachePtr->cache_obj, reqCachePtr->cache_obj_size);
        lockMutex(&cache.rwMutex);
        reqCachePtr->readReferenceCnt--;
        unLockMutex(&cache.rwMutex);
        /* Critical section reference has to be decremented by this point */
        /* Freeing parcer variable */
        parser_free(parseClientLine);
        return;
    }
#endif

    /*parse the uri to get hostname,file path ,port*/
    if ((parseClientLineState = parser_parse_line(parseClientLine, buf)) ==
        ERROR) {
        sio_printf("Client request parse error\n");
        /* Freeing parcer variable */
        parser_free(parseClientLine);
        return;
    }
    const char *portVal, *pathVal, *hostnameVal;
    if (parser_retrieve(parseClientLine, PORT, &portVal) < 0) {
        sio_printf("Value parsing for port failed\n");
        /* Freeing parcer variable */
        parser_free(parseClientLine);
        return;
    }
    if (parser_retrieve(parseClientLine, PATH, &pathVal) < 0) {
        sio_printf("Value parsing for path failed\n");
        /* Freeing parcer variable */
        parser_free(parseClientLine);
        return;
    }
    if (parser_retrieve(parseClientLine, HOST, &hostnameVal) < 0) {
        sio_printf("Value parsing for host failed\n");
        /* Freeing parcer variable */
        parser_free(parseClientLine);
        return;
    }
    if (portVal == NULL)
        port = DEFAULT_PORT_NUM;
    else
        port = atoi(portVal);

    strcpy(hostname, hostnameVal);
    strcpy(path, pathVal);

    /* Freeing parcer variable */
    parser_free(parseClientLine);

    /*build the http header which will send to the end server*/
    create_server_http_request(server_http_request, hostname, path, port, &rio);

    /*connect to the end server*/
    char portStr[MAXLINE];
    sprintf(portStr, "%d", port);
    serverfd = open_clientfd(hostname, portStr);
    if (serverfd < 0) {
        sio_printf("connection attempt to %s at %s failed\n", hostname,
                   portStr);
        return;
    }
    /* Initialise server I/O */
    rio_readinitb(&server_rio, serverfd);
    /*write the http header to destination server */
    rio_writen(serverfd, server_http_request, strlen(server_http_request));

    /* receive message from destination server and send to the client */
    size_t n;
#if CACHE_USED
    size_t sizebuf = 0;
    char ResponseObjectbuf[MAX_OBJECT_SIZE];
#endif
    while ((n = rio_readnb(&server_rio, buf, MAXLINE)) != 0) {
/* Cache copy here */
#if CACHE_USED
        if (sizebuf < MAX_OBJECT_SIZE) {
            memcpy(ResponseObjectbuf + sizebuf, buf, n);
        }
        sizebuf += n;
#endif
        /* Write to client FD the response received from server */
        rio_writen(connfd, buf, n);
    }
    close(serverfd);
#if CACHE_USED
    /*store it*/
    if ((sizebuf < MAX_OBJECT_SIZE) &&
        ((reqCachePtr = cache_find(uri)) == NULL)) {
        /* copy uri */
        lockMutex();
        char *cache_url = Malloc(sizeof(uri) / sizeof(uri[0]));
        memcpy(cache_url, uri, sizeof(uri) / sizeof(uri[0]));
        unLockMutex();
        cache_uri(cache_url, ResponseObjectbuf, sizebuf);
    }
#endif
}
/**
 * @brief creating server http request
 *
 *
 * @param[in]   *server_http_request                final server request string
 * @param[in]   *hostname                           hostname to be retireved
 * from uri
 * @param[in]   *path                               path to be retireved from
 * uri
 * @param[in]   *client_rio                         client side file to read and
 * write contents
 * @param[in]   port                                server port to communicate
 * with
 *
 * @return      void
 */

void create_server_http_request(char *server_http_request, char *hostname,
                                char *path, int port, rio_t *client_rio) {
    char buf[MAXLINE], request_hdr[MAXLINE], other_hdr[MAXLINE],
        host_hdr[MAXLINE];
    /*request line*/
    sprintf(request_hdr, requestlint_hdr_format, path);
    /*get other request header for client rio and change it */
    while (rio_readlineb(client_rio, buf, MAXLINE) > 0) {
        /*EOF*/
        if (strcmp(buf, endof_hdr) == 0)
            break;
        /*Host*/
        if (strstr(buf, host_key) != NULL) {
            strcpy(host_hdr, buf);
            continue;
        }
        if (!strstr(buf, connection_key) &&
            !strstr(buf, proxy_connection_key) &&
            !strstr(buf, user_agent_key) && !strstr(buf, host_key)) {
            strcat(other_hdr, buf);
        }
    }

    if (strlen(host_hdr) == 0) {
        sprintf(host_hdr, host_hdr_format, hostname);
    }

    sprintf(server_http_request, "%s%s%s%s%s%s%s", request_hdr, host_hdr,
            conn_hdr, prox_hdr, user_hdr, other_hdr, endof_hdr);
    return;
}

/**************************
 * Error-handling function taken from csapp.c
 **************************/
/**
 * @brief Posix style error handling
 *
 *
 * @param[in]   int           return code
 * @param[in]   *msg          message associated with return code
 *
 * @return      void
 */
void posix_error(int code, char *msg) /* Posix-style error */
{
    fprintf(stderr, "%s: %s\n", msg, strerror(code));
    exit(0);
}
/************************************************
 * Wrappers for Pthreads thread control functions
 ************************************************/
/**
 * @brief wrapper to handle creation of threads
 *
 *
 * @param[in]   *tidp           Thread ID
 * @param[in]   *attrp          Attributes of thread
 * @param[in]   *routine        Function to be called for therad execution
 * @param[in]   *argp           arguments to the thread function
 *
 * @return      void
 */
void Pthread_create(pthread_t *tidp, pthread_attr_t *attrp,
                    void *(*routine)(void *), void *argp) {
    int rc;

    if ((rc = pthread_create(tidp, attrp, routine, argp)) != 0)
        posix_error(rc, "Pthread_create error");
}
/**
 * @brief wrapper to handle detaching of a threads to reap automatically upon
 * termination
 *
 *
 * @param[in]   *tidp           Thread ID
 *
 * @return      void
 */
void Pthread_detach(pthread_t tid) {
    int rc;

    if ((rc = pthread_detach(tid)) != 0)
        posix_error(rc, "Pthread_detach error");
}

/**
 * @brief returns an error message to the client
 *
 *
 * @param[in]   fd              Client file descriptor write the error message
 * @param[in]   errnum          HTTP response ststus codes(error)
 * @param[in]   *shortmsg       Short message on error
 * @param[in]   *longmsg        long message on error
 *
 * @return      void
 */
void clienterror(int fd, const char *errnum, const char *shortmsg,
                 const char *longmsg) {
    char buf[MAXLINE];
    char body[MAXBUF];
    size_t buflen;
    size_t bodylen;

    /* Build the HTTP response body */
    bodylen = snprintf(body, MAXBUF,
                       "<!DOCTYPE html>\r\n"
                       "<html>\r\n"
                       "<head><title>Proxy Error</title></head>\r\n"
                       "<body bgcolor=\"ffffff\">\r\n"
                       "<h1>%s: %s</h1>\r\n"
                       "<p>%s</p>\r\n"
                       "<hr /><em>The Web Proxy</em>\r\n"
                       "</body></html>\r\n",
                       errnum, shortmsg, longmsg);
    if (bodylen >= MAXBUF) {
        return; // Overflow!
    }

    /* Build the HTTP response headers */
    buflen = snprintf(buf, MAXLINE,
                      "HTTP/1.0 %s %s\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: %zu\r\n\r\n",
                      errnum, shortmsg, bodylen);
    if (buflen >= MAXLINE) {
        return; // Overflow!
    }

    /* Write the headers */
    if (rio_writen(fd, buf, buflen) < 0) {
        fprintf(stderr, "Error writing error response headers to client\n");
        return;
    }

    /* Write the body */
    if (rio_writen(fd, body, bodylen) < 0) {
        fprintf(stderr, "Error writing error response body to client\n");
        return;
    }
}
