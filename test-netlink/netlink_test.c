#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#define NETLINK_MY_TEST 31 // 自定义Netlink协议号
static struct sock *nl_sk = NULL;

static void nl_recv_msg(struct sk_buff *skb) {
  struct nlmsghdr *nlh;
  int pid;
  struct sk_buff *skb_out;
  int msg_size;
  char *msg = "Hello from kernel";
  int res;
  printk(KERN_INFO "Netlink received message\n");
  // 解析收到的消息
  nlh = (struct nlmsghdr *)skb->data;
  pid = nlh->nlmsg_pid; // 获取发送进程的PID
  printk(KERN_INFO "Received from PID %d: %s\n", pid, (char *)NLMSG_DATA(nlh));
  // 准备回复消息
  msg_size = strlen(msg);
  skb_out = nlmsg_new(msg_size, 0);
  if (!skb_out) {
    printk(KERN_ERR "Failed to allocate new skb\n");
    return;
  }
  // 构造回复消息头
  nlh = nlmsg_put(skb_out, 0, 0, NLMSG_DONE, msg_size, 0);
  NETLINK_CB(skb_out).dst_group = 0; // 非多播消息
  strncpy(nlmsg_data(nlh), msg, msg_size);
  // 发送单播回复
  res = nlmsg_unicast(nl_sk, skb_out, pid);
  if (res < 0) {
    printk(KERN_ERR "Error while sending reply to user\n");
  }
}

static int __init netlink_test_init(void) {
  struct netlink_kernel_cfg cfg = {
      .input = nl_recv_msg, // 注册消息接收回调
  };
  printk(KERN_INFO "Initializing Netlink test module\n");
  // 创建Netlink套接字
  nl_sk = netlink_kernel_create(&init_net, NETLINK_MY_TEST, &cfg);
  if (!nl_sk) {
    printk(KERN_ALERT "Failed to create Netlink socket\n");
    return -ENOMEM;
  }
  printk(KERN_INFO "Netlink test module initialized\n");
  return 0;
}

static void __exit netlink_test_exit(void) {
  printk(KERN_INFO "Exiting Netlink test module\n");
  if (nl_sk) {
    netlink_kernel_release(nl_sk); // 释放Netlink套接字
    nl_sk = NULL;
  }
  printk(KERN_INFO "Netlink test module exited\n");
}
module_init(netlink_test_init);
module_exit(netlink_test_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Netlink Test Author");
MODULE_DESCRIPTION("A simple Netlink test module");