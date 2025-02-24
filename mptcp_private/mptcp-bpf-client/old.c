// SPDX-License-Identifier: GPL-2.0
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/if_arp.h>
#include <linux/if_packet.h>
#include <linux/ip.h>
#include <linux/types.h>
#include <linux/socket.h>
#include <linux/tcp.h>
//#include <netinet/in.h>
#include "bpf_helpers.h"
#include "bpf_endian.h"
#include "test_tcpbpf.h"

/* This bpf program is to let a client to signal the server (via an MPTCP option)
 * to cap the bandwidth of a subflow on server-side (by setting cwnd clamp)
 */

#define bswap_32(x) ((unsigned int)__builtin_bswap32(x))

int _version SEC("version") = 1;

struct mptcp_option {
	__u8 kind;
	__u8 len;
	__u8 rsv:4, subtype:4;
	__u8 data;
};

struct mptcp_option mp_opt = {
	.kind = 30, // MPTCP code
	.len = 4,
	.subtype = 15,
	.rsv = 0,
	.data = 12, // log2(capped bw)-> 2^12 = 4096 Kbps
};

//struct mptcp_option mp_opt;
SEC("sockops")
int bpf_testcb(struct bpf_sock_ops *skops)
{
	int rv = -1;
	int op;
	int v = 0;


	int option_len = sizeof(mp_opt);
	op = (int) skops->op;

	switch (op) {
	case BPF_SOCK_OPS_TCP_CONNECT_CB:
	{
		char fmt0[] = "tcp connect callback\n";
		bpf_trace_printk(fmt0, sizeof(fmt0));
		break;
	}
	case BPF_MPTCP_SYNACK_ARRIVED:
	{
		unsigned int id = skops->args[0];
		unsigned int dev_type = skops->args[1];
		char fmt1[] = "Client: rcv SYN-ACK on subflow: %u \t dev->type: %u\n";
		bpf_trace_printk(fmt1, sizeof(fmt1), id, dev_type);

		/* master subflow has id = 1
		 * dev_type should be cellular iface instead,
		 * this is just for testing */
		//if (id != 1  &&  dev_type == ARPHRD_LOOPBACK) {
		if (id != 100) {
			/* Enable option write callback on this subflow */
			bpf_sock_ops_cb_flags_set( skops, BPF_SOCK_OPS_OPTION_WRITE_FLAG);
			rv = 1;
		}
		else rv = 10;
		break;
	}
	case BPF_TCP_OPTIONS_SIZE_CALC:
		/* args[1] is the second argument */
		if (skops->args[1] + option_len <= 40) {
			rv = option_len;
			char fmt4[] = "OPTIONS_SIZE_CALC   original:%d add:%d bytes\n";
			bpf_trace_printk(fmt4, sizeof(fmt4), skops->args[1], option_len);
		}
		else rv = 0;
		break;
	case BPF_MPTCP_OPTIONS_WRITE:
	{
		struct mptcp_option mp_opt;	
        	mp_opt.kind = 30; // MPTCP code
        	mp_opt.len = 4;
        	mp_opt.subtype = 15;
        	mp_opt.rsv = skops->args[1];
        	//mp_opt.rsv = 256;
        	mp_opt.data = 12; // log2(capped bw)-> 2^12 = 4096 Kbps*/
		int option_buffer;
		// skops->reply_long = mp_opt;
		memcpy(&option_buffer, &mp_opt, sizeof(int));
		char fmt3[] = "OPTIONS_WRITE on subflow: %x\n\n";
		bpf_trace_printk(fmt3, sizeof(fmt3), option_buffer);
		/* put the struct option into the reply value */
		rv = option_buffer;
		break;
	}
	case BPF_MPTCP_PARSE_OPTIONS:
	{
		unsigned int clamp, val, bw, rtt;
		unsigned int mss = 1500;
		int sid;
		bpf_getsockopt(skops, IPPROTO_TCP, TCP_BPF_SNDCWND_CLAMP, &clamp, sizeof(clamp));
		char fmt11[] = "snd_cwnd clamp before = %d \n";
		bpf_trace_printk(fmt11, sizeof(fmt11), clamp);

		/* get the parsed option */
		unsigned int option = bpf_ntohl(skops->args[2]);
		sid=skops->args[1];
		val = option & 0x000000FF;

		/* bw = 2^val */
		if (val < 32)
			bw = 1 << val;
		else
			bw = val;

		rtt = (skops->srtt_us >> 3)/1000;

		char fmt10[] = "parse options: %d, %d, %x\n";
		bpf_trace_printk(fmt10, sizeof(fmt10),  skops->args[0], skops->args[1],
							option);

		char fmt12[] = "requested val:%u bw: %u KB/s sid: %d\n";
		bpf_trace_printk(fmt12, sizeof(fmt12), val, bw,sid);

		char fmt22[] = "rtt:%u ms  mss_cache:%u snd_cwnd: %u\n";
		bpf_trace_printk(fmt22, sizeof(fmt22), rtt, skops->mss_cache, skops->snd_cwnd);

		if (rtt == 0)
			break;
		/* if this is a valid MSS, use it to estimate targeted cwnd */
		/* better to use MTU, but we don't know it */
		if (skops->mss_cache > 0)
			/* convert from bytes to bits */
			mss = skops->mss_cache * 8;

		/* adding mss/2 is to round(), instead of floor() */
		clamp = (bw*rtt + mss/2)/mss;

		rv = bpf_setsockopt(skops, IPPROTO_TCP, TCP_BPF_SNDCWND_CLAMP,
							&clamp, sizeof(clamp));

		bpf_getsockopt(skops, IPPROTO_TCP, TCP_BPF_SNDCWND_CLAMP, &clamp, sizeof(clamp));
		char fmt13[] = "snd_cwnd clamp after = %d \n";
		bpf_trace_printk(fmt13, sizeof(fmt13), clamp);
		break;
	}
	default:
		rv = -1;
	}
	skops->reply = rv;
	return 1;
}
char _license[] SEC("license") = "GPL";
