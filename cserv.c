// cserv.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define DEFAULT_SERV_IP   "127.0.0.1"
#define DEFAULT_SERV_PORT 18800
#define DEFAULT_MAX_HIST  10
#define MAXLINE   1024
#define MAXCONN   128

typedef struct {
    int fd;
    int id;
    int inuse;
} Client;

typedef struct HNode {
    int client;           // sender id
    int length;           // message length
    char *line;           // heap-allocated message text
    struct HNode *next;
} HNode;

static int lis_fd = -1;
static Client clients[MAXCONN];
static int client_count = 0;

// runtime config
static char SERV_IP[64]   = {0};
static int  SERV_PORT     = 0;
static int  MAX_HIST      = 0;

// circular linked list for last N messages
static HNode *head = NULL, *tail = NULL;
static int hcount = 0;

static void cleanup_history(void){
    if(!head) return;
    HNode *cur = head;
    for(int i=0;i<hcount;i++){
        HNode *nxt = cur->next;
        free(cur->line);
        free(cur);
        cur = nxt;
    }
    head = tail = NULL;
    hcount = 0;
}

static void add_hnode(int client_id, const char *msg){
    // trim trailing newline
    size_t len = strlen(msg);
    while(len>0 && (msg[len-1]=='\n' || msg[len-1]=='\r')) len--;

    HNode *node = (HNode*)calloc(1,sizeof(HNode));
    node->client = client_id;
    node->length = (int)len;
    node->line   = (char*)malloc(len+1);
    memcpy(node->line, msg, len);
    node->line[len] = '\0';

    if(!head){
        head = tail = node;
        node->next = node; // circular
        hcount = 1;
    } else {
        node->next = head;
        tail->next = node;
        tail = node;
        if(hcount < MAX_HIST){
            hcount++;
        } else {
            // drop the oldest
            HNode *old = head;
            head = head->next;
            tail->next = head;
            free(old->line);
            free(old);
        }
    }
}

static void view_list_stdout(void){
    if(!head || hcount==0){
        printf("(empty)\n");
        fflush(stdout);
        return;
    }
    HNode *cur = head;
    for(int i=0;i<hcount;i++){
        printf("cli-%04d says: %s\n", cur->client, cur->line);
        cur = cur->next;
    }
    fflush(stdout);
}

static void view_list_to_client(int cfd){
    char buf[MAXLINE*2];
    if(!head || hcount==0){
        const char *empty="(empty)\n<END>\n";
        write(cfd, empty, strlen(empty));
        return;
    }
    HNode *cur = head;
    for(int i=0;i<hcount;i++){
        int n = snprintf(buf, sizeof(buf), "cli-%04d says: %s\n", cur->client, cur->line);
        if(n>0) write(cfd, buf, (size_t)n);
        cur = cur->next;
    }
    const char *end = "<END>\n";
    write(cfd, end, strlen(end));
}

static void remove_client_index(int idx){
    if(idx<0 || idx>=MAXCONN || !clients[idx].inuse) return;
    close(clients[idx].fd);
    clients[idx].inuse = 0;
    client_count--;
}

static int find_client_index_by_fd(int fd){
    for(int i=0;i<MAXCONN;i++){
        if(clients[i].inuse && clients[i].fd==fd) return i;
    }
    return -1;
}

static void add_client_fd(int cfd, int cid){
    for(int i=0;i<MAXCONN;i++){
        if(!clients[i].inuse){
            clients[i].inuse = 1;
            clients[i].fd = cfd;
            clients[i].id = cid;
            client_count++;
            return;
        }
    }
    close(cfd);
}

// graceful shutdown (Ctrl+C)
static volatile sig_atomic_t stop_flag = 0;
static void on_sigint(int sig){ (void)sig; stop_flag = 1; }

static void load_config_from_env(void){
    const char *ip = getenv("SERV_IP");
    const char *pt = getenv("SERV_PORT");
    const char *mh = getenv("MAX_HIST");

    snprintf(SERV_IP, sizeof(SERV_IP), "%s", ip && *ip ? ip : DEFAULT_SERV_IP);
    SERV_PORT = pt && *pt ? atoi(pt) : DEFAULT_SERV_PORT;
    if(SERV_PORT <= 0 || SERV_PORT > 65535) SERV_PORT = DEFAULT_SERV_PORT;

    MAX_HIST = mh && *mh ? atoi(mh) : DEFAULT_MAX_HIST;
    if(MAX_HIST <= 0) MAX_HIST = DEFAULT_MAX_HIST;
}

int main(void){
    signal(SIGINT, on_sigint); // Ctrl+C
    load_config_from_env();

    printf("Starting cserv on %s:%d (MAX_HIST=%d)\n", SERV_IP, SERV_PORT, MAX_HIST);

    struct sockaddr_in serv_addr;

    lis_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(lis_fd<0){ perror("socket"); exit(1); }

    int yes=1;
    setsockopt(lis_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    memset(&serv_addr,0,sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(SERV_PORT);
    serv_addr.sin_addr.s_addr = inet_addr(SERV_IP);

    if(bind(lis_fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr))<0){
        perror("bind"); exit(1);
    }
    if(listen(lis_fd, 16)<0){
        perror("listen"); exit(1);
    }

    fd_set base_rfds, rfds;
    FD_ZERO(&base_rfds);
    FD_SET(lis_fd, &base_rfds);
    FD_SET(fileno(stdin), &base_rfds);  // server-side viewlist
    int fdmax = lis_fd;
    if(fileno(stdin) > fdmax) fdmax = fileno(stdin);

    for(;;){
        if(stop_flag){
            printf("\n[server] SIGINT received, shutting down...\n");
            break;
        }

        rfds = base_rfds;
        if(select(fdmax+1, &rfds, NULL, NULL, NULL) < 0){
            if(errno==EINTR) continue;
            perror("select");
            break;
        }

        // 1) stdin: server operator commands
        if(FD_ISSET(fileno(stdin), &rfds)){
            char cmd[128];
            if(!fgets(cmd, sizeof(cmd), stdin)){
                // EOF on stdin -> ignore
            }else{
                size_t L=strlen(cmd);
                while(L>0 && (cmd[L-1]=='\n'||cmd[L-1]=='\r')) cmd[--L]='\0';
                if(strcmp(cmd,"viewlist")==0){
                    printf("Cserv> history (last %d):\n", MAX_HIST);
                    view_list_stdout();
                }else if(strcmp(cmd,"stats")==0){
                    printf("Cserv> clients=%d, history=%d\n", client_count, hcount);
                }else{
                    printf("Cserv> unknown: %s\n", cmd);
                }
            }
        }

        // 2) new connection
        if(FD_ISSET(lis_fd, &rfds)){
            int cfd = accept(lis_fd, NULL, NULL);
            if(cfd<0){ perror("accept"); continue; }

            // read initial client id line
            char idbuf[64]={0};
            int n = read(cfd, idbuf, sizeof(idbuf)-1);
            if(n<=0){
                close(cfd);
            }else{
                // parse id (accept either "0245" or "cli-0245")
                int cid=0;
                if(sscanf(idbuf,"cli-%d",&cid)!=1){
                    cid = atoi(idbuf);
                }
                if(cid<0) cid=0;
                add_client_fd(cfd, cid);
                FD_SET(cfd, &base_rfds);
                if(cfd>fdmax) fdmax=cfd;
                printf("New connection %d (cli-%04d)\n", cfd, cid);
                fflush(stdout);
            }
        }

        // 3) data from clients
        for(int fd=0; fd<=fdmax; fd++){
            if(fd==lis_fd || fd==fileno(stdin)) continue;
            if(!FD_ISSET(fd, &rfds)) continue;
            int idx = find_client_index_by_fd(fd);
            if(idx<0){
                // stale fd; clear it
                FD_CLR(fd, &base_rfds);
                close(fd);
                continue;
            }
            char line[MAXLINE];
            int n = read(fd, line, MAXLINE-1);
            if(n<=0){
                printf("Connection %d closed\n", fd);
                FD_CLR(fd, &base_rfds);
                remove_client_index(idx);
                continue;
            }
            line[n]='\0';

            // ---- Health check ----
            if(strncmp(line,"health",6)==0){
                const char *ok = "OK\n";
                write(fd, ok, strlen(ok));
                continue;
            }

            // client-side request to view history
            if(strncmp(line,"viewlist",8)==0){
                view_list_to_client(fd);
                continue;
            }

            // broadcast prefixed message to others
            char msg[MAXLINE+64];
            int m = snprintf(msg, sizeof(msg), "cli-%04d says: %s", clients[idx].id, line);
            // store to history
            add_hnode(clients[idx].id, line);

            for(int j=0;j<MAXCONN;j++){
                if(!clients[j].inuse) continue;
                if(clients[j].fd == fd) continue;
                write(clients[j].fd, msg, (size_t)m);
            }
        }
    }

    // cleanup
    for(int i=0;i<MAXCONN;i++) if(clients[i].inuse) close(clients[i].fd);
    if(lis_fd>=0) close(lis_fd);
    cleanup_history();
    return 0;
}
