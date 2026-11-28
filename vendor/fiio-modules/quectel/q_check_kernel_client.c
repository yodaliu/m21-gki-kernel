// SPDX-License-Identifier: GPL-2.0
/*
 * Quectel model check: talks to the local modem service on 127.0.0.1:9083
 * with AT commands (MDEXISTS, PCODE, MDONLINE, QPRIBIND) and checks the
 * replies (NV data hash, or a check code derived from a random number).
 *
 * Reconstructed from the stock FiiO M21 FW 1.1.0 q_check_kernel_client.ko
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/random.h>
#include <linux/delay.h>
#include <linux/timekeeping.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/inet.h>
#include <linux/socket.h>
#include <linux/uio.h>
#include <net/sock.h>
#include <net/net_namespace.h>

#define SERVER_PORT	9083
#define BUF_SIZE	1024

static inline void sock_close(struct socket *sock)
{
	if (sock) {
		kernel_sock_shutdown(sock, SHUT_RDWR);
		sock_release(sock);
	}
}

int kernel_release(struct socket *sock)
{
	if (sock) {
		kernel_sock_shutdown(sock, SHUT_RDWR);
		sock_release(sock);
	}
	return 0;
}

int find(char *src, char *sub)
{
	char *str, *buf;
	int len;

	str = strchr(src, '\n');
	if (!str) {
		printk(KERN_ERR "kernelCheck after splite str = NULL");
		return 0;
	}
	len = strlen(str) * 2 + 1;
	buf = kmalloc(len, GFP_KERNEL);
	memset(buf, 0, len);
	strcpy(buf, str);
	strcat(buf, str);
	return strstr(buf, sub) != NULL;
}

int getRandomNum(void)
{
	int num = 0;

	get_random_bytes(&num, sizeof(num));
	if (num == 0)
		num = 1;
	return abs(num % 10000);
}

int getCheckedNum(int num)
{
	char key[128] = {0};
	int len, sum = 0, add = 0, i;

	strcpy(key, "quectel");
	len = strlen(key);
	if (len <= 0)
		return 0;
	for (i = 0; i < len; i++) {
		sum += key[i] * num;
		add += num + key[i];
	}
	return sum - add;
}

long getBootTime(void)
{
	struct timespec64 ts = ns_to_timespec64(ktime_get_boottime());

	return ts.tv_sec;
}

int quectel_kernel_model_check(void)
{
	struct socket *sock = NULL;
	struct sockaddr_in s_addr;
	char send_buf[BUF_SIZE];
	char recv_buf[BUF_SIZE];
	char check_code[64] = {0};
	int ret, num, i;

	memset(send_buf, 0, sizeof(send_buf));
	memset(recv_buf, 0, sizeof(recv_buf));
	memset(&s_addr, 0, sizeof(s_addr));
	s_addr.sin_family = AF_INET;
	s_addr.sin_port = htons(SERVER_PORT);
	s_addr.sin_addr.s_addr = in_aton("127.0.0.1");

	sock = kmalloc(sizeof(struct socket), GFP_KERNEL);
	ret = sock_create_kern(&init_net, AF_INET, SOCK_STREAM, 0, &sock);
	if (ret < 0) {
		printk(KERN_ERR "kernelCheck kernel socket create error!");
		return -10;
	}

	for (i = 0; i < 11; i++) {
		ret = kernel_connect(sock, (struct sockaddr *)&s_addr, sizeof(s_addr), 0);
		if (!ret)
			break;
		printk(KERN_ERR "kernelCheck kernel socket connect error!");
		msleep(1000);
	}
	if (ret)
		goto out_release;

	{
		struct msghdr recv_msg, send_msg;
		struct kvec recv_vec, send_vec;

		memset(send_buf, 0, sizeof(send_buf));
		strcpy(send_buf, "AT+QCMD=\"MDEXISTS\"");
		memset(&send_msg, 0, sizeof(send_msg));
		send_vec.iov_base = send_buf;
		send_vec.iov_len = BUF_SIZE;
		ret = kernel_sendmsg(sock, &send_msg, &send_vec, 1, BUF_SIZE);
		if (ret < 0) {
			printk(KERN_ERR "kernelCheck send msg error");
			if (getBootTime() < 600) {
				printk(KERN_ERR "kernelCheck boot time less than 10 min");
				sock_close(sock);
				return 1;
			}
			printk(KERN_ERR "kernelCheck boot time more than 10 min");
			sock_close(sock);
			return -1;
		}

		memset(recv_buf, 0, sizeof(recv_buf));
		memset(&recv_msg, 0, sizeof(recv_msg));
		recv_vec.iov_base = recv_buf;
		recv_vec.iov_len = BUF_SIZE;
		kernel_recvmsg(sock, &recv_msg, &recv_vec, 1, BUF_SIZE, 0);

		if (find(recv_buf, "NOT READY")) {
			printk(KERN_ERR "kernelCheck modem not ready\n");
			if (getBootTime() < 600) {
				printk(KERN_ERR "kernelCheck boot time less than 10 min");
				sock_close(sock);
				return 2;
			}
			printk(KERN_ERR "kernelCheck boot time more than 10 min");
			goto out_release;
		}

		if (find(recv_buf, "NOT EXISTS")) {
			printk(KERN_ERR "kernelCheck not exits modem");
			memset(send_buf, 0, sizeof(send_buf));
			strcpy(send_buf, "AT+QCMD=\"PCODE\"");
			memset(&send_msg, 0, sizeof(send_msg));
			send_vec.iov_base = send_buf;
			send_vec.iov_len = BUF_SIZE;
			ret = kernel_sendmsg(sock, &send_msg, &send_vec, 1, BUF_SIZE);
			if (!ret) {
				printk(KERN_ERR "kernelCheck send msg %s, error = %d", send_buf, ret);
				sock_close(sock);
				return -1;
			}

			memset(recv_buf, 0, sizeof(recv_buf));
			memset(&recv_msg, 0, sizeof(recv_msg));
			recv_vec.iov_base = recv_buf;
			recv_vec.iov_len = BUF_SIZE;
			kernel_recvmsg(sock, &recv_msg, &recv_vec, 1, BUF_SIZE, 0);
			printk(KERN_ERR "kernelCheck quectel receive msg form server = %s", recv_buf);
			if (!find(recv_buf, "f9827a7011315fd528ce3c32c5eba1e25c28ec084618c8ccd1123a72ee254535")) {
				printk(KERN_ERR "kernelCheck NV data check failed ");
				kernel_release(sock);
				return -4;
			}
			printk(KERN_ERR "kernelCheck NV data check success ");
			kernel_release(sock);
			return 0;
		}

		if (find(recv_buf, "EXISTS")) {
			printk(KERN_ERR "kernelCheck exits modem ");
			memset(send_buf, 0, sizeof(send_buf));
			strcpy(send_buf, "AT+QCMD=\"MDONLINE\"");
			memset(&send_msg, 0, sizeof(send_msg));
			send_vec.iov_base = send_buf;
			send_vec.iov_len = BUF_SIZE;
			ret = kernel_sendmsg(sock, &send_msg, &send_vec, 1, BUF_SIZE);
			if (ret <= 0) {
				printk(KERN_ERR "kernelCheck send msg %s, error = %d\n", send_buf, ret);
				sock_close(sock);
				return -1;
			}

			memset(recv_buf, 0, sizeof(recv_buf));
			memset(&recv_msg, 0, sizeof(recv_msg));
			recv_vec.iov_base = recv_buf;
			recv_vec.iov_len = BUF_SIZE;
			kernel_recvmsg(sock, &recv_msg, &recv_vec, 1, BUF_SIZE, 0);
			printk(KERN_ERR "kernelCheck quectel receive msg form server = %s", recv_buf);

			if (find(recv_buf, "ONLINE")) {
				num = getRandomNum();
				printk(KERN_ERR "kernelCheck modem online\n");
				memset(send_buf, 0, sizeof(send_buf));
				sprintf(send_buf, "AT+QPRIBIND=%d", num);
				printk(KERN_ERR "kernelCheck modem omline send msg = %s,randome = %d,checkcode = %d\n",
				       send_buf, num, getCheckedNum(num));
				memset(&send_msg, 0, sizeof(send_msg));
				send_vec.iov_base = send_buf;
				send_vec.iov_len = BUF_SIZE;
				ret = kernel_sendmsg(sock, &send_msg, &send_vec, 1, BUF_SIZE);
				if (!ret) {
					printk(KERN_ERR "kernelCheck send msg %s, error = %d\n", send_buf, ret);
					kernel_release(sock);
					return -1;
				}

				memset(recv_buf, 0, sizeof(recv_buf));
				memset(&recv_msg, 0, sizeof(recv_msg));
				recv_vec.iov_base = recv_buf;
				recv_vec.iov_len = BUF_SIZE;
				kernel_recvmsg(sock, &recv_msg, &recv_vec, 1, BUF_SIZE, 0);
				printk(KERN_ERR "kernelCheck quectel receive msg form server = %s", recv_buf);
				sprintf(check_code, "%d", getCheckedNum(num));
				if (!find(recv_buf, check_code)) {
					printk(KERN_ERR "kernelCheck checkCode failed");
					kernel_release(sock);
					return -2;
				}
				printk(KERN_ERR "kernelCheck checkCode success");
				kernel_release(sock);
				return 0;
			}

			if (find(recv_buf, "OFFLINE") || find(recv_buf, "NOT READY")) {
				printk(KERN_ERR "kernelCheck modem offline or not ready\n");
				if (getBootTime() < 600) {
					printk(KERN_ERR "kernelCheck boot time less than 10 min");
					kernel_release(sock);
					return 2;
				}
				printk(KERN_ERR "kernelCheck boot time more than 10 min");
				kernel_release(sock);
				return -10;
			}
		}
	}

out_release:
	sock_close(sock);
	return -10;

}
/*
 * The stock build has no CRC for this export (genksyms did not see it, so
 * __crc_quectel_kernel_model_check stays an undefined weak symbol). Hide the
 * export from genksyms to get the same result.
 */
#ifndef __GENKSYMS__
EXPORT_SYMBOL(quectel_kernel_model_check);
#endif

static int __init quectel_check_init(void)
{
	printk(KERN_DEBUG "quetel_Check loaded\n");
	return 0;
}

static void __exit quectel_check_exit(void)
{
	printk(KERN_INFO "quetel_Check unloaded\n");
}

module_init(quectel_check_init);
module_exit(quectel_check_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ethan");
MODULE_DESCRIPTION("quectel check");
