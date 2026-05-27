#define _GNU_SOURCE 
#include<stdio.h>
#include<string.h>
#include<stdlib.h>
#include<sys/socket.h>
#include<sys/time.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<pthread.h>
#include<netdb.h>
#include<time.h>
#include<sys/stat.h>
#include<dirent.h>
#include<stdarg.h>
#include<errno.h>

#define PORT 2121
#define BUFFER_SIZE 4096
#define CACHE_DIR "./cache/"
#define LOG_FILE "froxy.log"

//locks
pthread_rwlock_t config_lock = PTHREAD_RWLOCK_INITIALIZER;
pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;

//config si stats
struct{
    long total_requests;
    long cache_hits;
    long total_bytes_served;
    long long bytes_saved_by_cache;
}server_stats;

typedef struct{
    char target_domain[100];
    int start_hour, end_hour;
    char forbidden_client_domain[100];
}AccessPolicy;

struct{
    int port;
    long max_cache_size;
    int ttl_seconds;
    AccessPolicy policies[50];
    int policy_count;
}global_config;

typedef struct{
    int client_sock;
    int server_sock;
    struct sockaddr_in client_addr;
    char client_hostname[256];
    int is_connected_upstream;
    int pasv_listen_sock;
    struct sockaddr_in server_data_addr;
    int upstream_data_sock;
    char current_filename[256];
    int is_downloading;
    int cache_hit;
}ClientSession;

void safe_log(const char *format, ...){
    pthread_mutex_lock(&log_lock);
    FILE *fp = fopen(LOG_FILE, "a");
    if(!fp){
        pthread_mutex_unlock(&log_lock);
        return;
    }
    
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(fp, "[%04d-%02d-%02d %02d:%02d:%02d]", t->tm_year + 1900, t->tm_mon +1 , t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    va_list args;
    va_start(args, format);
    vfprintf(fp, format, args);
    va_end(args);
    fprintf(fp, "\n");
    fclose(fp);
    pthread_mutex_unlock(&log_lock);
}

int is_extension_allowed( char *filename){
    char *ext = strrchr(filename, '.');
    if(!ext)
        return 1;
    const char *blocked[] = {".exe", ".sh", ".bat", ".mp4", ".iso", NULL};
    for(int i=0; blocked[i] != NULL; i++){
        if(strcasecmp(ext, blocked[i]) == 0)
            return 0;
    }
    return 1;
}

void load_config(){
    pthread_rwlock_wrlock(&config_lock);
    FILE *f = fopen("froxy.conf", "r");
    global_config.port = 2121;
    global_config.policy_count = 0;
    global_config.max_cache_size = 10 * 1024 * 1024;
    global_config.ttl_seconds = 300;

    if(!f){
        safe_log("[WARN] Fisierul froxy.conf lipsa. Default active.");
    }else{
        char line[256];
        while(fgets(line, sizeof(line), f)){
            if(line[0] == '#' || strlen(line) < 3)
                continue;
            line[strcspn(line, "\r\n")] = 0;
            if(strncmp(line, "PORT=", 5) == 0)
                global_config.port = atoi(line + 5);
            else if(strncmp(line, "MAX_CACHE=", 10) == 0)
                global_config.max_cache_size = atoi(line + 10);
            else if(strncmp(line, "TTL=", 4) == 0)
                global_config.ttl_seconds = atoi(line + 4);
            else if(strncmp(line, "DENY_DOMAIN", 11) == 0){
                AccessPolicy *p = &global_config.policies[global_config.policy_count];
                if(sscanf(line, "DENY_DOMAIN %s %d %d %s", p->target_domain, &p->start_hour, &p->end_hour, p->forbidden_client_domain) == 4)
                    global_config.policy_count++;
            }
        }
        fclose(f);
    }
    safe_log("[CONFIG] Loaded. Port: %d, TTL: %ds, Policies: %d", global_config.port, global_config.ttl_seconds, global_config.policy_count);
    pthread_rwlock_unlock(&config_lock);
}
 
//garbage collector si stats
void *garbage_collector(void *arg){
    (void) arg;
    while(1){
        sleep(15);
        pthread_rwlock_rdlock(&config_lock);
        int current_ttl = global_config.ttl_seconds;
        pthread_rwlock_unlock(&config_lock);
        DIR *d = opendir(CACHE_DIR);
        if(d){
            struct dirent *dir;
            time_t now = time(NULL);
            while((dir = readdir(d)) != NULL){
                if(dir->d_name[0] == '.')
                    continue;
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s%s", CACHE_DIR, dir->d_name);
                struct stat file_stat;
                if(stat(filepath, &file_stat) == 0){
                    if(difftime(now, file_stat.st_mtime) > current_ttl){
                        remove(filepath);
                        safe_log("[GC] Auto-deleted: %s", dir->d_name);
                    }
                }
            }
            closedir(d);
        }
        //stats report
        pthread_mutex_lock(&stats_lock);
        if(server_stats.total_requests > 0){
            double hit_rate = (double) server_stats.cache_hits / server_stats.total_requests * 100.0;
            double saved_mb = (double) server_stats.bytes_saved_by_cache / (1024.0 * 1024.0);
            double served_mb = (double) server_stats.total_bytes_served / (1024.0 * 1024.0);
            safe_log("[STATS] Requests: %ld | Hits: %ld (%.1f%%) | Traffic: %.2f MB | Saved: %.2f MB", server_stats.total_requests, server_stats.cache_hits, hit_rate, served_mb, saved_mb);
        }
        pthread_mutex_unlock(&stats_lock);
    }
    return NULL;
}

int check_access_policy(char *target_host, char *client_host){
    pthread_rwlock_rdlock(&config_lock);
    time_t now = time(NULL);
    struct tm *local = localtime(&now);
    int h = local->tm_hour;
    int allowed = 1;
    for(int i=0; i < global_config.policy_count; i++){
        AccessPolicy *p = &global_config.policies[i];
        if(strstr(target_host, p->target_domain) != NULL){
            if(strcmp(p->forbidden_client_domain, "*") == 0 || strstr(client_host, p->forbidden_client_domain) != NULL){
                if(h >= p->start_hour && h < p->end_hour){
                    safe_log("[POLICY] BLOCKED: %s -> %s (Hour: %d)", client_host, target_host, h);
                    allowed = 0;
                    break;
                }
            }
        }
    }
    pthread_rwlock_unlock(&config_lock);
    return allowed;
}

int connect_to_server(const char *hostname){
    struct hostent *he = gethostbyname(hostname);
    if(!he)
        return -1;
    int sock = socket ( AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(21);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    if(connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0){
        close(sock);
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = 4;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    char big_buff[10000];
    char temp_buf[1024];
    
    while(1){
        int n = recv(sock, temp_buf, sizeof(temp_buf) - 1, 0);
        if(n <= 0)
            break;
        
        temp_buf[n] = 0;

        if(strlen(big_buff) + n < sizeof(big_buff)){
            strcat(big_buff, temp_buf);
        }
        if(strstr(big_buff, "\n220") != NULL || (strncmp(big_buff, "220", 4) == 0 )){
            break;
        }
    }
    tv.tv_sec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    return sock;
}

int setup_pasv_listener(ClientSession *session){
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    listen(sock,1);
    socklen_t len = sizeof(addr);
    getsockname(sock, (struct sockaddr *)&addr, &len);
    session->pasv_listen_sock = sock;
    return ntohs(addr.sin_port);
}

void handle_data_transfer(ClientSession *session){
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int client_data_sock = accept(session->pasv_listen_sock, (struct sockaddr *)&client_addr, &len);
    if(client_data_sock < 0){
        close(session->pasv_listen_sock);
        return;
    }

    char buf[BUFFER_SIZE + 4];
    int bytes;
    long long session_bytes = 0;


    if(session->cache_hit){
        safe_log("[CACHE HIT] Servesc '%s' din cache.", session->current_filename);
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s%s", CACHE_DIR, session->current_filename);
        utimes(filepath, NULL); //update timestamp

        FILE *f = fopen (filepath, "rb");
        if(f){
            while((bytes = fread(buf, 1, sizeof(buf), f)) > 0){
                send(client_data_sock, buf, bytes, 0);
                session_bytes += bytes;
            }
            fclose(f);
            pthread_mutex_lock(&stats_lock);
            server_stats.total_requests ++;
            server_stats.cache_hits ++;
            server_stats.total_bytes_served += session_bytes;
            server_stats.bytes_saved_by_cache += session_bytes;
            pthread_mutex_unlock(&stats_lock);
        }
    }else{
        int server_data_sock = session->upstream_data_sock;
        FILE *cache_file = NULL;
        long total_written = 0;
        int caching_enabled = 0;
        char filepath[524];
        char temp_filepath[530];


        if(session->is_downloading){
            snprintf(filepath, sizeof(filepath), "%s%s", CACHE_DIR, session->current_filename);
            snprintf(temp_filepath, sizeof(temp_filepath), "%s.tmp", filepath);
            cache_file = fopen(temp_filepath, "wb");
            if(cache_file){
                caching_enabled = 1;
                safe_log("[CACHE MISS] Descarc si salvez '%s' ...", session->current_filename);
            }
        }

        while((bytes = recv(server_data_sock, buf, sizeof(buf), 0)) > 0){
            send(client_data_sock, buf, bytes, 0);
            session_bytes += bytes;
            if(caching_enabled && cache_file){
                fwrite(buf, 1, bytes, cache_file);
                total_written += bytes;
                pthread_rwlock_rdlock(&config_lock);
                long limit = global_config.max_cache_size;
                pthread_rwlock_unlock(&config_lock);
                if(total_written > limit){
                    safe_log("[CACHE LIMIT] Fisier prea mare. Opresc salvarea.");
                    fclose(cache_file);
                    cache_file = NULL;
                    caching_enabled = 0;
                    remove(temp_filepath);
                }
            }
        }
        if(cache_file)
            fclose(cache_file);
        if(caching_enabled){
            //redenumesc fisierul .tmp in fisierul final
            if(rename(temp_filepath, filepath) == 0){
                safe_log("[CACHE SAVED] Fisierul '%s' salvat cu succes.", session->current_filename);
            }
            else{
                safe_log("[CACHE ERR] Eroare la redenumire fisier tmp.");
                remove(temp_filepath);
            }
        }
        close(server_data_sock);
        pthread_mutex_lock(&stats_lock);
        server_stats.total_bytes_served += session_bytes;
        pthread_mutex_unlock(&stats_lock);
    }
    close(client_data_sock);
    close(session->pasv_listen_sock);
    session->pasv_listen_sock = -1;
}

void *client_handler(void *arg){
    ClientSession *session = (ClientSession *)arg;
    char buffer[BUFFER_SIZE + 4];
    char srv_resp[BUFFER_SIZE + 4];
    int n;

    getnameinfo((struct sockaddr *)&session->client_addr, sizeof(session->client_addr), session->client_hostname, sizeof(session->client_hostname), NULL, 0, 0);
    safe_log("[NEW SESSION] %s (%s)", session->client_hostname, inet_ntoa(session->client_addr.sin_addr));
    write(session->client_sock, "220 Froxy FTP Proxy Ready\r\n", 27);

    while((n= recv(session->client_sock, buffer, sizeof(buffer) - 1, 0)) > 0){
        buffer[n] = 0;
        char log_buf[BUFFER_SIZE];
        strcpy(log_buf, buffer);
        log_buf[strcspn(log_buf, "\r\n")] = 0;
        if(strncmp(log_buf, "PASS ", 5) == 0)
            safe_log("[CMD] %s: PASS ***", session->client_hostname);
        else
            safe_log("[CMD] %s: %s", session->client_hostname, log_buf);

        if(strncmp(buffer, "RELOAD_CONFIG", 13) == 0){
            load_config();
            write(session->client_sock, "200 Config Reloaded\r\n", 21);
            continue;
        }

        if(!session->is_connected_upstream){
            if(strncmp(buffer, "USER ", 5) == 0){
                char user[100], host[100];
                char *at = strchr(buffer, '@');
                if(at){
                    *at = 0;
                    strcpy(user, buffer + 5 );
                    strcpy(host, at + 1);
                    host[strcspn(host, "\r\n")] = 0;
                    if(!check_access_policy(host, session->client_hostname)){
                        write(session->client_sock, "530 Policy Violation\r\n", 22);
                        continue;
                    }
                    session->server_sock = connect_to_server(host);
                    if( session->server_sock < 0){
                        write(session->client_sock, "421 Remote Down\r\n", 17);
                        safe_log("[ERR] Connection failed to %s", host);
                    }else{
                        session->is_connected_upstream = 1;
                        char fwd[256];
                        sprintf(fwd, "USER %s\r\n", user);
                        send(session->server_sock, fwd, strlen(fwd), 0);
                        
                        //citesc raspunsul la user 331
                        n = recv(session->server_sock, srv_resp, sizeof(srv_resp), 0);
                        write(session->client_sock, srv_resp, n);
                    }
                }else{
                    write(session->client_sock, "501 Use USER user@host\r\n", 24);
                }
            }else if(strncmp(buffer, "QUIT", 4) == 0){
                    write(session->client_sock, "221 Goodbye\r\n", 13);
                    break;
            }else{
                    write(session->client_sock, "530 Login first\r\n", 17);
            }
        }else{
            if(strncmp(buffer, "RETR ", 5) == 0){
                sscanf(buffer + 5, "%s", session->current_filename);
                session->current_filename[strcspn(session->current_filename, "\r\n")] = 0;

                if(strstr(session->current_filename, "..") != NULL || strchr(session->current_filename, '/') != NULL){
                    safe_log("[SEC] BLOCKED Path Traversal detected: %s", session->current_filename);
                    write(session->client_sock, "550 Illegal filename.\r\n", 23);
                    continue;
                }
                
                if(!is_extension_allowed(session->current_filename)){
                    safe_log("[SEC] BLOCKED file: %s", session->current_filename);
                    write(session->client_sock, "550 Access Denied: Dangerous File\r\n", 35);
                    continue;
                }

                session->is_downloading = 1;
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s%s", CACHE_DIR, session->current_filename);

                if(access(filepath, F_OK) != -1){
                    session->cache_hit = 1;
                    //anunt clientul ca incep transferul
                    char *msg_start = "150 Opening BINARY mode data connection for file from cache.\r\n";
                    write(session->client_sock, msg_start, strlen(msg_start));
                    handle_data_transfer(session);
                    char *msg_end = "226 Transfer complete.\r\n";
                    write(session->client_sock, msg_end, strlen(msg_end));
                    session->cache_hit = 0;
                    continue;
                }
            }else if(strncmp(buffer, "LIST", 4) ==0){
                session->is_downloading = 0;
            }

            //trimit comanda originala 
            send(session->server_sock, buffer, strlen(buffer), 0);

            if(strncmp(buffer, "PASV", 4) == 0){
                //loop pt a gasi 227 printre msj vechi
                while(1){
                    n = recv(session->server_sock, srv_resp, sizeof(srv_resp) - 1 , 0);
                    if(n <= 0)
                        break;
                    srv_resp[n] = 0;
                    if(strstr(srv_resp, "227")){
                        int h1, h2, h3, h4, p1, p2;
                        char *start = strchr(srv_resp, '(');
                        if(start){
                            sscanf(start, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2);
                            session->server_data_addr.sin_family = AF_INET;
                            char ip[32];
                            sprintf(ip, "%d.%d.%d.%d", h1, h2, h3, h4);
                            inet_pton(AF_INET, ip, &session->server_data_addr.sin_addr);
                            session->server_data_addr.sin_port = htons(p1*256 + p2);
                            if(session->upstream_data_sock > 0)
                                close(session->upstream_data_sock);
                            session->upstream_data_sock = socket(AF_INET, SOCK_STREAM, 0);
                            if(connect(session->upstream_data_sock, (struct sockaddr *)&session->server_data_addr, sizeof(session->server_data_addr)) < 0){
                                safe_log("[ERR] Failed upstream data connect");
                            }

                            if(session->pasv_listen_sock > 0)
                                close(session->pasv_listen_sock);
                            int my_port = setup_pasv_listener(session);

                            char fake_227[128];
                            sprintf(fake_227, "227 Entering Passive Mode (127,0,0,1,%d,%d)", my_port/256, my_port%256);
                            write(session->client_sock, fake_227, strlen(fake_227));
                        }
                        break; //ies din bucla pasv
                    }else{
                        //nu e 227 , trimit la client sa stie de el si continui ascultarea
                        safe_log("[PASV-NOISE] Forwarding: %s", srv_resp);
                        write(session->client_sock, srv_resp, n);
                        //daca e eroare fatala (5..) ma opresc
                        if(srv_resp[0] == '5' || srv_resp[0] == '4')
                            break;
                    }
                }
            }else{
                //relay pt alte comenzi
                while(1){
                    n = recv(session->server_sock, srv_resp, sizeof(srv_resp) - 1, 0);
                    if(n <= 0)
                        break;
                    srv_resp[n] = 0;
                    write(session->client_sock, srv_resp, n);

                    //detectez start transfer 150/125
                    if(strncmp(srv_resp, "150", 3) == 0 || strncmp(srv_resp, "125", 3) == 0){
                        handle_data_transfer(session);
                        //daca 226 nu a venit lipit de alte msj, il astept
                        if(!strstr(srv_resp, "226")){
                            n = recv(session->server_sock, srv_resp, sizeof(srv_resp), 0);
                            if(n > 0)
                                write(session->client_sock, srv_resp, n);
                        }
                        break;
                    }
                    if(n < BUFFER_SIZE - 1)
                        break;
                }
            }
        }
    }
    close(session->client_sock);
    if(session->server_sock > 0)
        close(session->server_sock);
    if(session->upstream_data_sock)
        close(session->upstream_data_sock);
    free(session);
    safe_log("[SESSION END] Client deconectat.");
    return NULL;
}

int main(){
    mkdir(CACHE_DIR, 0777);
    load_config();
    pthread_t cleaner_thread;
    if(pthread_create(&cleaner_thread, NULL, garbage_collector, NULL) != 0)
        return 1;
    pthread_detach(cleaner_thread);

    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(global_config.port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if(bind(server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0){
        perror("Bind failed");
        return 1;
    }
    listen(server_sock, 10);
    safe_log("=== Froxy Server STARTED on port %d ===", global_config.port);
    printf("Server pornit. Verifica froxy.log\n");

    while(1){
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client_sock = accept(server_sock, (struct sockaddr *)&client_addr, &len);
        if(client_sock < 0)
            continue;
        
        ClientSession *sess = malloc(sizeof(ClientSession));
        sess->client_sock = client_sock;
        sess->client_addr = client_addr;
        sess->server_sock = -1;
        sess->is_connected_upstream = 0;
        sess->cache_hit = 0;
        sess->pasv_listen_sock = -1;
        sess->upstream_data_sock = -1;

        pthread_t t;
        if(pthread_create(&t, NULL, client_handler, sess) != 0){
            free(sess);
            close(client_sock);
        }else{
            pthread_detach(t);
        }
    }
    return 0;
}