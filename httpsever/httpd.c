#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdint.h>

#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server: jdbhttpd/0.1.0\r\n"
#define STDIN   0
#define STDOUT  1
#define STDERR  2

void accept_request(void *);
void bad_request(int);
void cat(int, FILE *);
void cannot_execute(int);
void error_die(const char *);
void execute_cgi(int, const char *, const char *, const char *);
int get_line(int, char *, int);
void headers(int, const char *);
void not_found(int);
void serve_file(int, const char *);
int startup(u_short *);
void unimplemented(int);

void accept_request(void *arg)
{
    int client = (intptr_t)arg; //再将指针转换为套接字的整数型
    char buf[1024];
    size_t numchars;  //size_t取值为目标平台下 最大可能的数组尺寸
    char method[255];
    char url[255];
    char path[512];
    size_t i, j;
    struct stat st;
    int cgi = 0;      /* becomes true if server decides this is a CGI
                       * program */
    char *query_string = NULL;

    numchars = get_line(client, buf, sizeof(buf));  //获取请求的第一行的数据
    i = 0; j = 0;
    while (!ISspace(buf[i]) && (i < sizeof(method) - 1))  //将请求的方法存到method数组中
    {
        method[i] = buf[i];
        i++;
    }
    j=i;
    method[i] = '\0';  //结束符

    if (strcasecmp(method, "GET") && strcasecmp(method, "POST"))  //如果方法不是GET或POST
    {
        unimplemented(client);       //返回包含方法错误的数据
        return;                      //结束线程
    }

    if (strcasecmp(method, "POST") == 0)       //如果方法是POST 开启cgi
        cgi = 1;

    i = 0;
    while (ISspace(buf[j]) && (j < numchars))
        j++;
    while (!ISspace(buf[j]) && (i < sizeof(url) - 1) && (j < numchars))    //将请求包含的url存储到url数组中
    {
        url[i] = buf[j];
        i++; j++;
    }
    url[i] = '\0';  //结束符

    if (strcasecmp(method, "GET") == 0)      //如果方法是GET 若携带参数 存储参数开始的位置
    {
        query_string = url;
        while ((*query_string != '?') && (*query_string != '\0'))
            query_string++;
        if (*query_string == '?')  //利用字符指针 指向参数开始的位置
        {
            cgi = 1;
            *query_string = '\0';
            query_string++;
        }
    }

    sprintf(path, "htdocs%s", url);
    if (path[strlen(path) - 1] == '/')
        strcat(path, "index.html");            // 格式化url到path数组，表示浏览器请求的服务器文件路径

    //根据路径找文件，并获取path文件信息保存到结构体st中，-1表示寻找失败
    if (stat(path, &st) == -1) { 
        while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers */
            numchars = get_line(client, buf, sizeof(buf));           //将报文清空
        not_found(client);
    }
    else
    {
        if ((st.st_mode & S_IFMT) == S_IFDIR)
            strcat(path, "/index.html");
        if ((st.st_mode & S_IXUSR) ||
                (st.st_mode & S_IXGRP) ||
                (st.st_mode & S_IXOTH)    )
            cgi = 1;
        if (!cgi)
            serve_file(client, path);
        else
            execute_cgi(client, path, method, query_string);
    }

    close(client);
}

void bad_request(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 400 BAD REQUEST\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "<P>Your browser sent a bad request, ");
    send(client, buf, sizeof(buf), 0);
    sprintf(buf, "such as a POST without a Content-Length.\r\n");
    send(client, buf, sizeof(buf), 0);
}

void cat(int client, FILE *resource)
{
    char buf[1024];

    fgets(buf, sizeof(buf), resource);
    while (!feof(resource)) //一行行发送 直到发送完
    {
        send(client, buf, strlen(buf), 0);
        fgets(buf, sizeof(buf), resource);
    }
}

void cannot_execute(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 500 Internal Server Error\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<P>Error prohibited CGI execution.\r\n");
    send(client, buf, strlen(buf), 0);
}

void error_die(const char *sc)
{
    perror(sc);   //如果套接字没创建成功
    exit(1);     //表示异常退出
}

void execute_cgi(int client, const char *path,
        const char *method, const char *query_string)
{
    char buf[1024];
    int cgi_output[2];
    int cgi_input[2];
    pid_t pid;
    int status;
    int i;
    char c;
    int numchars = 1;
    int content_length = -1;

    buf[0] = 'A'; buf[1] = '\0';
    if (strcasecmp(method, "GET") == 0)  //忽略大小写的比较 相等返回0
        while ((numchars > 0) && strcmp("\n", buf))  /* read & discard headers 读取并移除报文头*/ 
            numchars = get_line(client, buf, sizeof(buf));
    else if (strcasecmp(method, "POST") == 0) /*POST*/
    {
        numchars = get_line(client, buf, sizeof(buf));
        while ((numchars > 0) && strcmp("\n", buf))  //读取 content_length
        {
            buf[15] = '\0';
            if (strcasecmp(buf, "Content-Length:") == 0) //内存从第17位开始就是长度，将17位开始的所有字符串转成整数就是content_length
                content_length = atoi(&(buf[16])); 
            numchars = get_line(client, buf, sizeof(buf));
        }
        if (content_length == -1) {
            bad_request(client);
            return;
        }
    }
    else/*HEAD or other*/
    {
    }

    //创建2个管道
    if (pipe(cgi_output) < 0) {
        cannot_execute(client);
        return;
    }
    if (pipe(cgi_input) < 0) {
        cannot_execute(client);
        return;
    }

    //派生一个分进程
    if ( (pid = fork()) < 0 ) {
        cannot_execute(client);
        return;
    }
    sprintf(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    //确认子进程
    if (pid == 0)  /* child: CGI script */
    {
        char meth_env[255];
        char query_env[255];
        char length_env[255];

        dup2(cgi_output[1], STDOUT);
        dup2(cgi_input[0], STDIN);
        close(cgi_output[0]);
        close(cgi_input[1]);
        //配置环境变量
        sprintf(meth_env, "REQUEST_METHOD=%s", method);
        putenv(meth_env);
        if (strcasecmp(method, "GET") == 0) {
            sprintf(query_env, "QUERY_STRING=%s", query_string);
            putenv(query_env);
        }
        else {   /* POST */
            sprintf(length_env, "CONTENT_LENGTH=%d", content_length);
            putenv(length_env);
        }
        //运行path地址的cgi脚本
        execl(path,path, NULL);
        exit(0); //退出子进程
    } else {    /* parent */
        close(cgi_output[1]);
        close(cgi_input[0]);
        if (strcasecmp(method, "POST") == 0)
            for (i = 0; i < content_length; i++) {
                recv(client, &c, 1, 0); //将缓冲区c与客户端绑定
                write(cgi_input[1], &c, 1);//将POST的数据写入c
            }
        while (read(cgi_output[0], &c, 1) > 0)
            send(client, &c, 1, 0);

        close(cgi_output[0]);
        close(cgi_input[1]);
        waitpid(pid, &status, 0);
         //父进程从客户端套接字读取字符，通过cgi_input[1]写入cgi_input管道
    //cgi_input[0]被子进程重定向到标准输入流中，子进程替换为CGI脚本
    //CGI获取标准输入流中的数据，处理后将数据输入到标准输出流中，而标准输出流被重定向到cgi_output[1]中，
    //父进程重cgi_output[0]读取端读取要放回给客户端的数据，send到客户端

    }
}


int get_line(int sock, char *buf, int size)
{    int i = 0;、

    char c = '\0';
    int n;

    while ((i < size - 1) && (c != '\n'))  //提取到换行符或者结尾 就结束循环
    {
        n = recv(sock, &c, 1, 0); //从TCP连接的另一端接收数据 存储到缓存区c中  返回值是copy的字节数 存储到n中  并移除sock缓存区中的数据
        /* DEBUG printf("%02X\n", c); */
        if (n > 0)
        {
            if (c == '\r') //如果是回车字符继续接收下一个字节 因为换行符可能是\r\n
            {
                n = recv(sock, &c, 1, MSG_PEEK);  //再接收一个字符 但不移除sock缓存区中的数据
                /* DEBUG printf("%02X\n", c); */
                if ((n > 0) && (c == '\n'))   //如果是换行符 就移除sock缓存区中的数据
                    recv(sock, &c, 1, 0);
                else                        //否则给c赋值为换行符
                    c = '\n';
            }
            buf[i] = c;   //将提取到的字符存到Buf中
            i++;       //继续提取下一个字符
        }
        else   
            c = '\n';
    }
    buf[i] = '\0';

    return(i); //返回buf数组的大小
}

void headers(int client, const char *filename)
{
    char buf[1024];
    (void)filename;  /* could use filename to determine file type */

    strcpy(buf, "HTTP/1.0 200 OK\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    strcpy(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
}

void not_found(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 404 NOT FOUND\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "your request because the resource specified\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "is unavailable or nonexistent.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}

void serve_file(int client, const char *filename)
{
    FILE *resource = NULL;
    int numchars = 1;
    char buf[1024];

    buf[0] = 'A'; buf[1] = '\0';
    while ((numchars > 0) && strcmp("\n", buf))  /* 读取并丢弃没用的报文行 */
        numchars = get_line(client, buf, sizeof(buf));

    resource = fopen(filename, "r");
    if (resource == NULL)
        not_found(client);
    else
    {
        headers(client, filename);
        cat(client, resource);
    }
    fclose(resource);
}

int startup(u_short *port) //传入一个端口号
{
    int httpd = 0;
    int on = 1;
    struct sockaddr_in name;  //socket所绑定的地址

    //创建一个socket
    httpd = socket(PF_INET, SOCK_STREAM, 0);    //创建TCP套接字 返回的是一个int类型的文件描述符
            //第一个参数(IP地址类型)--->   PF_INET(IPv4地址)  or  PF_INET6 (IPv6地址)
            //第二个参数(数据传输方式/套接字类型)---> SOCK_STREAM (流格式套接字/面向连接的套接字)  or  SOCK_DGRAM（数据报套接字/无连接的套接字）
            //第三个参数(传输协议)  一般根据IP地址类型和套接字类型 系统会自动推演出来 满足上述条件的只有TCP协议
    if (httpd == -1)
        error_die("socket");    //输出因为套接字创建失败的错误
    
    //填写绑定的地址
    memset(&name, 0, sizeof(name));  //初始化
    name.sin_family = AF_INET;      //将IP地址类型初始化位IPv4地址
    name.sin_port = htons(*port);  /*因为导入参数的是指针 所以这里用*来取指针所指的数值  (若port为0,会随机赋值一个数
    *短整型*通过htons()将端口号从主机字节顺序转换成网络字节顺序 这是网络上使用统一的字节顺序 可以避免兼容性问题*/
    name.sin_addr.s_addr = htonl(INADDR_ANY); //服务器的IP地址设置为本机(INADDR_ANY) 同样为了兼容性问题
    //*长整型*要使用htonl()将其转换为网络字节顺序
    
    if ((setsockopt(httpd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on))) < 0) //使当前端口释放后可以立即使用  
    {  
        error_die("setsockopt failed");  //输出setsockopt选项设置失败的错误
    }

    //绑定socket到指定地址
    if (bind(httpd, (struct sockaddr *)&name, sizeof(name)) < 0)  //服务器启动时让套接字绑定一个端口 
        error_die("bind");

    //随机分配端口号
    if (*port == 0)  /* if dynamically allocating a port */ //如果端口为空 随机赋值一个端口号
    {
        socklen_t namelen = sizeof(name);
        if (getsockname(httpd, (struct sockaddr *)&name, &namelen) == -1)  //测试随机端口号是否正常进行绑定了
            error_die("getsockname");
        *port = ntohs(name.sin_port);  //将网络字节顺序转换为主机字节顺序
    }

    //进行监听 
    if (listen(httpd, 5) < 0)   //将主动连接的套接字转变为被动连接套接口 使得一个进程可以接收其他进程的请求
        error_die("listen");
    return(httpd);  //返回该套接字
}

void unimplemented(int client)
{
    char buf[1024];

    sprintf(buf, "HTTP/1.0 501 Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, SERVER_STRING);
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "Content-Type: text/html\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</TITLE></HEAD>\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
    send(client, buf, strlen(buf), 0);
    sprintf(buf, "</BODY></HTML>\r\n");
    send(client, buf, strlen(buf), 0);
}

int main(void)
{
    int server_sock = -1;
    u_short port = 4000;  //端口号
    int client_sock = -1;
    struct sockaddr_in client_name;  //包含目标地址和端口信息
    socklen_t  client_name_len = sizeof(client_name);  //客户端长度
    pthread_t newthread;  //线程ID

    server_sock = startup(&port);
    printf("httpd running on port %d\n", port); //显示端口号

    while (1)
    {
        client_sock = accept(server_sock,
                (struct sockaddr *)&client_name,
                &client_name_len);
        if (client_sock == -1)
            error_die("accept");
        /* accept_request(&client_sock); */
        if (pthread_create(&newthread , NULL, (void *)accept_request, (void *)(intptr_t)client_sock) != 0)  //第三个参数是函数的指针 要转换类型
            perror("pthread_create");
    }

    close(server_sock);

    return(0);
}