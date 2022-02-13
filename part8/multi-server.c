/*
 * multi-server.c
 */

#include <stdio.h>      /* for printf() and fprintf() */
#include <sys/socket.h> /* for socket(), bind(), and connect() */
#include <arpa/inet.h>  /* for sockaddr_in and inet_ntoa() */
#include <stdlib.h>     /* for atoi() and exit() */
#include <string.h>     /* for memset() */
#include <unistd.h>     /* for close() */
#include <time.h>       /* for time() */
#include <netdb.h>      /* for gethostbyname() */
#include <signal.h>     /* for signal() */
#include <sys/stat.h>   /* for stat() */
#include <pthread.h>    /* for pthread_create */

#define MAXPENDING 5    /* Maximum outstanding connection requests */

#define DISK_IO_BUF_SIZE 4096

#define N_THREADS 16
static pthread_t thread_pool[N_THREADS];

static void die(const char *message)
{
    perror(message);
    exit(1); 
}

/*
 * A message in a blocking queue
 */
struct message {
    int sock; // Payload, in our case a new client connection
    struct message *next; // Next message on the list
};

/*
 * This structure implements a blocking queue.
 * If a thread attempts to pop an item from an empty queue
 * it is blocked until another thread appends a new item.
 */
struct queue {
    pthread_mutex_t mutex; // mutex used to protect the queue
    pthread_cond_t cond;   // condition variable for threads to sleep on
    struct message *first; // first message in the queue
    struct message *last;  // last message in the queue
    unsigned int length;   // number of elements on the queue
};


// initializes the members of struct queue
void queue_init(struct queue *q){
    if(pthread_mutex_init(&q->mutex, NULL) != 0)
        die("mutex destruction failed"); 
    if(pthread_cond_init(&q->cond, NULL) != 0)
        die("cond destruction failed"); 
    q->first = NULL; 
    q->last = NULL; 
    q->length = 0; 
};

// deallocate and destroy everything in the queue
void queue_destroy(struct queue *q){
    struct message *msg;

    //lock
    pthread_mutex_lock(&q->mutex);
    while (q->length != 0){
        msg = q->first;
        q->first = q->first->next;
        q->length--;
        free(msg);
    }
    q->last = NULL;
    pthread_mutex_unlock(&q->mutex);

    if(pthread_mutex_destroy(&q->mutex) != 0)
        die("mutex destroy failed"); 
    if(pthread_cond_destroy(&q->cond) != 0)
        die("cond initialization failed"); 
};

// put a message into the queue and wake up workers if necessary
void queue_put(struct queue *q, int sock){
    struct message *pmsg;
    pmsg = (struct message *)malloc(sizeof(*pmsg));
    if (pmsg == NULL)
        die("malloc failed");
    pmsg->sock = sock;
    pmsg->next = NULL;

    //lock
    pthread_mutex_lock(&q->mutex);
    if (q->length==0)
        q->first = pmsg; 
    else 
        q->last->next = pmsg; 
    q->last = pmsg;
    q->length ++;
    pthread_mutex_unlock(&q->mutex);

    // how to wake up workers?
    if(pthread_cond_signal(&q->cond)!=0)
        die("pthread_cond_signal failed");

};

// take a socket descriptor from the queue; block if necessary
int queue_get(struct queue *q){
    int sock;
    struct message *pmsg;

    // lock
    pthread_mutex_lock(&q->mutex);

    // Is this block?
    while(q->length == 0)
        pthread_cond_wait(&q->cond, &q->mutex);
    pmsg = q->first; 
    sock = pmsg->sock; 
    q->first = pmsg->next;
    if (q->length == 1)
        q->last = NULL;
    q->length --;
    pthread_mutex_unlock(&q->mutex);
    free(pmsg);
    return sock; 
    
};




/*
 * Create a listening socket bound to the given port.
 */
static int createServerSocket(unsigned short port)
{
    int servSock;
    struct sockaddr_in servAddr;

    /* Create socket for incoming connections */
    if ((servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        die("socket() failed");
      
    /* Construct local address structure */
    memset(&servAddr, 0, sizeof(servAddr));       /* Zero out structure */
    servAddr.sin_family = AF_INET;                /* Internet address family */
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY); /* Any incoming interface */
    servAddr.sin_port = htons(port);              /* Local port */

    /* Bind to the local address */
    if (bind(servSock, (struct sockaddr *)&servAddr, sizeof(servAddr)) < 0)
        die("bind() failed");

    /* Mark the socket so it will listen for incoming connections */
    if (listen(servSock, MAXPENDING) < 0)
        die("listen() failed");

    return servSock;
}

/*
 * A wrapper around send() that does error checking and logging.
 * Returns -1 on failure.
 * 
 * This function assumes that buf is a null-terminated string, so
 * don't use this function to send binary data.
 */
ssize_t Send(int sock, const char *buf)
{
    size_t len = strlen(buf);
    ssize_t res = send(sock, buf, len, 0);
    if (res != len) {
        perror("send() failed");
        return -1;
    }
    else 
        return res;
}

/*
 * HTTP/1.0 status codes and the corresponding reason phrases.
 */

static struct {
    int status;
    char *reason;
} HTTP_StatusCodes[] = {
    { 200, "OK" },
    { 201, "Created" },
    { 202, "Accepted" },
    { 204, "No Content" },
    { 301, "Moved Permanently" },
    { 302, "Moved Temporarily" },
    { 304, "Not Modified" },
    { 400, "Bad Request" },
    { 401, "Unauthorized" },
    { 403, "Forbidden" },
    { 404, "Not Found" },
    { 500, "Internal Server Error" },
    { 501, "Not Implemented" },
    { 502, "Bad Gateway" },
    { 503, "Service Unavailable" },
    { 0, NULL } // marks the end of the list
};

static inline const char *getReasonPhrase(int statusCode)
{
    int i = 0;
    while (HTTP_StatusCodes[i].status > 0) {
        if (HTTP_StatusCodes[i].status == statusCode)
            return HTTP_StatusCodes[i].reason;
        i++;
    }
    return "Unknown Status Code";
}


/*
 * Send HTTP status line followed by a blank line.
 */
static void sendStatusLine(int clntSock, int statusCode)
{
    char buf[1000];
    const char *reasonPhrase = getReasonPhrase(statusCode);

    // print the status line into the buffer
    sprintf(buf, "HTTP/1.0 %d ", statusCode);
    strcat(buf, reasonPhrase);
    strcat(buf, "\r\n");

    // We don't send any HTTP header in this simple server.
    // We need to send a blank line to signal the end of headers.
    strcat(buf, "\r\n");

    // For non-200 status, format the status line as an HTML content
    // so that browers can display it.
    if (statusCode != 200) {
        char body[1000];
        sprintf(body, 
                "<html><body>\n"
                "<h1>%d %s</h1>\n"
                "</body></html>\n",
                statusCode, reasonPhrase);
        strcat(buf, body);
    }

    // send the buffer to the browser
    Send(clntSock, buf);
}

/*
 * Handle static file requests.
 * Returns the HTTP status code that was sent to the browser.
 */
static int handleFileRequest(
        const char *webRoot, const char *requestURI, int clntSock)
{
    int statusCode;
    FILE *fp = NULL;

    // Compose the file path from webRoot and requestURI.
    // If requestURI ends with '/', append "index.html".
    
    char *file = (char *)malloc(strlen(webRoot) + strlen(requestURI) + 100);
    if (file == NULL)
        die("malloc failed");
    strcpy(file, webRoot);
    strcat(file, requestURI);
    if (file[strlen(file)-1] == '/') {
        strcat(file, "index.html");
    }

    // See if the requested file is a directory.
    // Our server does not support directory listing.

    struct stat st;
    if (stat(file, &st) == 0 && S_ISDIR(st.st_mode)) {
        statusCode = 403; // "Forbidden"
        sendStatusLine(clntSock, statusCode);
        goto func_end;
    }

    // If unable to open the file, send "404 Not Found".

    fp = fopen(file, "rb");
    if (fp == NULL) {
        statusCode = 404; // "Not Found"
        sendStatusLine(clntSock, statusCode);
        goto func_end;
    }

    // Otherwise, send "200 OK" followed by the file content.

    statusCode = 200; // "OK"
    sendStatusLine(clntSock, statusCode);

    // send the file 
    size_t n;
    char buf[DISK_IO_BUF_SIZE];
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (send(clntSock, buf, n, 0) != n) {
            // send() failed.
            // We log the failure, break out of the loop,
            // and let the server continue on with the next request.
            perror("\nsend() failed");
            break;
        }
    }
    // fread() returns 0 both on EOF and on error.
    // Let's check if there was an error.
    if (ferror(fp))
        perror("fread failed");

func_end:

    // clean up
    free(file);
    if (fp)
        fclose(fp);

    return statusCode;
}


struct args {
    const char *webRoot;
    struct queue *q ;
};

/*
 * Create a function to handle requests
 * As there are more than one argument, arg should be an struct
 */
void * thr_worker(void *arg)
{
    pthread_detach(pthread_self());
    char line[1000];
    char requestLine[1000];
    int statusCode;
    int clntSock; 
    struct args *args;
    struct sockaddr_in clntAddr;
    const char *webRoot;
    struct queue *q ; 
    args = (struct args *)arg; 

    // servSock = args->servSock;
    webRoot = args->webRoot;
    q = args->q;

    for(;;){

        // There is a while loop in this function, it will wait until there is a socket comming 
        clntSock = queue_get(q); 

        if (clntSock < 0)
            die("queue_get failed"); 

        // We should get client address from client socket
        unsigned int clntLen = sizeof(clntAddr); 
        if(getsockname(clntSock, (struct sockaddr *)&clntAddr, &clntLen) != 0)
            die("getsockname failed");


        // This is the first command after accept in original main
        FILE *clntFp = fdopen(clntSock, "r");
            if (clntFp == NULL)
                die("fdopen failed");

        /*
         * Let's parse the request line.
         */

        char *method      = "";
        char *requestURI  = "";
        char *httpVersion = "";

        if (fgets(requestLine, sizeof(requestLine), clntFp) == NULL) {
            // socket closed - there isn't much we can do
            statusCode = 400; // "Bad Request"
            goto loop_end;
        }

        char *token_separators = "\t \r\n"; // tab, space, new line
        method = strtok(requestLine, token_separators);
        requestURI = strtok(NULL, token_separators);
        httpVersion = strtok(NULL, token_separators);
        char *extraThingsOnRequestLine = strtok(NULL, token_separators);

        // check if we have 3 (and only 3) things in the request line
        if (!method || !requestURI || !httpVersion || 
                extraThingsOnRequestLine) {
            statusCode = 501; // "Not Implemented"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }

        // we only support GET method 
        if (strcmp(method, "GET") != 0) {
            statusCode = 501; // "Not Implemented"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }

        // we only support HTTP/1.0 and HTTP/1.1
        if (strcmp(httpVersion, "HTTP/1.0") != 0 && 
            strcmp(httpVersion, "HTTP/1.1") != 0) {
            statusCode = 501; // "Not Implemented"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }
        
        // requestURI must begin with "/"
        if (!requestURI || *requestURI != '/') {
            statusCode = 400; // "Bad Request"
            sendStatusLine(clntSock, statusCode);
            goto loop_end;
        }

        // make sure that the requestURI does not contain "/../" and 
        // does not end with "/..", which would be a big security hole!
        int len = strlen(requestURI);
        if (len >= 3) {
            char *tail = requestURI + (len - 3);
            if (strcmp(tail, "/..") == 0 || 
                    strstr(requestURI, "/../") != NULL)
            {
                statusCode = 400; // "Bad Request"
                sendStatusLine(clntSock, statusCode);
                goto loop_end;
            }
        }

        /*
         * Now let's skip all headers.
         */


        while (1) {
            if (fgets(line, sizeof(line), clntFp) == NULL) {
                // socket closed prematurely - there isn't much we can do
                statusCode = 400; // "Bad Request"
                goto loop_end;
            }
            if (strcmp("\r\n", line) == 0 || strcmp("\n", line) == 0) {
                // This marks the end of headers.  
                // Break out of the while loop.
                break;
            }
        }

        /*
         * At this point, we have a well-formed HTTP GET request.
         * Let's handle it.
         */

        statusCode = handleFileRequest(webRoot, requestURI, clntSock);

loop_end:

        /*
         * Done with client request.
         * Log it, close the client socket, and go back to accepting
         * connection.
         */
        
        fprintf(stderr, "%s \"%s %s %s\" %d %s\n",
                inet_ntoa(clntAddr.sin_addr),
                method,
                requestURI,
                httpVersion,
                statusCode,
                getReasonPhrase(statusCode));

        // close the client socket 
        fclose(clntFp);
    } // for(;;)

    free(args);
    return((void *)0);

}

int main(int argc, char *argv[])
{
    
    int i = 0; 
    struct args *args; 
    int err;
    int nfds = 3;
    fd_set readfds;
    fd_set prev_readfds;
    // fd_set *restrict writefds; //not interested?
    // fd_set *restrict exceptfds;
    const char *webRoot;
    struct queue *sock_queue; 
    sock_queue = (struct queue *)malloc(sizeof(*sock_queue)); 
    queue_init(sock_queue); 

    // Ignore SIGPIPE so that we don't terminate when we call
    // send() on a disconnected socket.
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
        die("signal() failed");
    if (argc < 3) {
        fprintf(stderr,
            "usage: %s <server_port> [<server_port> ...] <web_root>\n",
            argv[0]);
        exit(1);
    
    }
    nfds = argc + 1;
    int servSocks[32];
    memset(servSocks, -1, sizeof(servSocks));
    FD_ZERO(&readfds);
    // Create server sockets for all ports we listen on
    for (i = 1; i < argc - 1; i++) {
        if (i >= (sizeof(servSocks) / sizeof(servSocks[0])))
            die("Too many listening sockets");
        servSocks[i - 1] = createServerSocket(atoi(argv[i]));
        FD_SET(servSocks[i-1], &readfds);
        fprintf(stderr, "servsocks: %d : %d \n", servSocks[i-1], atoi(argv[i]));
    }
    webRoot = argv[argc - 1];
    prev_readfds = readfds;
    // unsigned short servPort = atoi(argv[1]);
    // const char *webRoot = argv[2];
 
    // int servSock = createServerSocket(servPort);

    struct sockaddr_in clntAddr;

    args = (struct args *)malloc(sizeof(*args));
    args->webRoot = webRoot; 
    args->q = sock_queue; 

    // create thread
    for (i=0; i<N_THREADS; i++) {
        err = pthread_create(&thread_pool[i], NULL, thr_worker, args);
        if (err != 0)
            die("can’t create thread");

    } // for (; i<N_THREADS; i++)

    for (;;){

        /*
         * wait for a client to connect
         */

        //find a server socket with a client pending 

        // initialize the in-out parameter
        unsigned int clntLen = sizeof(clntAddr); 
        int servs = 0;
        while(servs == 0){
            servs = select(nfds, &readfds, NULL, NULL, NULL);
        }
        for(int validfd = 1; validfd <= nfds; validfd ++){
            if (FD_ISSET(validfd, &readfds)){
                int clntSock = accept(validfd, (struct sockaddr *)&clntAddr, &clntLen);
                if (clntSock < 0)
                    die("accept() failed");
                queue_put(sock_queue, clntSock); 
                // break;
            }
        }
        readfds = prev_readfds;
    } // for (;;)
    queue_destroy(sock_queue);
    free(sock_queue);
    // if (signal(SIGINT, sig_int) == SIG_ERR)
    //     die("signal(SIGINT) error");
    return 0;
}