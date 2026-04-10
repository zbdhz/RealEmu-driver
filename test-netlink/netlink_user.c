#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define NETLINK_MY_TEST 31 // 与内核模块相同的协议号
#define MAX_PAYLOAD 1024   // 最大消息负载
struct sockaddr_nl src_addr, dest_addr;
struct nlmsghdr *nlh = NULL;
struct iovec iov;
int sock_fd;
struct msghdr msg;

int main() {
  sock_fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_MY_TEST);
  if (sock_fd < 0) {
    printf("Error creating socket\n");
    return -1;
  }
  // 设置源地址（本进程）
  memset(&src_addr, 0, sizeof(src_addr));
  src_addr.nl_family = AF_NETLINK;
  src_addr.nl_pid = getpid(); // 使用进程ID作为端口ID
  src_addr.nl_groups = 0;     // 不加入任何多播组
  // 绑定套接字
  if (bind(sock_fd, (struct sockaddr *)&src_addr, sizeof(src_addr)) < 0) {
    printf("Error binding socket\n");
    close(sock_fd);
    return -1;
  }
  // 设置目标地址（内核）
  memset(&dest_addr, 0, sizeof(dest_addr));
  dest_addr.nl_family = AF_NETLINK;
  dest_addr.nl_pid = 0;    // 内核的PID为0
  dest_addr.nl_groups = 0; // 非多播
  // 分配和设置Netlink消息
  nlh = (struct nlmsghdr *)malloc(NLMSG_SPACE(MAX_PAYLOAD));
  memset(nlh, 0, NLMSG_SPACE(MAX_PAYLOAD));
  nlh->nlmsg_len = NLMSG_SPACE(MAX_PAYLOAD);
  nlh->nlmsg_pid = getpid();
  nlh->nlmsg_flags = 0;
  // 设置消息内容
  strcpy(NLMSG_DATA(nlh), "Hello from userspace");
  // 设置IO向量
  iov.iov_base = (void *)nlh;
  iov.iov_len = nlh->nlmsg_len;
  // 设置消息结构
  memset(&msg, 0, sizeof(msg));
  msg.msg_name = (void *)&dest_addr;
  msg.msg_namelen = sizeof(dest_addr);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  printf("Sending message to kernel: %s\n", (char *)NLMSG_DATA(nlh));
  // 发送消息到内核
  if (sendmsg(sock_fd, &msg, 0) < 0) {
    printf("Error sending message\n");
    close(sock_fd);
    return -1;
  }
  printf("Waiting for kernel reply...\n");
  // 接收内核回复
  if (recvmsg(sock_fd, &msg, 0) < 0) {
    printf("Error receiving message\n");
  } else {
    printf("Received kernel reply: %s\n", (char *)NLMSG_DATA(nlh));
  }
  close(sock_fd);
  free(nlh);
  return 0;
}