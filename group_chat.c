/*******************************************************************************
 * 
 * Group chat application, using multi-cast.
 * 
 * 1. The application can be started as follows-
 * ./groupchat -mcip 224.1.1.1 -port 3000
 * 
 * 2. To leave the group chat press Ctrl+C.
 * 
 * ****************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/ip.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <errno.h>
#include <termios.h>



/*  Macros of argument validation functions.  */
#define MAXPORT 65535
#define MINPORT 1

/* Macro defining maximum buffer size. */
#define BUFFSIZE 65536

/*  Macros for chat packet.  */
#define OPCODE_FIELD_SIZE 1
#define NAMELENGTH_FIELD_SIZE 1
#define TEXTLENGTH_FIELD_SIZE 2
#define OP_TEXT 1
#define OP_BYE 2
#define FIXEDLENGTH_FIELDS_SIZE 4
#define MAX_NAME_LENGTH 255
#define MAX_TEXT_LENGTH 65535
#define MAX_PACKET_LENGTH FIXEDLENGTH_FIELDS_SIZE + MAX_NAME_LENGTH + MAX_TEXT_LENGTH



typedef struct packet
{
    char Opcode;
    char NameLength;
    char *Name;
    unsigned short int TextLength;
    char *Text;
}packet;

char myName[MAX_NAME_LENGTH+1];
pthread_t recvT,sendT;
pthread_mutex_t bufferLock;
struct sockaddr_in multicastAddr;
char msgBuffer[BUFFSIZE];


void startGroupChat(struct in_addr,int);
void chatSession(int);

/* Thread functions. */
void* receiver(void*);
void* sender(void*);

/* Functions to send different packets. */
void sendTextMsg(int,char*);
void sendByeMsg(int);

/* Read packet functions. */
int readPacket(int,packet *);
int getTextLength(packet *,char **,int *);
int getOpcode(packet *,char **,int *);
int getNameLength(packet *,char **,int *);
int getName(packet *,char **,int *);
int getText(packet *,char **,int *);

/* Write packet functions. */
int writePacket(int, const packet *);
void setTextLength(const packet *,char **);
void setOpcode(const packet *,char **);
void setNameLength(const packet *,char **);
void setName(const packet *,char **);
void setText(const packet *,char **);

/* Other Utility function. */
char getch();
int readMsg(char*);
void setMyName();
void sessionKiller(int);

/* Display functions. */
void displayMsg(packet);
void displayBye(packet);
void restoreDisplay();

/* Network Utility functions. */
void getBinaryAddress(char*, struct in_addr*);
int getMultiCastSock(struct in_addr, int);
void closeSocket(int,char *);
void leaveGroup(int);

/* Argument validation functions. */
void processArgs(int,char**,char**,int*);
void extractArgs(int,char**,char**,char**);
void validateArgs(const char*,const char*,int*);
int validateAndGetPort(const char*);
int validateHost(const char*);
void invalidArgs(const char*);


int main(int argc, char **argv)
{
    char *multiIp=NULL;
    int port=0;
    struct in_addr multicastIp;

    processArgs(argc,argv,&multiIp,&port);
    getBinaryAddress(multiIp,&multicastIp);
    multicastAddr.sin_family = AF_INET;
    multicastAddr.sin_addr.s_addr = multicastIp.s_addr;
    multicastAddr.sin_port = htons(port);
    startGroupChat(multicastIp,port);
}

void startGroupChat(struct in_addr multicastIp,int port)
{
    int sock;
    sock = getMultiCastSock(multicastIp,port);
    setMyName();
    chatSession(sock);
    leaveGroup(sock);
    closeSocket(sock,"Error while closing socket:");
}


void chatSession(int newSock)
{
    int res;
    void *sendT_result,*recvT_result;
    
    sendT_result = NULL;
    recvT_result = NULL;
    
    pthread_mutex_init(&bufferLock,NULL);
    signal(SIGINT,sessionKiller);
    
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
    if((res = pthread_join(sendT,(void**)&sendT_result)) != 0)
    {
        perror("Send Thread join failed:");
        exit(EXIT_FAILURE);
    }

    if(recvT_result == PTHREAD_CANCELED)
    {
        sendByeMsg(newSock);
        printf("\n\n    Leaving group chat\n");
        fflush(stdout);
        restoreDisplay();
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
    int *retval,readReturnVal;
    int sock = *(int*)newSock;
    
    /*Start receiving.*/
    while(1)
    {
        packet msg;

        if((readReturnVal = readPacket(sock,&msg)) == -1)
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
        else if(readReturnVal == -2)
        {
	    printf("\rYou> ");
	    fflush(stdout);
            continue;
        }
           
        if(msg.Opcode == OP_TEXT)
        {
            displayMsg(msg);
	    free(msg.Text);
        }
        else if(msg.Opcode == OP_BYE)
        {
            displayBye(msg);
        }
	free(msg.Name);
    }
    fflush(stdout);
    pthread_exit(NULL);

}

void* sender(void *newSock)
{
    int sock = *(int*)newSock;
    char *msg;

    printf("\n Chat session started with group\n");
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
        printf("You> ");
        fflush(stdout);
        
        memset(msg,0,BUFFSIZE);

        if(readMsg(msg) == -1)
        {
            free(msg);
            pthread_exit(NULL);
        }
        else
        {
            sendTextMsg(sock,msg);
        }
    }
    free(msg);    
}


/******************************************************************************
 
 *                Functions to send different packets.
 
 ******************************************************************************/


void sendTextMsg(int sock,char *text)
{
    packet pkt;
    
    pkt.Opcode = OP_TEXT;
    pkt.NameLength = strlen(myName);
    pkt.Name = myName;
    pkt.TextLength = strlen(text);
    pkt.Text = text;
    writePacket(sock,&pkt);
}

void sendByeMsg(int sock)
{
    packet pkt;
    
    pkt.Opcode = OP_BYE;
    pkt.NameLength = strlen(myName);
    pkt.Name = myName;
    pkt.TextLength = 0;
    pkt.Text = NULL;
    writePacket(sock,&pkt);    
}


/******************************************************************************
 
 *                Write packet functions.
 
 ******************************************************************************/

int writePacket(int sock, const packet *msg)
{
    int totalLen,ret;
    char *bufPtr,*iterator;
    
    if(msg->Opcode == OP_TEXT )
    {
        totalLen = FIXEDLENGTH_FIELDS_SIZE + msg->NameLength + msg->TextLength;
    
        if((bufPtr = (char*)malloc(totalLen)) ==  NULL)
        {
	    fprintf(stderr,"\nFailed to allocate memory for host.");
	    exit(EXIT_FAILURE);         
        }
        iterator = bufPtr;
        memset(iterator,0,totalLen);
    
        setOpcode(msg,&iterator);
        setNameLength(msg,&iterator);
        setName(msg,&iterator);    
        setTextLength(msg,&iterator);
        setText(msg,&iterator);
    }
    else if(msg->Opcode == OP_BYE)
    {
        totalLen = OPCODE_FIELD_SIZE + msg->NameLength;
        if((bufPtr = (char*)malloc(totalLen)) ==  NULL)
        {
	    fprintf(stderr,"\nFailed to allocate memory for host.");
	    exit(EXIT_FAILURE);         
        }
        iterator = bufPtr;
        memset(iterator,0,totalLen);
        
        setOpcode(msg,&iterator);
        setName(msg,&iterator);
    }
    
    if((ret = sendto(sock,bufPtr,totalLen,0,(struct sockaddr*)&multicastAddr,sizeof(multicastAddr))) == -1)
    {
        perror("\nPacket sent failed");
        return -1;
    }
    free(bufPtr);

    return 0;
}

void setTextLength(const packet *msg,char **buffer)
{
    unsigned short int length;
    length = htons(msg->TextLength);
    memcpy(*buffer,(char*)&length,sizeof(length));
    *buffer = *buffer + sizeof(length);
}

void setOpcode(const packet *msg,char **buffer)
{
    char opcode;
    opcode = msg->Opcode;
    memcpy(*buffer,&opcode,sizeof(opcode));
    *buffer = *buffer + sizeof(opcode);
}

void setNameLength(const packet *msg,char **buffer)
{
    char nlength;
    nlength = msg->NameLength;
    memcpy(*buffer,&nlength,sizeof(nlength));
    *buffer = *buffer + sizeof(nlength);
}

void setName(const packet *msg,char **buffer)
{
    memcpy(*buffer,msg->Name,msg->NameLength);
    *buffer = *buffer + msg->NameLength;
}

void setText(const packet *msg,char **buffer)
{
    memcpy(*buffer,msg->Text,msg->TextLength); 
}

/******************************************************************************
 
 *                Read packet functions.
 
 ******************************************************************************/

int readPacket(int sock,packet *msg)
{
    int ret,pktLen;
    char *bufPtr,*iterator;
    
    if((bufPtr = (char*)malloc(MAX_PACKET_LENGTH)) == NULL)
    {
	fprintf(stderr,"\nFailed to allocate memory for host.");
	exit(EXIT_FAILURE); 
    }
    iterator = bufPtr;
    
    if((ret = read(sock,bufPtr,MAX_PACKET_LENGTH)) == -1)
    {
        perror("Failed to read message:");
        return -1;
    }
    pktLen = ret;
    
    if(getOpcode(msg,&iterator,&pktLen) == -1)
        return -2;
    if(msg->Opcode == OP_TEXT)
    {
        if(getNameLength(msg,&iterator,&pktLen) == -1)
            return -2;
        if(getName(msg,&iterator,&pktLen) == -1)
            return -2;
        if(getTextLength(msg,&iterator,&pktLen) == -1)
            return -2;
        if(getText(msg,&iterator,&pktLen) == -1)
            return -2;
    }
    else if(msg->Opcode == OP_BYE)
    {
        msg->NameLength = ret - OPCODE_FIELD_SIZE;
        if(getName(msg,&iterator,&pktLen) == -1)
            return -2;
    }
    free(bufPtr);
    
    return 0;
}

int getTextLength(packet *msg,char **buffer,int *pktLen)
{
    unsigned short int length;
    
    if(*pktLen >= TEXTLENGTH_FIELD_SIZE)
    {
        memcpy((char*)&length,*buffer,sizeof(length));
        msg->TextLength = ntohs(length);
        *buffer = *buffer + sizeof(msg->TextLength);
        *pktLen = *pktLen - sizeof(msg->TextLength);
        return 0;
    }
    else
    {
        fprintf(stderr,"\nInvalid incoming message:Missing Text Length field.\n");
        return -1;
    }
}

int getOpcode(packet *msg,char **buffer,int *pktLen)
{
    char opcode;
    if(*pktLen >= OPCODE_FIELD_SIZE)
    {
        memcpy(&opcode,*buffer,sizeof(opcode));
        msg->Opcode = opcode;
    
       *buffer = *buffer + sizeof(opcode);
       *pktLen = *pktLen - sizeof(opcode);
       return 0;
    }
    else
    {
        fprintf(stderr,"\nInvalid incoming message:Missing Opcode.\n");
        return -1;
    }
    
}

int getNameLength(packet *msg,char **buffer,int *pktLen)
{
    char nlength;
    if(*pktLen >= NAMELENGTH_FIELD_SIZE)
    {
        memcpy(&nlength,*buffer,sizeof(nlength));    
        msg->NameLength = nlength;
        *buffer = *buffer + sizeof(nlength);
        *pktLen = *pktLen - sizeof(nlength);
        return 0;
    }
    else
    {
        fprintf(stderr,"\nInvalid incoming message:Missing NameLength field.\n");
        return -1;
    }
}

int getName(packet *msg,char **buffer,int *pktLen)
{
    if(*pktLen >= msg->NameLength)
    {
        msg->Name = (char*)malloc(msg->NameLength + 1);
        if(msg->Name == NULL)
        {
	    fprintf(stderr,"\nFailed to allocate memory for host.");
	    exit(EXIT_FAILURE);         
        }
        memset(msg->Name,0,msg->NameLength + 1);
        memcpy(msg->Name,*buffer,msg->NameLength);
    
        *buffer = *buffer + msg->NameLength;
        *pktLen = *pktLen - msg->NameLength;
        return 0;
    
    }
    else
    {
        fprintf(stderr,"\nInvalid incoming message:The NameLength field "
        "advertises %d bytes but actually is  %d bytes.\n",msg->NameLength,*pktLen);
        return -1;
    }
}

int getText(packet *msg,char **buffer,int *pktLen)
{
    if(*pktLen >= msg->TextLength)
    {
        msg->Text = (char*)malloc(msg->TextLength + 1);
        if(msg->Text == NULL)
        {
            fprintf(stderr,"\nFailed to allocate memory for host.");
	    exit(EXIT_FAILURE);         
        }
        memset(msg->Text,0,msg->TextLength + 1);
        memcpy(msg->Text,*buffer,msg->TextLength);
        *pktLen = *pktLen - msg->TextLength;
        return 0;
    }
    else
    {
        fprintf(stderr,"\nInvalid incoming message:The TextLength field "
        "advertises %d bytes but actually is %d bytes\n",msg->TextLength,*pktLen);
        return -1;
    }
}


/******************************************************************************
 
 *                Other Utility functions.
 
 ******************************************************************************/
char getch()
{
    char buf = 0;
    struct termios old = {0};
    if (tcgetattr(0, &old) == -1)
    {
        perror("Error while fetching stdin properties:");
        exit(EXIT_FAILURE);
    }
    old.c_lflag &= ~ICANON;
    old.c_lflag &= ~ECHO;
    old.c_cc[VMIN] = 1;
    old.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &old) == -1)
    {
        perror("Error while setting stdin properties:");
        exit(EXIT_FAILURE);
    }
    if (read(0, &buf, 1) == -1)
    {
        perror ("Error while reading character from stdin:");
        exit(EXIT_FAILURE);
    }
    old.c_lflag |= ICANON;
    old.c_lflag |= ECHO;
    if (tcsetattr(0, TCSADRAIN, &old) == -1)
    {
        perror("Error while setting stdin properties:");
        exit(EXIT_FAILURE);
    }
    return (buf);
}

int readMsg(char *msg)
{
    int i=0;
    char ch=0;
    
    while(i < BUFFSIZE-1)
    {
        ch = getch();
        if(ch == 10)
        {
            printf("%c",ch);
            break;
        }
        else if(ch == 127 && i > 0)
        {
            printf("\033[1D");
            printf(" ");
            printf("\033[1D");
            i--;
            pthread_mutex_lock(&bufferLock);
                msgBuffer[i] = 0;
            pthread_mutex_unlock(&bufferLock);
        }
        else if(ch >= 32 && ch <= 126)
        {
            msg[i] = ch;
            pthread_mutex_lock(&bufferLock);
               msgBuffer[i++] = ch;
               msgBuffer[i] = 0;
            pthread_mutex_unlock(&bufferLock);
            printf("%c",ch);
        }
        fflush(stdout);
    }
    pthread_mutex_lock(&bufferLock);
        msgBuffer[0] = 0;
    pthread_mutex_unlock(&bufferLock);
    msg[i] = 0;
    return i;
}

void setMyName()
{
    printf("\n Enter your name: ");
    scanf("%s",myName);
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

/******************************************************************************
 
 *                Display functions.
 
 ******************************************************************************/

void displayMsg(packet msg)
{    
    printf("\r%s> %s",msg.Name,msg.Text);
    printf("\033[K");
    pthread_mutex_lock(&bufferLock);
        printf("\nYou> %s",msgBuffer);
    pthread_mutex_unlock(&bufferLock);
    fflush(stdout);
}

void displayBye(packet msg)
{
    printf("\r%s> Bye",msg.Name);
    printf("\033[K");
    printf("\n\n");
    printf("    %s left the group\n",msg.Name);
    pthread_mutex_lock(&bufferLock);
        printf("\nYou> %s",msgBuffer);
    pthread_mutex_unlock(&bufferLock);
    fflush(stdout);
}

void restoreDisplay()
{
    struct termios old = {0};
    if (tcgetattr(0, &old) == -1)
    {
        perror("Error while fetching stdin properties:");
        exit(EXIT_FAILURE);
    }
    old.c_lflag |= ICANON;
    old.c_lflag |= ECHO;
    if (tcsetattr(0, TCSADRAIN, &old) == -1)
    {
        perror("Error while setting stdin properties:");
        exit(EXIT_FAILURE);
    }    
}
/******************************************************************************
 
 *                Network Utility functions.
 
 ******************************************************************************/


void getBinaryAddress(char *hostname,struct in_addr *hostaddress)
{
    struct hostent *hostaddr;

    if ((hostaddr = gethostbyname(hostname)) == NULL)
    {
        perror("Error during hostname resolution");
        exit(EXIT_FAILURE);
    }
    hostaddress->s_addr = *(int*)*(hostaddr->h_addr_list);
}


int getMultiCastSock(struct in_addr multicastIp, int port)
{
    int socketd,value;
    struct sockaddr_in bindAddr;
    struct ip_mreq multiProp;

    if ((socketd = socket(AF_INET,SOCK_DGRAM,0)) == -1)
    {	
	perror("Error during multicast socket creation");
        exit(EXIT_FAILURE);
    }

    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(port);
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    if(bind(socketd,(struct sockaddr*)&bindAddr,sizeof(bindAddr)) == -1)
    {
	perror("Error during socket bind.");
	exit(EXIT_FAILURE);
    }

    value = 1;
    if(setsockopt(socketd,SOL_SOCKET,SO_REUSEADDR,(char*)&value,sizeof(value)) == -1)
    {
        perror("\nError during setting SO_REUSEADDR socket options:");
        exit(EXIT_FAILURE);
    }

    multiProp.imr_multiaddr.s_addr = multicastIp.s_addr;
    multiProp.imr_interface.s_addr = INADDR_ANY;

    if(setsockopt(socketd,IPPROTO_IP,IP_ADD_MEMBERSHIP,(char*)&multiProp,sizeof(multiProp)) == -1)
    {
        perror("\nError during setting IP_ADD_MEMBERSHIP socket options:");
        exit(EXIT_FAILURE);
    }

    value = 0;
    if(setsockopt(socketd,IPPROTO_IP,IP_MULTICAST_LOOP,&value,sizeof(value)) == -1)
    {
        perror("\nError during setting IP_MULTICAST_LOOP socket options:");
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

void leaveGroup(int sd)
{
    struct ip_mreq multiProp;
    multiProp.imr_multiaddr.s_addr = multicastAddr.sin_addr.s_addr;
    multiProp.imr_interface.s_addr = INADDR_ANY;
    
    if(setsockopt(sd,IPPROTO_IP,IP_DROP_MEMBERSHIP,(char*)&multiProp,sizeof(multiProp)) == -1)
    {
        perror("\nError while leaving the group");
        exit(EXIT_FAILURE);
    }
}

/*******************************************************************************

 *      Arguments extraction and validation functions.

 ******************************************************************************/

void processArgs(int argc, char **argv, char **multiIp, int *port)
{
    char *portStr=NULL;
    extractArgs(argc,argv,multiIp,&portStr);
    validateArgs(*multiIp,portStr,port);
}

void extractArgs(int argc, char **argv, char **multiIp, char **port)
{
    int i;
    for (i=1;i<argc;i++)
    {
	char *argument = argv[i];
        if(!strcmp(argument,"-port"))
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
	else if(!strcmp(argument,"-mcip"))
	{
	    if(++i < argc)
	    {
		 *multiIp = argv[i];
            }
	    else
	    {
		 invalidArgs("Multicast IP address missing.");
		 exit(EXIT_FAILURE);
            }
	}
        else
        {
	   invalidArgs("Invalid arguments");
	   exit(EXIT_FAILURE);
	}   
    }
}


void validateArgs(const char *multiIp,const char *strPort,int *port)
{
    if(multiIp == NULL || multiIp[0] == 0)
    {
        invalidArgs("Multicast IP not specified.");
        exit(EXIT_FAILURE);
    }
    if(strPort == NULL || strPort[0] == 0)
    {
        invalidArgs("No port specified.");
        exit(EXIT_FAILURE);
    }
    if(validateHost(multiIp) == -1)
    {
        invalidArgs("Invalid Peer hostname.");
        exit(EXIT_FAILURE);
    }
    if((*port = validateAndGetPort(strPort)) == -1)
    {
        invalidArgs("Invalid Peer port.");
        exit(EXIT_FAILURE);
    }
}

int validateAndGetPort(const char *portStr)
{
    int i,port;
    for(i=0;portStr[i] != '\0';i++)
    {
        if(isdigit(portStr[i]) == 0)
            return -1;
    }
    port = atoi(portStr);
    if(port >= MINPORT && port <= MAXPORT)
	return port;
    return -1;
}

int validateHost(const char *hostStr)
{
    if(hostStr == NULL)
        return -1;
    if(hostStr[0] == 0)
	return -1;
    return 0;
}

void invalidArgs(const char*message)
{
    fprintf(stderr,"\n%s",message);
    fprintf(stderr,"\n\nCommand should adhere to the following format:");
    fprintf(stderr,"\n\n     ./groupChat -mcip x.x.x.x -port XX\n\n");
}
