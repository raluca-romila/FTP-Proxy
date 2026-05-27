#include<stdio.h>
#include<stdlib.h>
#include<sys/socket.h>
#include<arpa/inet.h>
#include<unistd.h>
#include<sys/stat.h>
#include<sys/types.h>
#include<string.h>

#define PORT 2121 
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 4096
#define DOWNLOAD_DIR "./downloads/"

void handle_data_connection(int port , char *ip, char *filename){
    int data_sock;
    struct sockaddr_in data_server;
    char buffer[BUFFER_SIZE + 4];
    int len;
    FILE *f = NULL;

    //daca  am filename, pregatesc calea in folderul downloads
    if(filename){
        char filepath[512];
        snprintf(filepath, sizeof(filepath), "%s%s", DOWNLOAD_DIR, filename);

        f = fopen(filepath, "wb");
        if(!f){
            perror("[CLIENT] Eroare la crearea fisierului in downloads/");
            return;
        }
        printf("[CLIENT] Descarc fisieru; '%s' in '%s' ...\n", filename, filepath);
    }else{
        printf("\n--- LISTING DIRECTORY ---\n");
    }

    data_sock = socket(AF_INET, SOCK_STREAM, 0);
    if(data_sock < 0)
        return;
    
    data_server.sin_family = AF_INET;
    data_server.sin_port = htons(port);
    inet_pton(AF_INET, ip, &data_server.sin_addr);

    if(connect(data_sock, (struct sockaddr *)&data_server, sizeof(data_server)) < 0){
        perror("[CLIENT] Eroare conectare date");
        if(f)
            fclose(f);
        close(data_sock);
        return;
    }

    while((len = recv (data_sock, buffer, sizeof(buffer), 0)) > 0){
        if(f){
            fwrite(buffer, 1, len, f);
        }else{
            printf("%.*s", len, buffer);
        }
    }

    if(f){
        fclose(f);
        printf("[CLIENT] Transfer complet. Fisier salvat cu succes.\n");
    }else{
        printf("\n--- END LISTING ---\n");
    }

    close(data_sock);
}

int main(){
    int sock;
    struct sockaddr_in server;
    char message[BUFFER_SIZE + 4], server_reply[BUFFER_SIZE + 4];

    //creez folderul downloads la pornire 
    // 0777 permisiuni de w/r/e pt toti
    struct stat st = {0};
    if(stat(DOWNLOAD_DIR, &st) == -1){
        mkdir(DOWNLOAD_DIR, 0777);
        printf("[CLIENT] Am creat folderul '%s' pentru fisiere.\n", DOWNLOAD_DIR);
    }

    while(1){
        printf("\n[CLIENT] Conectare la Froxy...\n");
        sock = socket(AF_INET, SOCK_STREAM, 0);
        server.sin_family = AF_INET;
        server.sin_port = htons(PORT);
        server.sin_addr.s_addr = inet_addr(SERVER_IP);

        if(connect(sock, (struct sockaddr *)&server, sizeof(server)) < 0){
            perror("Conectare esuata. Retry in 3s...");
            sleep(3);
            continue;
        }

        memset(server_reply, 0, BUFFER_SIZE);
        if(recv(sock, server_reply, BUFFER_SIZE, 0) > 0)
            printf("%s", server_reply);
        
        while(1){
            printf("Comanda> ");
            if(fgets(message, BUFFER_SIZE, stdin) == NULL)
                break;
            message[strcspn(message, "\n")] = 0;

            int is_list = (strncmp(message, "LIST", 4) == 0);
            int is_retr = (strncmp(message, "RETR", 4) == 0);

            if(is_list || is_retr){
                //1)PASV
                send(sock, "PASV\r\n", 6, 0);

                int port = 0;
                char ip_str[32] = {0};

                while(1){
                    memset(server_reply, 0, BUFFER_SIZE);
                    if(recv(sock, server_reply, BUFFER_SIZE, 0) <= 0)
                        break;
                    printf("%s", server_reply);

                    if(strstr(server_reply, "227")){
                        int h1, h2, h3, h4, p1, p2;
                        char *start = strchr(server_reply, '(');
                        if(start){
                            sscanf(start, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2);
                            port = p1 * 256 + p2;
                            sprintf(ip_str, "%d.%d.%d.%d", h1, h2, h3, h4);
                            break;
                        }
                    }
                }

                if(port != 0){
                    //2)comanda propriu-zisa
                    char cmd_buf[BUFFER_SIZE + 6];
                    sprintf(cmd_buf, "%s\r\n", message);
                    send(sock, cmd_buf, strlen(cmd_buf), 0);

                    memset(server_reply, 0, BUFFER_SIZE);
                    recv(sock, server_reply, BUFFER_SIZE, 0);
                    printf("%s", server_reply);

                    //verific daca serverul a dat eroare (550 Access Denied)
                    if(strstr(server_reply, "550") || strstr(server_reply, "425")){
                        continue;
                    }

                    char *filename = NULL;
                    if(is_retr){
                        filename = message + 5;
                        while(*filename == ' ')
                            filename++;
                    }

                    handle_data_connection(port, ip_str, filename);

                    if(strstr(server_reply, "226") == NULL){
                        memset(server_reply, 0, BUFFER_SIZE);
                        recv(sock, server_reply, BUFFER_SIZE, 0);
                        printf("%s", server_reply);
                    }
                    continue;
                }
            }

            char send_buf[BUFFER_SIZE + 6];
            sprintf(send_buf, "%s\r\n", message);
            send(sock, send_buf, strlen(send_buf), 0);

            memset(server_reply, 0, BUFFER_SIZE);
            if(recv(sock, server_reply, BUFFER_SIZE, 0) <= 0)
                break;
            
            printf("%s", server_reply);
        }
        close(sock);
    }
    return 0;
}