// dcli.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SERV_IP   "127.0.0.1"
#define SERV_PORT 18800
#define MAXLINE   1024

static int conn_fd=-1;
static int client_id=0;
static int wrote_shutdown=0;

static void cleanup(void){
    if(conn_fd>=0) close(conn_fd);
}

static int gen_id4(void){
    srand((unsigned)time(NULL) ^ getpid());
    return (rand()%9000)+1000; // 1000..9999
}

int main(int argc, char *argv[]){
    if(argc==2){
        client_id = atoi(argv[1]);
        if(client_id<=0 || client_id>9999) client_id = gen_id4();
    }else{
        client_id = gen_id4();
    }

    struct sockaddr_in serv_addr;
    conn_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(conn_fd<0){ perror("socket"); exit(1); }

    memset(&serv_addr,0,sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(SERV_PORT);
    serv_addr.sin_addr.s_addr = inet_addr(SERV_IP);

    if(connect(conn_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr))<0){
        perror("connect");
        cleanup(); exit(1);
    }

    // send initial ID (as "cli-XXXX\n")
    char idbuf[32];
    snprintf(idbuf,sizeof(idbuf),"cli-%04d\n", client_id);
    write(conn_fd, idbuf, strlen(idbuf));

    fd_set base_rfds, rfds;
    FD_ZERO(&base_rfds);
    FD_SET(fileno(stdin), &base_rfds);
    FD_SET(conn_fd, &base_rfds);
    int fdmax = conn_fd;

    for(;;){
        printf("Dcli%04d> ", client_id);
        fflush(stdout);

        rfds = base_rfds;
        if(select(fdmax+1, &rfds, NULL, NULL, NULL) < 0){
            if(errno==EINTR) continue;
            perror("select");
            cleanup(); exit(1);
        }

        // stdin -> send to server
        if(FD_ISSET(fileno(stdin), &rfds)){
            char line[MAXLINE];
            if(!fgets(line, sizeof(line), stdin)){
                // EOF: half-close write side
                if(!wrote_shutdown){
                    shutdown(conn_fd, SHUT_WR);
                    wrote_shutdown=1;
                }
            }else{
                // send text as-is (supports "viewlist\n" command)
                int n = write(conn_fd, line, strlen(line));
                if(n<0){ perror("write"); cleanup(); exit(1); }
            }
        }

        // socket -> print
        if(FD_ISSET(conn_fd, &rfds)){
            char line[MAXLINE];
            int m = read(conn_fd, line, sizeof(line)-1);
            if(m==0){
                // server closed
                printf("\\n[TCP closed]\\n");
                cleanup(); break;
            }else if(m<0){
                perror("read"); cleanup(); exit(1);
            }else{
                line[m]='\\0';
                fputs(line, stdout);
            }
        }
    }
    return 0;
}
