/*******************************************************************************
 * 
 * Chat application
 * 
 * 1. This application can be started in active and passive mode-
 * Passive mode-
 *    ./chat_application --passive --port 3000
 * Active mode-
 *    ./chat_application --active --peer localhost --port 3000
 * 
 * 2. To end the chat session type and press Ctrl + c
 * 
 * 3. The length field contains the length of the packet after the length field.
 *  
 *
 * ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <pthread.h>
#include <netdb.h>
#include <semaphore.h>
#include <signal.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>


/*  Macros of argument validation functions.  */
#define HOSTSIZE 50
#define PORTSIZE 6
#define MAXPORT 65535
#define MINPORT 1
#define HOSTNAME_DELIMITER 58 /* ASCII value of colon. */

/*  Macros for Tcp.  */
#define QUEUE_SIZE 5

/*  Macro defining max length for myName and Friend name. */
#define MAXNAME 40

/* Macro defining maximum buffer size. */
#define BUFFSIZE 65534

#define READTIMEOUT_SEC 900

/*  Macros for chat packet.  */
#define HEADERSIZE 3
#define LENGTH_FIELD_SIZE 2
#define OPCODE_FIELD_SIZE 1
#define MAXTEXTSIZE 65533
#define OP_NAME 1
#define OP_TEXT 2
#define OP_BYE 3

typedef enum {active,passive,undefined} AppMode;

typedef struct packet
{
    short int Length;
    char Opcode;
    char *Text;
}packet;


char myName[MAXNAME];
char friendName[MAXNAME];
char msgBuffer[BUFFSIZE];
pthread_t recvT,sendT;
sem_t sem;


void processArgs(int,char**,AppMode*,int*,char**);
void extractArgs(int,char**,AppMode*,char**,char**);
int validatePort(const char*);
int validateHost(const char*);
void validateArgs(AppMode,char*,char*,int*);
void invalidArgs(const char*);


int passiveSock(int);
int activeSock(char*,int);
void closeSocket(int,char *);


void passiveApp(int);
void activeApp(char*,int);
void chatSession(int);


void* receiver(void*);
void* sender(void*);


int writePacket(int, const packet*);
int readPacket(int,packet*);


void sendNameMsg(int);
void sendByeMsg(int);
void sendTextMsg(int,char*);


int getFrndName(int);
void setMyNameIfNotSet();
void setMyName();


int readMsgFromUser(char *);
void sessionKiller(int);
void displayMsg(packet);


int main(int argc,char **argv)
{
    AppMode mode;
    int port=0;
    char *peerHost=NULL;
    mode = undefined;
    
    processArgs(argc,argv,&mode,&port,&peerHost);
    #ifdef DEBUG
    printf("\nIn debug mode.");
    #endif
    if (mode == active)
    {
        activeApp(peerHost,port);
    }
    else if (mode == passive)
    {
        passiveApp(port);   
    }
    
    return EXIT_SUCCESS;
}

void activeApp(char *peerHost,int peerPort)
{
    int sock;
    sock = activeSock(peerHost,peerPort);
    chatSession(sock);    
    closeSocket(sock,"Error while closing socket:");
}

void passiveApp(int port)
{
    int sock,value=1;
    struct timeval timeout;
    setMyName();
    sock = passiveSock(port);
    while(1)
    {
        int newSock,size;
        struct sockaddr_in clientAddr;

        printf("\n Waiting for new connection...");
        fflush(stdout);
        if((newSock = accept(sock,(struct sockaddr*)&clientAddr,(socklen_t*)&size)) == -1)
        {
            perror("\nError during connection accept: ");
            exit(EXIT_FAILURE);
        }
        if(setsockopt(newSock,IPPROTO_TCP,TCP_NODELAY,&value,sizeof(int)) == -1)
        {
            perror("\nError during setting TCP_NODELAY socket options:");
            exit(EXIT_FAILURE);
        }
        timeout.tv_sec = READTIMEOUT_SEC;
        timeout.tv_usec = 1;
        if (setsockopt(newSock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof (timeout)) == -1)
        {
            perror("\nError during setting of socket options:");
        }
        chatSession(newSock);
        closeSocket(newSock,"Error while closing socket:");
    }
    closeSocket(sock,"Error while closing socket:");
    
}

void chatSession(int newSock)
{
    int res;
    void *sendT_result,*recvT_result;
    
    sendT_result = NULL;
    recvT_result = NULL;
    
    signal(SIGINT,sessionKiller);
    if(sem_init(&sem,0,0) != 0)
    {
        perror("Error during semaphore initialization.");
        exit(EXIT_FAILURE);
    }
    
    if((res = pthread_create(&recvT,NULL,receiver,(void*)&newSock)) != 0)
    {
        fprintf(stderr,"\nThread creation failed : %s",strerror(res));
        exit(EXIT_FAILURE);
    }

    if((res = pthread_create(&sendT,NULL,sender,(void*)&newSock)) != 0)
    {
        fprintf(stderr,"\nThread creation failed : %s",strerror(res));
        exit(EXIT_FAILURE);
    }
    if((res = pthread_join(recvT,(void**)&recvT_result)) != 0)
    {
        perror("Recv Thread join failed:");
        exit(EXIT_FAILURE);
    }
    #ifdef DEBUG
        printf("\n[chatSession] Recv thread ended.");
        fflush(stdout);
    #endif
    if((res = pthread_join(sendT,(void**)&sendT_result)) != 0)
    {
        perror("Send Thread join failed:");
        exit(EXIT_FAILURE);
    }
    #ifdef DEBUG
        printf("\n[chatSession] Send thread ended.");
        fflush(stdout);
    #endif
    if(recvT_result == PTHREAD_CANCELED)
    {
        sendByeMsg(newSock);
        printf("\n Closing chat session with %s\n",friendName);
        fflush(stdout);
    }
    else
    {
        signal(SIGINT,SIG_DFL);
        free(recvT_result);
        
    }
}




/******************************************************************************
 
 *                       Thread functions.
 
 ******************************************************************************/

void* receiver(void *newSock)
{
    int *retval;
    int sock = *(int*)newSock;
    #ifdef DEBUG
        printf("\n[receiver] Waiting for Friend's name.");
        fflush(stdout);
    #endif
    
    if(getFrndName(sock) != 0)
    {
        fprintf(stderr, "\n Failed to receive name.");
        if(pthread_cancel(sendT) != 0)
        {
            perror("Thread cancellation failed.");
            exit(EXIT_FAILURE);
        }
        retval = (int*)malloc(sizeof(int));
        if(retval == NULL)
        {
            perror("Failed during memory allocation:");
            exit(EXIT_FAILURE);                
        }
        *retval = 1;
        pthread_exit(retval);        
    }
    sem_post(&sem);
    #ifdef DEBUG
        printf("\n[receiver] Starting to receive messages");
        fflush(stdout);
    #endif
    
    /*Start receiving.*/
    while(1)
    {
        packet msg;
        #ifdef DEBUG
            printf("\n[receiver] In receiver loop.");
            fflush(stdout);
        #endif
        if(readPacket(sock,&msg) == -1)
        {
            if(pthread_cancel(sendT) != 0)
            {
                perror("Thread cancellation failed.");
                exit(EXIT_FAILURE);
            }
            retval = (int*)malloc(sizeof(int));
            if(retval == NULL)
            {
                perror("Failed during memory allocation:");
                exit(EXIT_FAILURE);                
            }
            *retval = 1;
            pthread_exit(retval);
        }
        #ifdef DEBUG
            printf("\n[receiver] Packet received: %hu %d %s",msg.Length,msg.Opcode,msg.Text);
            fflush(stdout);
        #endif            
        if(msg.Opcode == OP_TEXT)
        {
            displayMsg(msg);
        }
        else if(msg.Opcode == OP_BYE)
        {
            printf("\r%s> Bye      \n",friendName);
            fflush(stdout);
            if(pthread_cancel(sendT) != 0)
            {
                perror("Thread cancellation failed.");
                exit(EXIT_FAILURE);
            }
            retval = (int*)malloc(sizeof(int));
            if(retval == NULL)
            {
                perror("Failed during memory allocation:");
                exit(EXIT_FAILURE);                
            }
            *retval = 1;
            pthread_exit(retval);
        }
        free(msg.Text);
    }
    fflush(stdout);
    pthread_exit(NULL);
}

void* sender(void *newSock)
{
    int sock = *(int*)newSock;
    char *msg;

    setMyNameIfNotSet();
    sendNameMsg(sock);
    #ifdef DEBUG
        printf("\n[sender] myName send.");
        fflush(stdout);
    #endif    
    sem_wait(&sem);
    printf("\n Chat session started with %s\n",friendName);
    fflush(stdout);
    
    msg = (char*)malloc(BUFFSIZE);
    if(msg == NULL)
    {
        perror("Failed during memory allocation:");
        exit(EXIT_FAILURE);
    }
    printf("\n");
    /* Start sending.*/
    while(1)
    {
        memset(msg,0,BUFFSIZE);
        printf("You> ");

        if(readMsgFromUser(msg) == -1)
        {
            pthread_exit(NULL);
        }
        else
            sendTextMsg(sock,msg);
    }
    free(msg);
    
    pthread_exit(NULL);
}


/******************************************************************************
 
 *                Functions handling Name.
 
 ******************************************************************************/

int getFrndName(int sock)
{
    packet pt;
    if(readPacket(sock,&pt) == -1)
    {
        return -2;
    }
    #ifdef DEBUG
        printf("\n[getFrndName]Name packet received: ");
        printf("%hu %d %s",pt.Length,pt.Opcode,pt.Text);
        fflush(stdout);
    #endif
    if(pt.Opcode == OP_NAME)
        strcpy(friendName,pt.Text);
    else
        return -1;
    return 0;
}


void setMyNameIfNotSet()
{
    if(myName[0] == 0) /* MyName empty. */
    {
        setMyName();
    }
}

void setMyName()
{
    printf("\n Enter your name: ");
    scanf("%s",myName);
}

/******************************************************************************
 
 *                Functions to send different packets.
 
 ******************************************************************************/


void sendTextMsg(int sock,char *msg)
{
    packet sendMsg;
    sendMsg.Opcode = OP_TEXT;
    sendMsg.Length = OPCODE_FIELD_SIZE + strlen(msg);
    sendMsg.Text = msg;
    writePacket(sock,&sendMsg);
}

void sendNameMsg(int sock)
{    
    packet pt;
    int nameLen;

    nameLen = strlen(myName);
    pt.Opcode = OP_NAME;
    pt.Length = nameLen + OPCODE_FIELD_SIZE;
    pt.Text = myName;
    writePacket(sock,&pt);
}

void sendByeMsg(int sock)
{
    packet sendMsg;
    sendMsg.Opcode = OP_BYE;
    sendMsg.Length = OPCODE_FIELD_SIZE;
    sendMsg.Text = NULL;
    writePacket(sock,&sendMsg);    
}

/******************************************************************************
 
 *                Chat session IO functions.
 
 ******************************************************************************/

int readPacket(int sock,packet *msg)
{
    int ret,textLen;
    unsigned short int len;
    char *bufPtr;
    
    if((ret = read(sock,&len,sizeof(msg->Length))) == -1)
    {
        perror("Failed to read message:");
        return -1;
    }
    msg->Length = ntohs(len);
    if((ret = read(sock,&(msg->Opcode),sizeof(msg->Opcode))) == -1)
    {
        perror("Failed to read message:");
        return -1;
    }
    textLen = msg->Length - OPCODE_FIELD_SIZE;
    msg->Text = (char*)malloc(textLen+1);
    if(msg->Text == NULL)
    {
	fprintf(stderr,"\nFailed to allocate memory for host.");
	exit(EXIT_FAILURE);        
    }
    memset(msg->Text,0,textLen+1);

    bufPtr = msg->Text;
    while(textLen!=0)
    {
        if((ret = read(sock,bufPtr,textLen)) == -1)
        {
            perror("Failed to read message:");
            return -1;
        }
        textLen-=ret;
        bufPtr+=ret;
    }
    return 0;
}

int writePacket(int sock, const packet *pt)
{
    int textLen,ret;
    unsigned short int pktLen;
    char *bufPtr;

    pktLen = htons(pt->Length);
    textLen = pt->Length - OPCODE_FIELD_SIZE;
    if(write(sock,&pktLen,sizeof(pt->Length)) == -1)
    {
        perror("Write failed while sending Length:");
        return -1;
    }
    
    if(write(sock,&(pt->Opcode),sizeof(pt->Opcode)) == -1)
    {
        perror("Write failed while sending Opcode:");
        return -1;
    }
    
    bufPtr = pt->Text;
    while(textLen != 0)
    {
        if((ret = write(sock,bufPtr,textLen)) == -1)
        {
            perror("Write failed while sending Chat Message:");
            return -1;
        }
        textLen-=ret;
        bufPtr+=ret;
        
    }
    return 0;
}

/******************************************************************************
 
 *                Other utility functions.
 
 ******************************************************************************/

int readMsgFromUser(char *msg)
{
    int i=0;
    char ch;
    
    if(scanf("%c",&ch) == EOF)
    {
        if(errno == EINTR)
        {
            return -1;
        }
    }
    if(ch != 10)
    {
        msg[i] = ch;
        msgBuffer[i++] = ch;
    }
    
    while(i<BUFFSIZE)
    {
        if(scanf("%c",&ch) == EOF)
        {
            if(errno == EINTR)
            {
                return -1;
            }
        }
        if(ch == 10)
        {
            msg[i] = '\0';
            msgBuffer[i] = '\0';
            break;
        }
        else
        {
            msg[i] = ch;
            msgBuffer[i++] = ch;
        }
    }
    return 0;
}

/*  Signal Handler for SIGINT */
void sessionKiller(int signal_val)
{
    if(pthread_cancel(recvT) != 0)
    {
        perror("Thread cancellation failed.");
        fflush(stderr);
        exit(EXIT_FAILURE);
    }
    if(pthread_cancel(sendT) != 0)
    {
        perror("Thread cancellation failed.");
        exit(EXIT_FAILURE);
    }
    signal(SIGINT,SIG_DFL);
}

void displayMsg(packet msg)
{    
    printf("\r%s> %s      ",friendName,msg.Text);
    printf("\nYou> ");
    fflush(stdout);
}


/******************************************************************************
 
 *                Network Utility functions.
 
 ******************************************************************************/


int activeSock(char *host,int port)
{
    int socketd,value;
    struct sockaddr_in address;
    struct in_addr hostname;
    struct hostent *hostaddr;
    struct timeval timeout;
    
    if((socketd = socket(AF_INET,SOCK_STREAM,0)) == -1)
    {
       perror("\nError during socket creation: ");
       exit(EXIT_FAILURE);
    }
    
    if ((hostaddr = gethostbyname(host)) == NULL)
    {
        perror("Failed to get host address:");
        exit(0);
    }
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    hostname.s_addr = *(int*)*(hostaddr->h_addr_list);
    address.sin_addr = hostname;
    
    if(connect(socketd,(struct sockaddr*)&address,(socklen_t)sizeof(address)) == -1)
    {
        perror("\nError during socket connection:");
        exit(EXIT_FAILURE);
    }
    value = 1;
    if(setsockopt(socketd,SOL_SOCKET,SO_REUSEADDR,&value,sizeof(int)) == -1)
    {
        perror("\nError during setting SO_REUSEADDR socket options:");
        exit(EXIT_FAILURE);
    }
    if(setsockopt(socketd,IPPROTO_TCP,TCP_NODELAY,&value,sizeof(int)) == -1)
    {
        perror("\nError during setting TCP_NODELAY socket options:");
        exit(EXIT_FAILURE);
    }
    
    timeout.tv_sec = READTIMEOUT_SEC;
    timeout.tv_usec = 1;
    if (setsockopt(socketd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof (timeout)) == -1)
    {
        perror("\nError during setting of socket options:");
    }

    return socketd;
}

int passiveSock(int port)
{
    int socketd,value;
    struct sockaddr_in address;
    struct in_addr hostname;
    
    if((socketd = socket(AF_INET,SOCK_STREAM,0)) == -1)
    {
       perror("\nError during socket creation: ");
       exit(EXIT_FAILURE);
    }
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    hostname.s_addr = INADDR_ANY;
    address.sin_addr = hostname;
    if(bind(socketd,(struct sockaddr*)&address,sizeof(address)) == -1)
    {
        perror("\nError during socket bind: ");
        exit(EXIT_FAILURE);
    }
    
    if (listen(socketd, QUEUE_SIZE) == -1)
    {
        perror("\nError during socket listen: ");
        exit(EXIT_FAILURE);
    }

    value = 1;
    if(setsockopt(socketd,SOL_SOCKET,SO_REUSEADDR,&value,sizeof(int)) == -1)
    {
        perror("\nError during setting SO_REUSEADDR socket options:");
        exit(EXIT_FAILURE);
    }
    return socketd;
}

void closeSocket(int sd,char *message)
{
    if(close(sd) == -1)
    {
        perror(message);
    }
}

/*******************************************************************************
 
 *      Arguments extraction and validation functions.
 
 ******************************************************************************/

void processArgs(int argc,char **argv,AppMode *mode,int *port,char **peerHost)
{
    char *strPort=NULL;
    extractArgs(argc,argv,mode,&strPort,peerHost);
    validateArgs(*mode,strPort,*peerHost,port);
}

void extractArgs(int argc,char **argv,AppMode *mode,char **port,char **host)
{
    int i;
    for (i=1;i<argc;i++)
    {
	char *argument = argv[i];
	if(!strcmp(argument,"--active"))
	{
	    if(*mode==undefined)
		*mode = active;
	     else
	     {
		invalidArgs("Mode defined more than one time.");
		exit(EXIT_FAILURE);
	     }
	}
	else if(!strcmp(argument,"--passive"))
        {
	    if(*mode==undefined)
	         *mode = passive;
	    else
	    {
		invalidArgs("Mode defined more than one time.");
		exit(EXIT_FAILURE);
	    }
	}
	else if(!strcmp(argument,"--port"))
	{
	    if(++i < argc)
	    {
	        *port = argv[i];
	    }
	    else
	    {
	        invalidArgs("Port missing.");
		exit(EXIT_FAILURE);
	    }
	}
	else if(!strcmp(argument,"--peer"))
	{
	    if(++i < argc)
	    {
		 *host = argv[i];
            }
	    else
	    {
		 invalidArgs("Peer hostname missing.");
		 exit(EXIT_FAILURE);
            }
	}
    }
}

void validateArgs(AppMode mode,char *strPort,char *strPeer,int *port)
{
    if(mode == passive)
    {
	if(strPort == NULL || strPort[0] == 0)
	{
	    invalidArgs("No port specified for passive mode.");
	    exit(EXIT_FAILURE);
	}
	else
	{
            if(validatePort(strPort) && (*port=atoi(strPort)) >= MINPORT && *port <= MAXPORT)
	    {
		/*  Do nothing  */
	    }
	    else
	    {
		invalidArgs("Invalid Port.");
		exit(EXIT_FAILURE);
	    }
	}
    }
    else if(mode == active)
    {
	if(strPeer == NULL || strPeer[0] == 0)
	{
	    invalidArgs("No peer hostname specified for active mode.");
	    exit(EXIT_FAILURE);
	}
	if(strPort == NULL || strPort[0] == 0)
	{
	    invalidArgs("No peer port specified for active mode.");
	    exit(EXIT_FAILURE);
	}
	else
	{
            if(!validateHost(strPeer))
            {
                invalidArgs("Invalid Peer hostname.");
	        exit(EXIT_FAILURE);
            }
	    if(validatePort(strPort) && (*port=atoi(strPort)) >= MINPORT && *port <= MAXPORT)
            {
		/*  Do Nothing   */
            }
	    else
	    {
		invalidArgs("Invalid Peer port.");
		exit(EXIT_FAILURE);
	    }
	}
    }
    else if(mode == undefined)
    {
        invalidArgs("Mode not specified.");
        exit(EXIT_FAILURE);
    }
}


int validatePort(const char *portStr)
{
    int i;
    for(i=0;portStr[i] != '\0';i++)
    {
        if(isdigit(portStr[i]) == 0)
            return 0;
    }
    return 1;
}

int validateHost(const char *hostStr)
{
    if(hostStr == NULL)
        return 0;
    if(hostStr[0] == 0)
	return 0;
    return 1;
}

void invalidArgs(const char*message)
{
    fprintf(stderr,"\n%s",message);
    fprintf(stderr,"\n\nCommand should adhere to the following format:");
    fprintf(stderr,"\n\n     ./chatApp  (--active | --passive) --port XXXX [--peer [IPADDRS | DNSNAME]]\n\n");
}
