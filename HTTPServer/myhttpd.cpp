
const char * usage =
"                                                               \n"
"Http-server:                                                \n"
"                                                               \n"
"To use it in base mode in one window type:                     \n"
"                                                               \n"
"   ./myhttpd <port>                                       \n"
"                                                               \n"
"Where 1024 < port < 65536.             \n"
"                                                               \n"
"In your browser type:                                       \n"
"                                                               \n"
"   <host>:<port>                                        \n"
"                                                               \n"
"where <host> is the name of the machine where http-server  \n"
"is running. <port> is the port number you used when you run   \n"
"http-server.                                               \n"
"                                                               \n"
"Then type the path of the document. You will get the document \n"
"you required.                                    \n"
"                                                               \n";


#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>

int QueueLength = 5;
int errno;
pthread_mutex_t mutex;

// Processes http request
void iterativeServer( int masterSocket);
void forkServer( int masterSocket);
void createThreadForEachRequest(int masterSocket);
void poolOfThreads( int masterSocket );
void loopthread (int masterSocket);
void dispatchHTTP(int slaveSocket);
void processRequest( int socket );
void processCGI(int fd, char * method, char * path);
void processLM(int fd, char * script, char * query);
bool endsWith(const char * path, const char * end);
void badRequest(int fd, int errorCode);
void sort(char ** fileTable, int count);
void killzombie(int sig);

typedef void (* httprun_t)(int fd, char * query);

int
main( int argc, char ** argv )
{
    
    struct sigaction signalAction;
    signalAction.sa_handler = killzombie;
    sigemptyset(&signalAction.sa_mask);
    signalAction.sa_flags = SA_RESTART;
    int e = sigaction(SIGCHLD, &signalAction, NULL );
    
    if(e) {
        perror("sigaction");
    }
    
    
    // Print usage if not enough arguments
    if ( argc < 2 || argc > 3) {
        fprintf( stderr, "%s", usage );
        exit( -1 );
    }
    
    // Get the port from the arguments
    int port;
    if(argc == 2) {
        port = atoi( argv[1] );
    } else {
        port = atoi( argv[2] );
    }
    
    if (port < 1024 || port > 65536) {
        fprintf( stderr, "%s", usage );
        exit( -1 );
    }
    
    
    // Set the IP address and port for this server
    struct sockaddr_in serverIPAddress;
    memset( &serverIPAddress, 0, sizeof(serverIPAddress) );
    serverIPAddress.sin_family = AF_INET;
    serverIPAddress.sin_addr.s_addr = INADDR_ANY;
    serverIPAddress.sin_port = htons((u_short) port);
    
    // Allocate a socket
    int masterSocket =  socket(PF_INET, SOCK_STREAM, 0);
    if ( masterSocket < 0) {
        perror("socket");
        exit( -1 );
    }
    
    // Set socket options to reuse port. Otherwise we will
    // have to wait about 2 minutes before reusing the sae port number
    int optval = 1;
    int err = setsockopt(masterSocket, SOL_SOCKET, SO_REUSEADDR,
                         (char *) &optval, sizeof( int ) );
    
    // Bind the socket to the IP address and port
    int error = bind( masterSocket,
                     (struct sockaddr *)&serverIPAddress,
                     sizeof(serverIPAddress) );
    if ( error ) {
        perror("bind");
        exit( -1 );
    }
    
    // Put socket in listening mode and set the
    // size of the queue of unprocessed connections
    error = listen( masterSocket, QueueLength);
    if ( error ) {
        perror("listen");
        exit( -1 );
    }
    
    if(argc == 3 && argv[1][1] == 'f') {
        forkServer(masterSocket);
        
    } else if (argc == 3 && argv[1][1] == 't') {
        createThreadForEachRequest(masterSocket);
        
    } else if (argc == 3 && argv[1][1] == 'p') {
        poolOfThreads(masterSocket );
        
    } else if (argc == 2) {
        iterativeServer(masterSocket);
        
    } else {
        fprintf( stderr, "%s", usage );
        exit( -1 );
    }
    
}

void
iterativeServer( int masterSocket) {
    while (1) {
        // Accept incoming connections
        struct sockaddr_in clientIPAddress;
        int alen = sizeof( clientIPAddress );
        int slaveSocket = accept( masterSocket,
                                 (struct sockaddr *)&clientIPAddress,
                                 (socklen_t*)&alen);
        
        if ( slaveSocket < 0 ) {
            perror( "accept" );
            exit( -1 );
        }
        
        if (slaveSocket >= 0) {
            dispatchHTTP(slaveSocket);
        }
    }
}

void
forkServer( int masterSocket) {
    while (1) {
        struct sockaddr_in clientIPAddress;
        int alen = sizeof( clientIPAddress );
        int slaveSocket = accept( masterSocket,
                                 (struct sockaddr *)&clientIPAddress,
                                 (socklen_t*)&alen);
        if (slaveSocket >= 0) {
            int ret = fork();
            
            if (ret == 0) {
                dispatchHTTP(slaveSocket);
                dprintf(1, "closing zombie process\n");
                exit(EXIT_SUCCESS);
            } else if(ret < 0) {
                perror("fork");
            }
            
            close(slaveSocket);
        }
    }
}

void
createThreadForEachRequest(int masterSocket)
{
    while (1) {
        struct sockaddr_in clientIPAddress;
        int alen = sizeof( clientIPAddress );
        pthread_mutex_lock( &mutex );
        int slaveSocket = accept( masterSocket,
                                 (struct sockaddr *)&clientIPAddress,
                                 (socklen_t*)&alen);
        pthread_mutex_unlock( &mutex );
        
        if(slaveSocket == -1 && errno == EINTR){
            continue;
        }
        
        if (slaveSocket >= 0) {
            // When the thread ends resources are recycled
            pthread_t thread;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr,
                                        PTHREAD_CREATE_DETACHED);
            pthread_create(&thread, &attr,
                           (void * (*) (void *)) dispatchHTTP, (void *) slaveSocket);
        }
    }
}

void
poolOfThreads( int masterSocket ) {
    pthread_t thread[5];
    for (int i = 0; i < 5; i++) {
        pthread_create(&thread[i], NULL, (void * (*)(void *))loopthread,
                       (void *)masterSocket);
    }
    pthread_join(thread[0], NULL);
}

void
loopthread (int masterSocket) {
    while (1) {
        struct sockaddr_in clientIPAddress;
        int alen = sizeof( clientIPAddress );
        pthread_mutex_lock(&mutex);
        int slaveSocket = accept( masterSocket,
                                 (struct sockaddr *)&clientIPAddress,
                                 (socklen_t*)&alen);
        pthread_mutex_unlock(&mutex);
        
        if(slaveSocket == -1 && errno == EINTR){
            continue;
        }
        
        if (slaveSocket >= 0) {
            dispatchHTTP(slaveSocket);
        }
    }
}

void
dispatchHTTP(int slaveSocket) {
    processRequest( slaveSocket );
    
    // Close socket
    shutdown( slaveSocket, 1 );
    close( slaveSocket );
}

void
processRequest( int fd )
{
    // Buffer used to store the name received from the client
    const int MaxEntry = 1024;
    char entry[ MaxEntry + 1 ];
    int entryLength = 0;
    int n;
    char header [ MaxEntry ];
    char buf [ MaxEntry ];
    
    // Currently character read
    unsigned char newChar;
    
    // Last character read
    unsigned char lastChar = 0;
    
    while ( entryLength < MaxEntry &&
           ( n = read( fd, &newChar, sizeof(newChar) ) ) > 0 ) {
        
        if ( lastChar == '\015' && newChar == '\012' && entry[ entryLength - 3] == '\015' && entry[ entryLength - 2 ] == '\012') {
            // Discard previous <CR> from name
            entryLength = entryLength - 3;
            break;
        }
        
        entry[ entryLength ] = newChar;
        entryLength++;
        
        lastChar = newChar;
    }
    
    // Add null character at the end of the string
    entry[ entryLength ] = 0;
    
    if (entryLength == 0) {
        return;
    }
    if (strncmp(entry, "GET", 3) != 0 && strncmp(entry, "POST", 4) != 0) {
        return;
    }
    
    printf("entry is %s\n", entry);
    
    char * temp = strdup(entry);
    char * defPath;
    char * pathStart = strchr(temp, '/');
    temp = pathStart + 1;
    char * pathEnd = strchr(temp, ' ');
    pathEnd--;
    temp[ pathEnd - pathStart ] = '\0';
    
    printf("temp is %s\n", temp);
    
    char cwd[256] = {0};
    char path[ MaxEntry ] = {0};
    getcwd(cwd, sizeof(cwd));
    
    if (strlen(temp) == 0) {
        strcpy(path, cwd);
        strcat(path, "/http-root-dir/htdocs/index.html");
        defPath = strdup(path);
    } else if (strncmp(temp, "icons", 5) == 0 || strncmp(temp, "htdocs", 6) == 0) {
        strcpy(path, cwd);
        strcat(path, "/http-root-dir/");
        strcat(path, temp);
    } else if (strncmp(temp, "u/", 2) == 0 || strncmp(temp, "homes/", 6) == 0) {
        strcpy(path, "/");
        strcat(path, temp);
    } else if (strncmp(temp, "cgi-bin", 7) == 0) {
        strcpy(path, cwd);
        strcat(path, "/http-root-dir/");
        strcat(path, temp);
        
        char * sp = strchr(entry, ' ');
        char method[5] = {0};
        strncpy(method, entry, sp - entry);
        
        processCGI(fd, method, path);
        return;
    } else {
        strcpy(path, cwd);
        strcat(path, "/http-root-dir/htdocs/");
        strcat(path, temp);
    }
    
    printf("Path is %s\n", path);
    
    struct stat statbuf;
    int ret = stat(path, &statbuf);
    if (ret < 0) {
        return;
    }
    
    if(S_ISDIR(statbuf.st_mode)) {
        // is Directory
        char lastPath[256];
        strcpy(lastPath, path);
        if(endsWith(lastPath, "/")) {
            strcat(lastPath,"..");
        } else {
            strcat(lastPath,"/..");
        }
        char * ep = realpath(lastPath, NULL);
        if( ep == NULL) {
            perror("realpath");
            return;
        }
        
        char * pPath = strdup("..");
        if(strlen(ep) > (strlen(cwd) + strlen("/http-root-dir/htdocs/"))) {
            pPath = strdup(".");
        }
        
        char ** fileTable = (char **) calloc(1, 512);
        int count = 0;
        DIR * dir = opendir(path);
        struct dirent * ent;
        if (dir == NULL) {
            badRequest(fd, 404);
        }
        
        sprintf(header, "HTTP/1.1 200 Document follows\nServer: CS252 lab4\nConnection: close\nContent-type: text/html\n\n");
        write(fd, header, strlen(header));
        
        sprintf(buf, "<html>\n<head>\n<title>Index of %s </title>\n<script src=\"/htdocs/sorttable.js\"></script>\n</head>\n<body>\n<h1>Index of %s </h1>\n<table id=\"dirTable\" class=\"sortable\"><tr><th><img src=\"/icons/blank.gif\" alt=\"[ICO]\"></th><th><a>Name</a></th><th><a>Last modified</a></th><th><a>Size</a></th><th><a>Description</a></th></tr>\n", path, path);
        write(fd, buf, strlen(buf));

        
        sprintf(buf, "<tr><td valign=\"top\"><img src=\"/icons/back.gif\" alt=\"[DIR]\"></td><td><a href=\"%s\">Parent Directory</a> </tr>\n", pPath);
        write(fd, buf, strlen(buf));
        
        while ((ent = readdir(dir)) != NULL) {
            if(ent->d_name[0] == '.') {
                continue;
            } else {
                fileTable[count] = (char *) calloc(1, sizeof(ent->d_name));
                fileTable[count] = ent->d_name;
                count++;
            }
        }
        
        sort(fileTable, count);
        
        //        for (int i = 0; i < count; i++) {
        //            printf("filetable %d is %s\n", i, fileTable[i]);
        //        }
        
        int i;
        for(i = 0; i < count; i++) {
            
            char fpath[512] = {0};
            struct stat fstatbuf;
            strcat(fpath, path);
            if(path[strlen(path) - 1] != '/') {
                strcat(fpath, "/");
            }
            strcat(fpath, fileTable[i]);
            
            ret = stat(fpath, &fstatbuf);
            
            if(ret < 0) {
                perror("stat");
                return;
            }
            
            char * modTime = strdup(ctime(&fstatbuf.st_mtime));
            
            if (S_ISDIR(fstatbuf.st_mode)) {
                sprintf(buf, "<tr><td valign=\"top\"><img src=\"/icons/menu.gif\" alt=\"[DIR]\"></td>");
            } else if (endsWith(fileTable[i], ".gif")) {
                sprintf(buf, "<tr><td valign=\"top\"><img src=\"/icons/image.gif\" alt=\"[IMG]\"></td>");
            } else {
                sprintf(buf, "<tr><td valign=\"top\"><img src=\"/icons/unknown.gif\" alt=\"[   ]\"></td>");
            }
            write(fd, buf, strlen(buf));
            
            sprintf(buf, "<td><a href=\"");
            write(fd, buf, strlen(buf));
            
            if(path[strlen(path) - 1] != '/') {
                write(fd, temp, strlen(temp));
                write(fd, "/", 1);
            }
            
            sprintf(buf, "%s\">%s</td><td align=\"right\"> %s </td><td align=\"right\"> ", fileTable[i], fileTable[i], modTime);
            write(fd, buf, strlen(buf));
            
            
            if (S_ISDIR(fstatbuf.st_mode)) {
                write(fd, "-", 1);
            } else {
                double size = fstatbuf.st_size;
                if(size == 0.0) {
                    write(fd, "0", 1);
                } else {
                    double ksize = size/1024;
                    sprintf(buf, "%.1fK", ksize);
                    write(fd, buf, strlen(buf));
                }
                
            }
            
            sprintf(buf, " </td></tr>\n");
            write(fd, buf, strlen(buf));
        }
        
        sprintf(buf, "</table>\n</body></html>");
        write(fd, buf, strlen(buf));
        // int k = 0;
        // for(k = 0; k < count; k++) {
        //     free(fileTable[k]);
        // }
        free(fileTable);
    }
    else {
        // is file
        if (endsWith(path, ".html") || endsWith(path, ".html/")) {
            sprintf(header, "HTTP/1.1 200 Document follows\nServer: CS252 lab4\nConnection: close\nContent-type: text/html\n\n");
        } else if (endsWith(path, ".gif") || endsWith(path, ".gif/")) {
            sprintf(header, "HTTP/1.1 200 Document follows\nServer: CS252 lab4\nConnection: close\nContent-type: image/gif\n\n");
        } else {
            sprintf(header, "HTTP/1.1 200 Document follows\nServer: CS252 lab4\nConnection: close\nContent-type: text/plain\n\n");
        }
        printf("header is %s\n", header);
        write(fd, header, strlen(header));
        
        FILE * resource;
        
        resource = fopen(path, "r");
        
        if (resource == NULL) {
            perror("fopen");
            badRequest(fd, 404);
        } else {
            if (fseek(resource,0,SEEK_END) != 0) {
                return;
            }
            size_t ret = ftell(resource);
            if (ret == -1) {
                return;
            }
            rewind(resource);
            
            char *buf = new char[ret];
            if (fread( buf, sizeof(char), ret, resource ) != ret) {
                return;
            }
            if (fclose(resource) < 0) {
                perror("fclose");
                return;
            };
            write(fd, buf, ret);
        }
    }
    
    // Send last newline
    const char * newline="\n";
    write(fd, newline, strlen(newline));
}

void
processCGI(int fd, char * m, char * path) {
    int l = strlen(path);
    
    char query[l];
    char script[l];
    char method[l];
    char header[1024];
    char * temp = strdup(path);
    
    char * s = strrchr(temp, '/');
    char * q = strchr(temp, '?');
    if (q == NULL) {
        sprintf(query, "QUERY_STRING=\0");
    } else {
        q++;
        sprintf(query, "QUERY_STRING=%s", q);
        q--;
        *q = '\0';
    }
    
    s++;
    sprintf(script, "%s", s);
    
    s = strrchr(temp, '/');
    s++;
    *s = '\0';
    sprintf(method, "REQUEST_METHOD=%s", m);
    
    dprintf(1, "temp path is .%s.\n", temp);
    dprintf(1, "script name is .%s.\n", script);
    dprintf(1, "method is .%s.\n", method);
    dprintf(1, "query is .%s.\n",query);
    
    int defaultout = dup(1);
    int ret = fork();
    if (!ret) {
        //child process
        putenv(query);
        putenv(method);
        dup2(fd, 1);
        chdir(temp);
        
        sprintf(header, "HTTP/1.1 200 Document follows\nServer: CS252 lab4\nConnection: close\n");
        write(fd, header, strlen(header));

        if (endsWith(script, ".so")) {
            *s = script[0];
            printf("after temp: %s\n", temp);
            processLM(fd, temp, query);
            return;
        }
         
        execv(script, NULL);
        perror("execv");
        _exit(1);
        
    } else if (ret < 0) {
        perror("fork");
        return;
    }
    
    waitpid(ret, 0, 0);
    dup2(defaultout, 1);
    close(defaultout);
}

void
processLM(int fd, char * script, char * query) {
    
    void * handle = dlopen(script, RTLD_LAZY);
    if (!handle) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return;
    }
    
    dlerror();
    
    httprun_t httprunScript;
    
    httprunScript = (httprun_t) dlsym(handle, "httprun");
    
    if (dlerror()) {
        perror("dlsym");
        return;
    }
    
    httprunScript(fd, query);
    return;
}

bool
endsWith(const char * path, const char * end) {
    int length = strlen(path);
    int endLength = strlen(end);
    char * suffix = strdup(path);
    char * p = suffix + length - endLength;
    
    if(strcmp(p, end) == 0) {
        return true;
    }
    return false;
}

void
badRequest(int fd, int errorCode) {
    char errorHeader [1024];
    char * contentType;
    if (errorCode == 404) {
        sprintf(errorHeader, "HTTP/1.0 404 File Not Found\nServer: CS252 lab4\nContent-type: text/plain\n\n" );
        write(fd, errorHeader, strlen(errorHeader));
    }
    
    if (errorCode == 400) {
        sprintf(errorHeader, "HTTP/1.0 400 Permission Denied\nServer: CS252 lab4\nContent-type: text/plain\n\n" );
        write(fd, errorHeader, strlen(errorHeader));
    }
}

void
sort(char ** fileTable, int count) {
    int i, j;
    for(i = 0; i < count - 1; i++) {
        for(j = 0; j < count - i - 1; j++) {
            if(strcmp(fileTable[j], fileTable[j + 1]) > 0) {
                char * temp = strdup(fileTable[j]);
                fileTable[j] = strdup(fileTable[j + 1]);
                fileTable[j + 1] = strdup(temp);
            }
        }
    }
}

void
killzombie(int sig) {
    while(waitpid(-1, NULL, WNOHANG) > 0);
}
