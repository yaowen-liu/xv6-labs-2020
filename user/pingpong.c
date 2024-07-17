#include "kernel/types.h"
#include "user/user.h"



int main(int argc,char* argv[])
{
    if(argc!=1)
    {
        fprintf(2,"Usage:pingpong\r\n");
        exit(1);
    }
    int pipeFd1[2],pipeFd2[2];/* 第一个是父->子 第二个是子->父*/
    /* 创建管道 */
    pipe(pipeFd1);
    pipe(pipeFd2);
    int pid;
    /* 创建子进程 */
    pid=fork();
    if(pid==0)
    {
        /* 子进程 */
        int pidZ=getpid();
        char buf[1024];
        int len=read(pipeFd1[0],buf,sizeof(buf));
        if(len==-1)
        {
            fprintf(2,"read error\r\n");
        }
        fprintf(1,"%d: received ping\r\n",pidZ);
        write(pipeFd2[1],"1",1);
    }
    else
    {
        /* 父进程 */
        int pidF=getpid();
        write(pipeFd1[1],"1",1);
        char buf[1024];
        int len=read(pipeFd2[0],buf,sizeof(buf));
        if(len==-1)
        {
            fprintf(2,"read error\r\n");
        }
        fprintf(1,"%d: received pong\r\n",pidF);
    }
    exit(1);
}