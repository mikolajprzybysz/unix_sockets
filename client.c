#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <signal.h>
#include <netdb.h>
#define ERR(source) (perror(source),\
		     fprintf(stderr,"%s:%d\n ",__FILE__,__LINE__),\
		     exit(EXIT_FAILURE))
#define HERR(source) (fprintf(stderr,"%s(%d) at %s:%d\n",source,h_errno,__FILE__,__LINE__),\
		     exit(EXIT_FAILURE))
		     
#define MAX_LEN 256	

// file descriptor id for used socket 
int socket_descriptor;

// uninteruptable
volatile sig_atomic_t do_work=1;

// process id
pid_t clientInputReaderProccessId;	     

ssize_t bulk_write(int fd, char *buf, size_t count);

// safe closing a file descriptor  of a socket
int safe_close(int fd){
	int status;
	for(;;){
		status = close(fd);
		
		// if failed close was caused by interupt repeat
		if( (status < 0)
			&& EINTR == errno){
			continue;
		}
		return status;
	}
}


// sets handlers for sigals
int sethandler( void (*f)(int), int sigNo){
	struct sigaction act;
	memset(&act, 0, sizeof(struct sigaction));
	act.sa_handler = f;
	if (-1 == sigaction(sigNo, &act, NULL)){
		return -1;
	}
	return 0;
}

// SIG_INT handler
void sigint_handler(int sig){
	if (do_work){
		if(safe_close(socket_descriptor) < 0) ERR("close");
	}
	do_work = 0;
}

// SIG_CHLD handler
void sigchld_handler(int sig){
	pid_t pid;
	for(;;){
		pid = waitpid(0, NULL, WNOHANG);
		if(0 == pid){
			return;
		}
		if(pid <= 0) {
			if(ECHILD == errno){
				return;
			}
			perror("waitpid:");
			exit(EXIT_FAILURE);
		}
	}
}

// creates socket for communication
int make_socket(void){
	int sock;
	
	// -1 on fail or file descriptor id
	sock = socket(PF_INET,SOCK_STREAM,0);
	if(sock < 0) ERR("socket");
	return sock;
}

// gets address based na args passed
struct sockaddr_in make_address(char *address, uint16_t port){
	struct sockaddr_in addr;
	struct hostent *hostinfo;
	addr.sin_family = AF_INET;
	addr.sin_port = htons (port);
	hostinfo = gethostbyname(address);
	if(NULL == hostinfo) HERR("gethostbyname");
	addr.sin_addr = *(struct in_addr*) hostinfo->h_addr;
	return addr;
}

int connect_socket(char *name, uint16_t port){
	struct sockaddr_in addr;
	int socketfd;
	int status;
	
	socketfd = make_socket();
	addr = make_address(name,port);
	
	// establish connection on socket
	if(connect(socketfd,(struct sockaddr*) &addr,sizeof(struct sockaddr_in)) < 0){
		if (EINTR != errno) ERR("connect");
			
		// if the only error was interupt keep n working
		else { 
			fd_set wfds;
			socklen_t size = sizeof(int);
			
			// zero-outs  all descriptors
			FD_ZERO(&wfds);
			
			// applies zeroed descriptors to socket
			FD_SET(socketfd, &wfds);
			
			// find descriptors ready to be writeen
			if(TEMP_FAILURE_RETRY(select(socketfd + 1,NULL,&wfds,NULL,NULL)) < 0) ERR("select");			
			if(getsockopt(socketfd,SOL_SOCKET,SO_ERROR,&status,&size) < 0) ERR("getsockopt");			
			if(0 != status) ERR("connect");			
		}
	}
	return socketfd;
}

// buffered secure read
ssize_t bulk_read(int fd, char *buf, size_t count){
	int c;
	size_t len = 0;
	do {
		c = TEMP_FAILURE_RETRY(read(fd,buf,count));
		if(c < 0){
			return c;
		}
		if(0 == c){
			return len;
		}
		buf += c;
		len += c;
		count -= c;
	} while(count > 0);
	return len;
}

// buffered secure write
ssize_t bulk_write(int fd, char *buf, size_t count){
	int c;
	size_t len = 0;
	do {
		c = TEMP_FAILURE_RETRY(write(fd,buf,count));
		if(c < 0){
			return c;
		}
		buf += c;
		len += c;
		count -= c;
	} while(count > 0);
	return len;
}

// info function
void usage(char * name){
	fprintf(stderr,"USAGE: %s [DOMAIN] [PORT] \n",name);
}

// replaces return carriage and new line with null symbol
void filterData(char *name, int len){
	int i;
	
	for(i = 0; i < len; i++){
		// replace \r and \n with \0
		if ('\r' == name[i] 
			|| '\n' == name[i]){
			name[i] = '\0';
		}
	}
}

// handle recieved data
void fetchSocketData( int socket ){
	char data[MAX_LEN];
	int size;
	
	while (do_work){
		if( ((size = bulk_read(socket,data,MAX_LEN)) < 0) 
			|| (MAX_LEN != size) 
			|| (0 == data[0]) ){
			
			// free socket after reading
			if (do_work && safe_close(socket)<0) ERR("close");
			
			return;
		}				
		fprintf(stderr,"%s\n", data);
	}	
}


// read client input in async manner
// then pass data via socket
void readClientIO( int socket ){
	char data[MAX_LEN];

	while ((fgets( data, MAX_LEN, stdin) != NULL ) 
			&& (do_work)){		
		
		// mark reading in the console
		fprintf(stderr,"\n");
		
		// remove bad symbols
		filterData(  data, MAX_LEN );				
		
		// push data to socket
		if(bulk_write(socket, data,MAX_LEN) < 0) ERR("write:");		
	}
}

// initial game settings
void gameInit(int socket){
	char line[MAX_LEN];
	int size;	
	fprintf(stderr,"Waiting for other players to connect.\n");	
	if(((size = bulk_read(socket,line,MAX_LEN)) < 0) 
		|| (MAX_LEN != size) 
		|| (0 == line[0])  ){	
		
		if (do_work && safe_close(socket)<0) ERR("close");			
		
		return;
	}		
	fprintf(stderr,"%s\n", line );	
	fprintf(stderr,"Enter your nickname:\n");
}

// main client logic
void mainClientProcess(int socket){
	gameInit(socket);
	
	if(do_work == 1){
		clientInputReaderProccessId= fork();
		switch (clientInputReaderProccessId){
		
			// child proccess
			case 0:		
				readClientIO( socket );
				safe_close( socket );
				exit(EXIT_SUCCESS);
			
			// on error
			case -1:
				perror("Fork:");
				exit(EXIT_FAILURE);
		}	
	
	}
	
	fetchSocketData( socket );
	if(kill(clientInputReaderProccessId, SIGINT)<0) ERR("kill");
}

int main(int argc, char** argv){	
	// check input
	if (argc != 3) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}	
	
	// set signal masks	
	// ignore sigpipe
	if(sethandler(SIG_IGN,SIGPIPE)) ERR("Seting SIGPIPE:");
	if(sethandler(sigchld_handler,SIGCHLD)) ERR("Setting parent SIGCHLD:");
	if(sethandler(sigint_handler,SIGINT)) ERR("Seting SIGINT:");
	
	// establish connection
	socket_descriptor = connect_socket(argv[1],atoi(argv[2]));	
	
	// handle logic
	mainClientProcess(socket_descriptor);
		
	// after logic is complete	
	return EXIT_SUCCESS;
}
