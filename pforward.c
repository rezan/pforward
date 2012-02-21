/*
 *  pForward
 *
 *  Copyright (C) 2003-2005  Reza Naghibi
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/time.h>

//defines
#define VERSION		"2005-11-06"
#define DEFAULTCON	10
#define BUFLEN		2500
#define CONFIG		"pforward.ini"
#define OUTFILE		"pforward.txt"
#define STATWAIT	3600
#define _REENTRANT

//globals
static int kills=0;
static int done=0;
static int commands=0;
static int errors=0;
static int TOTAL=0;
static int connections=DEFAULTCON;
static int statwait=STATWAIT;
static int proxies=0;
static struct timeval ts;
static FILE *fout;

//socket waiting queue
typedef struct
{
	struct socketnode *next;
	int s;
	int id;
} socketnode;

//connection structure
typedef struct
{
	int id;
	char outaddr[128];
	int outport;
	int inport;
	char fname[128];
	pthread_t wait_t;
} connectionnode;
static connectionnode *cn=NULL;

//head and tail for waiting queue
static socketnode *head=NULL,*tail=NULL;

//mutexs and conds
static pthread_mutex_t queue;
static pthread_cond_t ready;

//prototypes
void *serve(void*);
void add(int,int);
int getsocket(int*);
int size();
int sizen();
void *waitForConnection(void*);
void quit();
void sigkill(int);
void sigignor(int);
void KILL();
int service(int,int);
int relaysocks(int,int);
void stat1();
void stats();
void getsettings();
int makeConnection(char*,int);
int msleep(int);
void logfunc(char*,char*,char*,FILE*);
void handle_signal(int sig, void (*handler)(int));
void getsocketname(int,char*);
char *gettimestamp();
void chomp(char*);
char *_tolower(char*);

//code

int main(int argc, char *argv[])
{
	unsigned long i;
	pthread_t serv_t;

	if(!(fout=fopen(OUTFILE,"w")))
		fout=stderr;

	fprintf(fout,"%s\nPFORWARD server is running\n",gettimestamp());
	printf("%s\nPFORWARD server is running\n",gettimestamp());
	gettimeofday(&ts,NULL);

	signal(SIGINT,sigkill);
	signal(SIGPIPE,sigignor);
	handle_signal(SIGPIPE, SIG_IGN);

	pthread_mutex_init(&queue,NULL);
	pthread_cond_init(&ready,NULL);

	getsettings();

	if(!proxies)
	{
		fprintf(fout,"No pforward entries, exiting\n");
		return 0;
	}

	//create the worker threads
	for(i=1;i<=connections;i++)
		pthread_create(&serv_t,NULL,serve,(void*)i);
	TOTAL=connections;

	fprintf(fout,"Max connections: %d\n",connections);
	for(i=0;i<proxies;i++)
		//create MASTER threads
		pthread_create(&(cn[i].wait_t),NULL,waitForConnection,(void*)i);

	fflush(fout);

	while(statwait)
	{
		msleep(statwait*1000);
		stats();
	}

	for(i=0;i<proxies;i++)
		pthread_join(cn[i].wait_t,NULL);

	return 0;
}

void getsettings()
{
	FILE *f;
	char buf[128];
	char *token;
	char *c;
	int tport1,tport2;
	char *taddr;

	if(!(f=fopen(CONFIG,"r")))
	{
		fprintf(fout,"ERROR: cannot open config file %s\n",CONFIG);
		return;
	}

	while(fgets(buf,128,f))
	{
		if(!strlen(buf))
			continue;
		chomp(buf);
		_tolower(buf);
		token=strtok(buf,"=");
		if(!token)
			continue;
		else if(!strcmp(token,"maxconnections"))
		{
			connections=strtol((c=strtok(NULL,"="))?c:"",NULL,10);
			if(!connections)
				continue;
			if(connections<1)
				connections=DEFAULTCON;
			if(connections>200)
				connections=200;
		}
		else if(!strcmp(token,"statwait"))
			statwait=strtol((c=strtok(NULL,"="))?c:"",NULL,10);
		else if(!strcmp(token,"pforward"))
		{
			token=strtok(NULL,"=");
			if(!token)
				continue;
			c=strtok(token,":");
			tport1=strtol(c?c:"",NULL,10);
			taddr=strtok(NULL,":");
			c=strtok(NULL,":");
			tport2=strtol(c?c:"",NULL,10);
			if(taddr && strlen(taddr) && tport1>0 && tport2>0)
			{
				proxies++;
				cn=realloc(cn,sizeof(connectionnode)*proxies);
				if(cn==NULL)
				{
					proxies=0;
					return;
				}
				cn[proxies-1].id=proxies-1;
				cn[proxies-1].inport=tport1;
				cn[proxies-1].outport=tport2;
				strcpy(cn[proxies-1].outaddr,taddr);
				sprintf(cn[proxies-1].fname,"%s_%d.txt",taddr,tport2);
			}
		}
	}

	return;
}

//waits for a client to connect
void *waitForConnection(void *arg)
{
	int new_socket;
	socklen_t addrlen;
	struct sockaddr_in address;
	int create_socket;
	int one=1;
	unsigned long id=(unsigned long)arg;

	if ((create_socket = socket(PF_INET,SOCK_STREAM,0)) < 0)
	{
		fprintf(fout,"The Socket was NOT created\n");
		return NULL;
	}

	memset(&address,0,sizeof(struct sockaddr_in));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(cn[id].inport);

	setsockopt(create_socket,SOL_SOCKET,SO_REUSEADDR,(char*)&one,sizeof(int));

	if (bind(create_socket,(struct sockaddr *)&address,sizeof(address)) != 0)
	{
		perror("bind");
		return NULL;
	}

	fprintf(fout,"pforward ready on %d to %s:%d\n",cn[id].inport,
		cn[id].outaddr,cn[id].outport);

	listen(create_socket,64);

	while(!kills)
	{
		fflush(fout);
		addrlen = sizeof(struct sockaddr_in);
		new_socket = accept(create_socket,(struct sockaddr *)&address,&addrlen);

		if (new_socket > 0)
		{
			add(new_socket,id);
		}
	}
	
	close(create_socket);

	return NULL;
}

void *serve(void *arg)
{
	int s;
	int ret;
	char ip[128];
	unsigned long pos=(unsigned long)arg;
	char msg[256];
	int id;

	signal(SIGPIPE,sigignor);
	handle_signal(SIGPIPE, SIG_IGN);

	fprintf(fout,"Connection %lu ready\n",pos);
	fflush(fout);

	while(!kills)
	{
		pthread_mutex_lock(&queue);
		if(sizen()==0)
		{
			//block until there is a socket ready
			pthread_cond_wait(&ready,&queue);
			if(!kills)
				done++;
		}
		pthread_mutex_unlock(&queue);

		s=getsocket(&id);

		if(!s)
			continue;

		getsocketname(s,ip);

		ret=service(s,id);

		pthread_mutex_lock(&queue);
		commands++;
		if(ret<0)
			errors++;
		sprintf(msg,"Connection closed: %d bytes",ret);
		logfunc(msg,ip,cn[id].fname,NULL);
		pthread_mutex_unlock(&queue);
		close(s);
	}

	pthread_mutex_lock(&queue);
	done++;
	pthread_mutex_unlock(&queue);

	return NULL;
}

int service(int from,int id)
{
	int len;
	int tot=0;
	int to;
	char msg[128];
	char ip[128];
	fd_set socks;
	int hs;
	int ret;

	getsocketname(from,ip);
	if(strlen(ip)==0)
	{
		logfunc("Client disconnected before connection could be made","UNKNOWN",cn[id].fname,NULL);
		return -1;
	}

	sprintf(msg,"Thread dispatched to service %s request",cn[id].outaddr);
	logfunc(msg,ip,cn[id].fname,NULL);

	if((to=makeConnection(cn[id].outaddr,cn[id].outport))<1)
	{
		sprintf(msg,"Could not connect to %s:%d",cn[id].outaddr,cn[id].outport);
		logfunc(msg,ip,cn[id].fname,NULL);
		return -1;
	}

	if(to>from)
		hs=to;
	else
		hs=from;

	while(1)
	{
		FD_ZERO(&socks);
		FD_SET(from,&socks);
		FD_SET(to,&socks);

		ret=select(hs+1,&socks,NULL,NULL,0);

		if(ret<0)
		{
			perror("select");
			logfunc("SELECT ERROR",ip,cn[id].fname,NULL);
			return -1;
		}

		if(FD_ISSET(from,&socks))
		{
			len=relaysocks(from,to);
			if(len<1)
				break;
			else
				tot+=len;
		}
		if(FD_ISSET(to,&socks))
		{
			len=relaysocks(to,from);
			if(len<1)
				break;
			else
				tot+=len;
		}
	}

	close(to);
	return tot;
}

//relays data from from to to
int relaysocks(int from,int to)
{
	int len;
	int sent;
	int ret;
	char buf[BUFLEN];

	len=recv(from,buf,BUFLEN,0);

	sent=0;
	while(sent!=len)
	{
		ret=send(to,buf+sent,len-sent,0);
		if(ret>0)
			sent+=ret;
		else
		{
			len=0;
			break;
		}
	}

	return len;
}

//adds a socket to the queue
void add(int socket,int id)
{
	socketnode *node;

	node=(socketnode*)malloc(sizeof(socketnode));
	if(!node)
	{
		fprintf(fout,"ERROR: out of memory, throwing away connection\n");
		return;
	}

	pthread_mutex_lock(&queue);

	node->s=socket;
	node->id=id;
	node->next=NULL;

	if(!head && !tail)
	{
		head=node;
		tail=node;
	}
	else
	{
		tail->next=(struct socketnode*)node;
		tail=node;
	}

	pthread_mutex_unlock(&queue);

	//signal worker thread work is available
	pthread_cond_signal(&ready);

	return;
}

//returns first socket from queue
int getsocket(int *id)
{
	int s;
	socketnode *ret;

	pthread_mutex_lock(&queue);

	ret=head;

	if(!head)
	{
		pthread_mutex_unlock(&queue);
		return 0;
	}

	head=(socketnode*)(head->next);

	if(!head)
		tail=NULL;

	s=ret->s;
	*id=ret->id;

	free(ret);

	pthread_mutex_unlock(&queue);

	return s;
}

//returns the amount of sockets in the queue
int _size(int lock)
{
	int s=0;
	socketnode *ptr;

	if(lock)
		pthread_mutex_lock(&queue);

	ptr=head;

	while(ptr)
	{
		s++;
		ptr=(socketnode*)(ptr->next);
	}

	if(lock)
		pthread_mutex_unlock(&queue);

	return s;
}

//returns the size with locking
int size()
{
	return _size(1);
}

//returns the size without locking
int sizen()
{
	return _size(0);
}

//actively kills the program
void KILL()
{
	fprintf(fout,"Killing server\n");
	exit(1);
}

void sigkill(int x)
{
	KILL();
}

void sigignor(int x)
{
	fprintf(fout,"SIG Ignored\n");
	return;
}

void stats()
{
	done=0;
	pthread_cond_broadcast(&ready);
	msleep(1000);
	stat1();
}
//prints stats
void stat1()
{
	struct timeval te;
	int secs;

	gettimeofday(&te,NULL);
	secs=te.tv_sec-ts.tv_sec;
	fprintf(fout,"Stats (%s) Threads: %d/%d, Conn: %d, Err: %d\n",gettimestamp(),
		done,TOTAL,commands,errors);
	fprintf(fout,"Uptime: %dd %dh %dm %ds\n",secs/(3600*24),(secs%(3600*24))/3600,(secs%3600)/60,secs%60);
	fflush(fout);
}

// creates connection with server
int makeConnection(char *server,int port)
{
	int create_socket;
	struct sockaddr_in address;
	struct hostent *hp;
	int one=1;

	if(!server)
		return 0;

	if ((create_socket = socket(AF_INET,SOCK_STREAM,0))<0)
		return 0;

	if ((hp = gethostbyname(server)) == NULL)
		return 0;

	memcpy((char *)&address.sin_addr,hp->h_addr,hp->h_length);
	address.sin_family = AF_INET;
	address.sin_port = htons(port);

	one=1;
	setsockopt(create_socket,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
	one=1;
	setsockopt(create_socket,SOL_SOCKET,SO_KEEPALIVE,&one,sizeof(one));

	if (connect(create_socket,(struct sockaddr *)&address,sizeof(address)) != 0)
		return 0;

	return create_socket;
}

int msleep(int ms)
{
	struct timespec ts;

	if(ms<=0)
		return 0;

	ts.tv_sec=ms/1000;
	ts.tv_nsec=1000*1000*(ms%1000);

	return nanosleep(&ts,NULL);
}

void getsocketname(int socket,char *ip)
{
	socklen_t addrlen;
	struct sockaddr_in sin;
	struct hostent *hp;

	ip[0]='\0';

	addrlen=sizeof(sin);
	if(!getpeername(socket,(struct sockaddr*)&sin,&addrlen))
	{
		if ((hp=gethostbyaddr((char *)&sin.sin_addr,sizeof (sin.sin_addr),AF_INET))==NULL)
			strcpy(ip,inet_ntoa(sin.sin_addr));
		else
			strcpy(ip,hp->h_name);
	}

	return;
}

// writes data to logfile
void logfunc(char *data,char *ip,char *filename,FILE *fout)
{
	FILE *f;

	if(filename!=NULL)
	{
		f=fopen(filename,"a");

		if(f)
		{
			fprintf(f,"%s %s: %s\n",gettimestamp(),ip,data);
			fclose(f);
		}
	}

	if(fout!=NULL)
	{
		fprintf(fout,"%s %s: %s\n",gettimestamp(),ip,data);
		fflush(fout);
	}

	return;
}

// returns timestamp
char *gettimestamp()
{
	time_t tm;
	static char tbuf[128];

	tm=time(NULL);
	strcpy(tbuf,ctime(&tm));
	chomp(tbuf);

	return tbuf;
}

// converts string to lowercase
char *_tolower(char *buf)
{
	int i;

	for(i=0;buf[i];i++)
	{
		if(buf[i]>='A' && buf[i]<='Z')
			buf[i]+='a'-'A';
	}

	return buf;
}

// removes newline from end of string
void chomp(char *buf)
{
	if(buf[strlen(buf)-1]=='\n')
		buf[strlen(buf)-1]='\0';

	return;
}

void handle_signal(int sig, void (*handler)(int))
{
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = handler;

	if(!sigfillset(&sa.sa_mask))
		sigaction(sig, &sa, NULL);
}
