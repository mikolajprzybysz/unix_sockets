#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <netinet/in.h>
#include <signal.h>
#include <netdb.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/sem.h>
#include <time.h>

#define ERR(source) (perror(source),\
		     fprintf(stderr,"%s:%d\n",__FILE__,__LINE__),\
		     exit(EXIT_FAILURE))

#define LOGFILE "logs.txt"			 
#define BACKLOG 3
#define MAX_LEN 256
#define MAX_player_array 128


// player states
#define IDLE 0
#define NOTPLAYING 1
#define PLAYING 2
#define FINISHED 3

// msg types
#define CHATPRV 0
#define MOVE 1
#define CHATALL 2

// game symbol
#define EMPTYCELL '-'
#define X 'X'
#define O 'O'

volatile sig_atomic_t do_work=1;
int semid;

int pipes[2];

int addPlayer(int);
void removePlayer(int);
int getBySocket(int);
int getByPID(pid_t);
int getUnpairedPlayer(int);
void sendBoard(int, char b[]);
void clearBoard(char b[]);
void sendText(int, char* str);
void sendSplit(int);
void lockSemaphore(int);
void unlockSemaphore(int);
int boardState(char, char*);
void chatPrv(int,char*);
int playerCommunicationInit(int,FILE*);
int playerInit(int);
void broadcastListen( int );

// http://linux.die.net/man/2/semctl
union semun {		

	// Value for SETVAL
	int              val;
	
	// Buffer for IPC_STAT, IPC_SET
	struct semid_ds *buf;    /*  */
	
	// Array for GETALL, SETALL
	unsigned short  *array;  /*  */
	
	// Buffer for IPC_INFO
	struct seminfo  *__buf;  
};

// PLAYER_STRUCT
typedef struct {	

	//	0 - IDLE
	//	1 - NOTPLAYING
	//	2 - PLAYING
	//	3 - FINISHED
	int state;		
	
	// player process id
	pid_t pid;
	
	// player socket and paired player socket
	int socket, pairsocket;
	
	// id of paried player
	int pairnum;
	
	// player nick name
	char name[64];
	
	// game board
	char board[25];
	
	// if player is moving now
	int movieing;
	
} player_struct;

player_struct* player_array;

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

// SIG_INT handler
void sigint_handler(int sig){
	do_work = 0;
}

// SIG_CHLD handler
void sigchld_handler(int sig){
	pid_t pid;
	for(;;){
	
		// request status for any child process whose process group ID 
		// is equal to that of the calling process
		pid = waitpid(0, NULL, WNOHANG);
		
		// if status unavailable (due to WNOHANG)
		if(0 == pid){
			return;
		
		// if status is available
		} else {
		
			// on error
			if(pid <= 0) 
			{
				// The process or process group specified by pid does not exist 
				// or is not a child of the calling process.
				if(ECHILD == errno) return;
				perror("waitpid:");
				exit(EXIT_FAILURE);
				
			// child pid returned
			} else {
				lockSemaphore(0);
				int p = getByPID(pid);
				if (p > -1){
					removePlayer(p);
				}
				unlockSemaphore(0);
			}
		}
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

// creates socket for communication
int make_socket(int domain, int type){
	int sock;
	
	// -1 on fail or file descriptor id
	sock = socket(domain,type,0);
	if(sock < 0) ERR("socket");
	return sock;
}

// create socket handler
int bind_inet_socket(uint16_t port,int type){
	struct sockaddr_in addr;
	int socketfd;
	int t = 1;
	socketfd = make_socket(PF_INET,type);
	memset(&addr, 0, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	
	// host to network short
	addr.sin_port = htons(port);
	
	// host to network long
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	
	// use socket on socket level, reuse if not listening
	if(setsockopt(socketfd, SOL_SOCKET, SO_REUSEADDR,&t, sizeof(t))) ERR("setsockopt");
	if(bind(socketfd,(struct sockaddr*) &addr,sizeof(addr)) < 0)  ERR("bind");
	if(SOCK_STREAM == type){
		if(listen(socketfd, BACKLOG) < 0) ERR("listen");
	}
	return socketfd;
}

// accept new connections
int add_new_client(int sfd){
	int nfd;
	if((nfd = TEMP_FAILURE_RETRY(accept(sfd,NULL,NULL))) < 0) {
		if(EAGAIN == errno
			|| EWOULDBLOCK == errno){
			return -1;
		}
		ERR("accept");
	}
	return nfd;
}

// manual
void usage(char* name){
	fprintf(stderr,"USAGE: %s [PORT]\n",name);
}

// read block
ssize_t bulk_read(int fd, char *buf, size_t count){
	int c;
	size_t len = 0;
	do{
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
	return len ;
}

// write block
ssize_t bulk_write(int fd, char *buf, size_t count){
	int c;
	size_t len = 0;
	do{
		c = TEMP_FAILURE_RETRY(write(fd,buf,count));
		if(c < 0) return c;
		buf += c;
		len += c;
		count -= c;
	} while(count > 0);
	return len ;
}

// 00 01 02 03 04
// 05 06 07 08 09
// 10 11 12 13 14
// 15 16 17 18 19
// 20 21 22 23 24
// check if given symbol has won returns 1 if winner exists or 0 if not
int isWinner(char player_arrayymbol, char* board){
	int i;
	
	// left top to bottom right
	if(board[0]==player_arrayymbol 
		&& board[6]==player_arrayymbol
		&& board[12]==player_arrayymbol
		&& board[18]==player_arrayymbol
		&& board[24]==player_arrayymbol){
		return 1;
	}
	
	// right top to bottom left
	if(board[4]==player_arrayymbol
		&& board[8]==player_arrayymbol
		&& board[12]==player_arrayymbol
		&& board[16]==player_arrayymbol
		&& board[20]==player_arrayymbol){
		return 1;
	}
	
	for(i=0; i<5; i++){
	
		// horizontal rules
		if(board[i*5]==player_arrayymbol
			&& board[i*5+1]==player_arrayymbol
			&& board[i*5+2]==player_arrayymbol
			&& board[i*5+3]==player_arrayymbol
			&& board[i*5+4]==player_arrayymbol){
			return 1;
		}
		
		// vertical rules
		if(board[i]==player_arrayymbol
			&& board[i+5]==player_arrayymbol
			&& board[i+5*2]==player_arrayymbol
			&& board[i+5*3]==player_arrayymbol
			&& board[i+5*4]==player_arrayymbol){
			return 1;
		}
	}
	return 0;	
}

// if no emptycells return tie (1) or not (0)
int isTie(char* board){
	int i;
	for(i=0; i<24; i++){
		if(EMPTYCELL == board[i]){
			return 0;
		}
	}
	return 1;
}

//zwracam 0 jesli gra sie toczy; 1 jesli c wygral; 2 jesli remis
int boardState(char player_arrayymbol, char* board ){
	if(isWinner(player_arrayymbol, board)){
		return 1;
	}
	else if(isTie(board)){
		return 2;
	}
	return 0;
}

// check if game should be finished
void checkGameStatus(int num, char* board,FILE* logfile){
	char data[MAX_LEN];
	lockSemaphore(0);
	
	// get player_array sockets
	int pairsocket = player_array[num].pairsocket;
	int socket = player_array[num].socket;
	
	// get time
	time_t tt;
	struct tm *t; 	
	time(&tt);
	t = localtime(&tt);	
	
	// we dont know if current player plays with X or O so we check for both
	if (1 == boardState( X, board ) 
		|| 1 == boardState( O, board )){
		// current player won
		sprintf( data, "#%s gracz:  %s wygral z graczem:  %s\n", asctime(t), player_array[num].name, player_array[player_array[num].pairnum].name);		
	
	// if neighter player won
	} else {
		// check for tie
		if (2 == boardState( X, board )){
			// tie
			sprintf( data, "#%s gracz:  %s remisuje z graczem: %s\n", asctime(t), player_array[num].name, player_array[player_array[num].pairnum].name);	
			
		// if no tie the game continues
		} else {
			unlockSemaphore(0);			
			sendText( socket, "Waiting for opponent to move" );
			// dont log data
			return;
		}
	}		
	
	fprintf(stderr,"%s",data);
	// update player state
	player_array[num].state = player_array[player_array[num].pairnum].state = FINISHED;
	unlockSemaphore(0);
	
	// send information
	sendText(socket, data );	
	sendText(pairsocket, data );	
	
	// disconnect
	sendSplit(socket);
	sendSplit(pairsocket);	
	
	// log data
	lockSemaphore(1);
	fputs(data, logfile);
	fputs(board,logfile);
	fputs("\n",logfile);
	unlockSemaphore(1);		
}

// make a move
void playerMove(int playerId, int move, FILE* fLog){
	lockSemaphore(0);	
	int pairsocket = player_array[playerId].pairsocket;
	char* board = player_array[playerId].board;	
	char* board2 = player_array[player_array[playerId].pairnum].board;

	if (player_array[playerId].state == PLAYING)	{
		
		// if X is moving
		if (X != player_array[playerId].board[move] 
			&& O != player_array[playerId].board[move]){
			
			// set symbol
			player_array[playerId].board[move] = X;			
			unlockSemaphore(0);
			
			sendBoard(pairsocket, board );
			
			// next player move
			lockSemaphore(0);
			player_array[playerId].movieing = 0;
			player_array[player_array[playerId].pairnum].movieing = 1;
			unlockSemaphore(0);
			
			checkGameStatus(playerId, board, fLog);
		} else {
			unlockSemaphore(0);
		}
	} else {
	
		// if O is moving
		if (X != player_array[player_array[playerId].pairnum].board[move] 
			&& O != player_array[player_array[playerId].pairnum].board[move])	{
			
			// set symbol
			player_array[player_array[playerId].pairnum].board[move] = O;
			unlockSemaphore(0);
			
			sendBoard( pairsocket, board2 );
			
			// next player mpve
			lockSemaphore(0);
			player_array[playerId].movieing = 0;
			player_array[player_array[playerId].pairnum ].movieing = 1;
			unlockSemaphore(0);
			
			checkGameStatus(playerId, board2, fLog);
		} else {
			unlockSemaphore(0);
		}
	}	
}

// private chat with opponent
void chatPrv(int num, char* data){
	char buf[MAX_LEN];
	int opponentSocket;
	
	// exclusive access to pipe and player_array
	lockSemaphore(0);
	
	opponentSocket = player_array[num].pairsocket;
	fprintf(stderr, "[%s] %s\n", player_array[num].name, data );
	sprintf(buf, "[%s]: %s", player_array[num].name, data );
	unlockSemaphore(0);
	
	if (-1 != opponentSocket){
		if(bulk_write(opponentSocket, buf, MAX_LEN) < 0) ERR("chat prv write:");		
	}
}

// chat to all
void chatAll(int num, char* data){	
	char buf[MAX_LEN];
	int pipefd;
	
	// exclusive access to pipe and player_array
	lockSemaphore(0);
	
	// write pipe
	pipefd = pipes[1];
	fprintf(stderr, "[%s] %s\n", player_array[num].name, data );
	sprintf(buf, "[%s]: %s", player_array[num].name, data );

	unlockSemaphore(0);
					
	if(bulk_write(pipefd, buf, MAX_LEN) < 0) ERR("chat all write:");

}

// returns message type
int getMsgType(char* data){

	// checks proper move (00-19)
	if( ('0' == data[0]
		|| '1' == data[0]) 
		&& data[1] >= '0'){
		return MOVE;
		
	// checks proper move (20-24)
	} else if('2' == data[0] 
				&& data[1] >= '0' 
				&& data[1]<'5'){
		return MOVE;
		
	// if CHATALL
	} else if ('@' == data[0]){
		return CHATALL;
	}
	return CHATPRV;
}

// setup communication with new player
int playerCommunicationInit(int playerId,FILE* fLog){
	ssize_t size;
	char data[MAX_LEN];
	int socket;
	lockSemaphore(0);
	socket = player_array[playerId].socket;
	unlockSemaphore(0);

	for(;;){
		size = bulk_read(socket, data, MAX_LEN);
		if(MAX_LEN == size){
			lockSemaphore(0);
			
			// MOVE
			if( (player_array[playerId].movieing)
				&& (MOVE == getMsgType(data))){
				unlockSemaphore(0);
				int data0 = (int)data[0]-'0';
				int data1 = (int)data[1]-'0';
				int move = data0 * 10 + data1;
				playerMove(playerId, move, fLog);
				
			// CHATALL
			} else if(CHATALL == (getMsgType(data))){
				unlockSemaphore(0);
				chatAll(playerId, data );				
			
			// CHATPRV type
			} else {
				unlockSemaphore(0);
				chatPrv(playerId, data );
			}	
		} else {
			if(safe_close(socket) < 0) ERR("close");
			return 0;
		}
	}
	return 1;
}

// set starting player and save user nickname
int playerInit(int playerId){
	ssize_t size;
	char data[MAX_LEN];
	
	// exclusive read player socket and game state
	lockSemaphore(0);	
	int socket = player_array[playerId].socket;
	char* board = player_array[playerId].board;		
	unlockSemaphore(0);	

	// get data from player
	sendText(socket, "Your nickname: ");
	size = bulk_read(socket, data, MAX_LEN);

	if(MAX_LEN == size){
		lockSemaphore(0);		
		
		// save player_array name
		strcpy(player_array[playerId].name, data);
		if (NOTPLAYING == player_array[playerId].state 
			&& NOTPLAYING == player_array[player_array[playerId].pairnum].state){
			
			// set whos move is now
			player_array[playerId].movieing = 1;
			player_array[player_array[playerId].pairnum].movieing = 0;
			player_array[playerId].state = PLAYING;			
			unlockSemaphore(0);
			
			// send board to player_array
			sendBoard(socket, board);						
		} else {
			unlockSemaphore(0);					
		}	
		return 1;
	} else {
		if(safe_close(socket) < 0) ERR("close");
		return 0;
	}

}
// disconnects player_array and logs game in the log file
void disconnectplayer_array(int playerId, FILE* fLog){
	char data[MAX_LEN];	
	time_t tt;	
	struct tm *t; 	
	
	lockSemaphore(0);	
	// player is not IDLE
	if(IDLE < player_array[playerId].state){
		// finish transmition
		sendSplit(player_array[ playerId ].socket);
		
		// if player is playing
		if (player_array[ playerId ].state == PLAYING){
			// get time	
			time(&tt);
			// get local time
			t = localtime(&tt);				
			// format date 
			sprintf( data, "#%s gracz:  %s kontra gracz: %s nierozstrzygniete\n", asctime(t), player_array[ playerId ].name, player_array[player_array[ playerId ].pairnum].name );	
			unlockSemaphore(0);	
			
			fprintf(stderr,"%s",data);
			
			lockSemaphore(1);
			fputs(data, fLog);
			fputs(player_array[ playerId ].board,fLog);
			fputs("\n",fLog);
			unlockSemaphore(1);		
		} else {
			unlockSemaphore(0);
		}
	}
	else
	unlockSemaphore(0);
}

// initiate player and communication with it or handle disconnects
void mainClientProcess( int socketfd, int socket, int playerId, FILE* fLog){
	pid_t pid = fork();
	// parent work
	if (0 == pid){
		if(safe_close(socketfd) < 0)ERR("close1");
		if (1 == playerInit(playerId)){			
			playerCommunicationInit(playerId, fLog);
		}
		if (player_array[ playerId ].state < FINISHED){
			disconnectplayer_array(player_array[ playerId ].pairnum, fLog);		
		}
		exit(EXIT_SUCCESS);
	// child work
	} else {
		if (pid > 0 )	{
			lockSemaphore(0);
			player_array[ playerId ].pid = pid;
			unlockSemaphore(0);
			
		//on error
		} else {
			ERR("fork:");
		}
	}
}
void broadcastListen( int pipefd ){
	char data[MAX_LEN];
	int size;
	int i = 0; 
	int state = -1; 
	int socket = -1;
	
	// read data
	if ( ((size = bulk_read(pipefd,data,MAX_LEN)) < 0) 
		|| (size != MAX_LEN) 
		|| (0 == data[0]) ){
		
		if (do_work){
			if(safe_close(pipefd) < 0) ERR("close");
		}		
	}			

	// broadcast it to player_array
	for(i=0;i<MAX_player_array;i++)	{	
		lockSemaphore(0);
		socket = player_array[i].socket;
		state = player_array[i].state;
		unlockSemaphore(0);
		if(PLAYING == state
			|| NOTPLAYING == state){
			if (-1 != socket){				
				if(bulk_write(socket,data,MAX_LEN)<0) ERR("broadcastListen");					
			}
		}
	}	
}

void mainServerProcess(int socketfd, FILE* fLog){
	int socket, fdmax, firstsocket, playerId, playerId2;
	int pipeStatus, pipefd;
	fd_set base_rfds, rfds;
	sigset_t mask, oldmask;	
	 
	// on pipe fail
	if (-1 == (pipeStatus = pipe(pipes))) { 		
   		perror("pipe");
    	exit(1);
	}
	// get pipe read
	pipefd = pipes[0];
	
	// set masks
	FD_ZERO(&base_rfds);
	FD_SET(socketfd, &base_rfds);
	FD_SET(pipefd, &base_rfds);
	
	// get max file descriptor
	fdmax = (socketfd > pipefd ? socketfd : pipefd);

	// init empty set
	sigemptyset (&mask);
	
	// add sig_int
	sigaddset (&mask, SIGINT);
	
	socket = -1;
	sigprocmask (SIG_BLOCK, &mask, &oldmask);
	
	while(do_work){			
		rfds = base_rfds;
		
		if(pselect(fdmax + 1,&rfds,NULL,NULL,NULL,&oldmask) > 0){			
			if(FD_ISSET(pipefd,&rfds)){
				broadcastListen( pipefd );				
			}
			if(FD_ISSET(socketfd,&rfds)){			
				// add new client
				socket = add_new_client(socketfd);
				
				// get exclusive
				lockSemaphore(0);
				playerId = addPlayer(socket);
				playerId2 = getUnpairedPlayer(playerId);
				unlockSemaphore(0);
				
				// try game start
				if (playerId2 >= 0){
					mainClientProcess(socketfd, firstsocket, playerId2, fLog);
					mainClientProcess(socketfd, socket, playerId, fLog);				
				} else {
					firstsocket = socket;
				}
			}			
		} else {
			if(EINTR == errno) continue;
			ERR("pselect");
		}
	}
	sigprocmask (SIG_UNBLOCK, &mask, NULL);
}
//ustawia wszystkich graczy na nieaktywnych na poczatku
void setupplayer_array(){
	int i;

	for (i = 0; i < MAX_player_array; i++){
		player_array[i].state = IDLE;
	}
}

// clears player_array array disconneting them before
void clearplayer_array(){
	int i;
	
	// get exclusive access to player_array array
	lockSemaphore(0);
	for (i = 0; i < MAX_player_array; i++){
		if (player_array[i].state){	
			sendSplit(player_array[i].socket);
			removePlayer(i);			
		}		
	}
	unlockSemaphore(0);
}

// add new player and return its pos in player_array array
int addPlayer(int socket){
	int i;
	for (i = 0; i < MAX_player_array; i++){	
		if (IDLE == player_array[i].state){			
			player_array[i].state = NOTPLAYING;
			player_array[i].socket = socket;
			player_array[i].pairsocket = -1;
			player_array[i].movieing = 0;
			// allocated mem for player name
			memset(player_array[i].name, 0, 64);			
			fprintf(stderr,"Hello [%i]!\n", i);
			return i;
		}
	}

	return -1;
}

// finds unpaired player and return its array pos or -1 on error, additionally we pair free player to the player with id given
int getUnpairedPlayer(int freePlayer){
	int i;
	for (i = 0; i < MAX_player_array; i++){
		if (i != freePlayer && player_array[i].state && player_array[i].pairsocket == -1){
			// pair unpaired player with given
			player_array[freePlayer].pairsocket = player_array[i].socket;
			player_array[i].pairsocket = player_array[freePlayer].socket;
			player_array[freePlayer].pairnum = getBySocket( player_array[freePlayer].pairsocket );
			player_array[i].pairnum = getBySocket( player_array[i].pairsocket );
			clearBoard( player_array[freePlayer].board );
			clearBoard( player_array[i].board );			
			return i;
		}	
	}
	return -1;
}

// change player state to IDLE
void removePlayer(int num){
	if (player_array[num].state)	{
		player_array[num].state = IDLE;
		if(safe_close(player_array[num].socket) < 0) ERR("close");
			fprintf(stderr,"Player: %s left the game\n", player_array[num].name);
	}
}

// returns platers array pos or -1 on error
int getBySocket(int socketfd){
	int i;
	for (i = 0; i < MAX_player_array; i++){
		if (player_array[i].state && player_array[i].socket == socketfd){
			return i;
		}
	}
	return -1;
}

// returns player_array array pos or -1 on error
int getByPID(pid_t pid){
	int i;
	for (i = 0; i < MAX_player_array; i++){
		if (player_array[i].state 
			&& player_array[i].pid == pid){		
			return i;
		}
	}
	return -1;
}

// sends the board to a player_array
void sendBoard( int socket, char b[] ){
	char data[MAX_LEN];
	int i;
	for(i=0; i<5; i++){
		if(i<2){
			sprintf(data, "%c | %c | %c | %c | %c\t 0%d | 0%d | 0%d | 0%d | 0%d", b[i*5], b[i*5+1], b[i*5+2], b[i*5+3], b[i*5+4],i*5, i*5+1, i*5+2, i*5+3, i*5+4);
		} else {
			sprintf(data, "%c | %c | %c | %c | %c\t %d | %d | %d | %d | %d", b[i*5], b[i*5+1], b[i*5+2], b[i*5+3], b[i*5+4],i*5, i*5+1, i*5+2, i*5+3, i*5+4);
		}
		if(bulk_write(socket,data,MAX_LEN)<0) ERR("sendBoard");
	}
	sprintf( data, "You can make move by typing number from 00 to 24.\nChat all simply by preciding your message with @ or chat directly to your opponent");
	if(bulk_write(socket,data,MAX_LEN)<0) ERR("sendBoard2");	
}

// clears gaming board with EMPTYCELL symbol
void clearBoard(char b[])
{
	int i;
	for (i = 0; i < 25; i++)
		b[i] = EMPTYCELL;
	
}

// send text via socket
void sendText( int socket, char* str ){
	char data[MAX_LEN];
	strcpy( data, str );
	if(bulk_write(socket,data,MAX_LEN)<0) ERR("sendText");	
}

// sends null to split strings
void sendSplit( int socket ){
	sendText( socket, "\0" );
}

// lock sempahore
void lockSemaphore(int sn){
	// allocate resource
	struct sembuf sb = {sn, -1, 0};  
	if (-1 == semop(semid, &sb, 1)) ERR("semop");
}

// free semaphor
void unlockSemaphore(int sn)
{
	// allocate resource
	struct sembuf sb = {sn, -1, 0};  
	// resource available
	sb.sem_op = 1; 
	if (-1 == semop(semid, &sb, 1) ) ERR("semop");
}

// create shared memory returns pointer to shared memory block
void sharedMemoryInit(){
	key_t key;
    int shmid;
	
	// path to key_t identifier
    if (-1 == (key = ftok("client.c", 'a'))) ERR("ftok");     
	// read all, write only owner
    if (-1 == (shmid = shmget(key, sizeof(player_struct)*MAX_player_array, 0644 | IPC_CREAT))) ERR("shmget");
	// attaches the shared memory segment to the data segment of the calling process shmat(id, addr, flgs)    
    if ((player_struct *)(-1) == (player_array = shmat(shmid, (void *)0, 0)) ) ERR("shmat");
}

// detach shared memory
void removeSharedMem(){

	// detaches the shared memory segment
	if (-1 == shmdt(player_array)) ERR("shmdt");
}

// create semaphors
void semaphorInit(){
	key_t key;
    union semun arg;
    	
	// path to key_t identifier
    if (-1 == (key = ftok("client.c", 'a')) ) ERR("ftok");
	// read & write for all, no execute
    if (-1 == (semid = semget(key, 2, 0666 | IPC_CREAT))) ERR("semget");
    arg.val = 1;
	// set val of both semaphors from group semid
    if (-1 == (semctl(semid, 0, SETVAL, arg) ) || (-1 == semctl(semid, 1, SETVAL, arg) )) ERR("semctl");
}

// destroy semaphors
void semaphorRem(){
	union semun arg;
    if (-1 == semctl(semid, 0, IPC_RMID, arg)) ERR("semctl");	
}


int serverInit(char* port, FILE* fLog){
	int flags_mod;
	int socketfd;
	// ignore sigpipe
	if(sethandler(SIG_IGN,SIGPIPE)) ERR("Seting SIGPIPE:");
	
	// pass SIGCHLD to proper function
	if(sethandler(sigchld_handler,SIGCHLD)) ERR("Setting parent SIGCHLD:");
	
	// pass SIGINT to proper function
	if(sethandler(sigint_handler,SIGINT)) ERR("Seting SIGINT:");
	
	// set socket
	socketfd = bind_inet_socket(atoi(port),SOCK_STREAM);
	
	// get flags
	flags_mod = fcntl(socketfd, F_GETFL) | O_NONBLOCK;
	
	// update flags
	fcntl(socketfd, F_SETFL, flags_mod);
	
	// setup log
	if (NULL == (fLog = fopen(LOGFILE, "a+"))) ERR("fopen");	
			
	sharedMemoryInit();        
	semaphorInit();      
	setupplayer_array();
	
	return socketfd;
}

int main(int argc, char** argv){  
	FILE* fLog;
	int socketfd;	
	
	pid_t pid;
	
	// check arguments
	if(argc!=2){
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	
	socketfd = serverInit(argv[1], fLog);
	mainServerProcess(socketfd,fLog);
	clearplayer_array();

	
	do	{
		pid = waitpid(0, NULL, 0); 
	}
	while (pid  > 0);

	if(safe_close(socketfd) < 0) ERR("close");
	if(safe_close(pipes[0]) < 0) ERR("close");
	if(safe_close(pipes[1]) < 0) ERR("close");
	removeSharedMem();    
	semaphorRem();
	if(0 != fclose(fLog)) ERR("fclose");

	fprintf(stderr,"Serwer zakonczyl prace.\n");
	return EXIT_SUCCESS;
}