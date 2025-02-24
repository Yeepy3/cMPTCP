// SPDX-License-Identifier: GPL-2.0
/*	MPTCP ECF Scheduler
 *
 *	Algorithm Design:
 *	Yeon-sup Lim <ylim@cs.umass.edu>
 *	Don Towsley <towsley@cs.umass.edu>
 *	Erich M. Nahum <nahum@us.ibm.com>
 *	Richard J. Gibbens <richard.gibbens@cl.cam.ac.uk>
 *
 *	Initial Implementation:
 *	Yeon-sup Lim <ylim@cs.umass.edu>
 *
 *	Additional Authors:
 *	Daniel Weber <weberd@cs.uni-bonn.de>
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#include <linux/module.h>
#include <net/mptcp.h>
#include <trace/events/tcp.h>

static unsigned int mptcp_ecf_r_beta __read_mostly = 4; /* beta = 1/r_beta = 0.25 */
module_param(mptcp_ecf_r_beta, int, 0644);
MODULE_PARM_DESC(mptcp_ecf_r_beta, "beta for ECF");

struct ecfsched_priv {
	u32 last_rbuf_opti;
};

struct ecfsched_cb {
	u32 switching_margin; /* this is "waiting" in algorithm description */
};

static struct ecfsched_priv *ecfsched_get_priv(const struct tcp_sock *tp)
{
	return (struct ecfsched_priv *)&tp->mptcp->mptcp_sched[0];
}

static struct ecfsched_cb *ecfsched_get_cb(const struct tcp_sock *tp)
{
	return (struct ecfsched_cb *)&tp->mpcb->mptcp_sched[0];
}

/* This is the ECF scheduler. This function decides on which flow to send
 * a given MSS. If all subflows are found to be busy or the currently best
 * subflow is estimated to be slower than waiting for minsk, NULL is returned.
 */
static struct sock *ecf_get_available_subflow(struct sock *meta_sk,
					      struct sk_buff *skb,
					      bool zero_wnd_test)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct sock *bestsk, *minsk = NULL;
	struct tcp_sock *besttp;
	struct mptcp_tcp_sock *mptcp;
	struct ecfsched_cb *ecf_cb = ecfsched_get_cb(tcp_sk(meta_sk));
	u32 min_srtt = U32_MAX;
	u32 sub_sndbuf = 0;
	u32 sub_packets_out = 0;

	/* Answer data_fin on same subflow!!! */
	if (meta_sk->sk_shutdown & RCV_SHUTDOWN &&
	    skb && mptcp_is_data_fin(skb)) {
		mptcp_for_each_sub(mpcb, mptcp) {
			bestsk = mptcp_to_sock(mptcp);

			if (tcp_sk(bestsk)->mptcp->path_index == mpcb->dfin_path_index &&
			    mptcp_is_available(bestsk, skb, zero_wnd_test))
				return bestsk;
		}
	}

	/* First, find the overall best (fastest) subflow */
	mptcp_for_each_sub(mpcb, mptcp) {
		bestsk = mptcp_to_sock(mptcp);
		besttp = tcp_sk(bestsk);

		/* Set of states for which we are allowed to send data */
		if (!mptcp_sk_can_send(bestsk))
			continue;

		/* We do not send data on this subflow unless it is
		 * fully established, i.e. the 4th ack has been received.
		 */
		if (besttp->mptcp->pre_established)
			continue;

		sub_sndbuf += bestsk->sk_wmem_queued;
		sub_packets_out += besttp->packets_out;

		/* record minimal rtt */
		if (besttp->srtt_us < min_srtt) {
			min_srtt = besttp->srtt_us;
			minsk = bestsk;
		}
	}

	/* find the current best subflow according to the default scheduler */
	bestsk = get_available_subflow(meta_sk, skb, zero_wnd_test);

	/* if we decided to use a slower flow, we have the option of not using it at all */
	if (bestsk && minsk && bestsk != minsk) {
		u32 mss = tcp_current_mss(bestsk); /* assuming equal MSS */
		u32 sndbuf_meta = meta_sk->sk_wmem_queued;
		u32 sndbuf_minus = sub_sndbuf;
		u32 sndbuf = 0;

		u32 cwnd_f = tcp_sk(minsk)->snd_cwnd;
		u32 srtt_f = tcp_sk(minsk)->srtt_us >> 3;
		u32 rttvar_f = tcp_sk(minsk)->rttvar_us >> 1;

		u32 cwnd_s = tcp_sk(bestsk)->snd_cwnd;
		u32 srtt_s = tcp_sk(bestsk)->srtt_us >> 3;
		u32 rttvar_s = tcp_sk(bestsk)->rttvar_us >> 1;

		u32 delta = max(rttvar_f, rttvar_s);

		u32 x_f;
		u64 lhs, rhs; /* to avoid overflow, using u64 */

		if (tcp_sk(meta_sk)->packets_out > sub_packets_out)
			sndbuf_minus += (tcp_sk(meta_sk)->packets_out - sub_packets_out) * mss;

		if (sndbuf_meta > sndbuf_minus)
			sndbuf = sndbuf_meta - sndbuf_minus;

		/* we have something to send.
		 * at least one time tx over fastest subflow is required
		 */
		x_f = sndbuf > cwnd_f * mss ? sndbuf : cwnd_f * mss;
		lhs = srtt_f * (x_f + cwnd_f * mss);
		rhs = cwnd_f * mss * (srtt_s + delta);

		if (mptcp_ecf_r_beta * lhs < mptcp_ecf_r_beta * rhs + ecf_cb->switching_margin * rhs) {
			u32 x_s = sndbuf > cwnd_s * mss ? sndbuf : cwnd_s * mss;
			u64 lhs_s = srtt_s * x_s;
			u64 rhs_s = cwnd_s * mss * (2 * srtt_f + delta);

			if (lhs_s >= rhs_s) {
				/* too slower than fastest */
				ecf_cb->switching_margin = 1;
				return NULL;
			}
		} else {
			/* use slower one */
			ecf_cb->switching_margin = 0;
		}
	}

	return bestsk;
}

/* copy from mptcp_sched.c: mptcp_rcv_buf_optimization */
static struct sk_buff *mptcp_ecf_rcv_buf_optimization(struct sock *sk, int penal)
{
	struct sock *meta_sk;
	const struct tcp_sock *tp = tcp_sk(sk);
	struct mptcp_tcp_sock *mptcp;
	struct sk_buff *skb_head;
	struct ecfsched_priv *ecf_p = ecfsched_get_priv(tp);

	meta_sk = mptcp_meta_sk(sk);
	skb_head = tcp_rtx_queue_head(meta_sk);

	if (!skb_head)
		return NULL;

	/* If penalization is optional (coming from mptcp_next_segment() and
	 * We are not send-buffer-limited we do not penalize. The retransmission
	 * is just an optimization to fix the idle-time due to the delay before
	 * we wake up the application.
	 */
	if (!penal && sk_stream_memory_free(meta_sk))
		goto retrans;

	/* Only penalize again after an RTT has elapsed */
	if (tcp_jiffies32 - ecf_p->last_rbuf_opti < usecs_to_jiffies(tp->srtt_us >> 3))
		goto retrans;

	/* Half the cwnd of the slow flows */
	mptcp_for_each_sub(tp->mpcb, mptcp) {
		struct tcp_sock *tp_it = mptcp->tp;

		if (tp_it != tp &&
		    TCP_SKB_CB(skb_head)->path_mask & mptcp_pi_to_flag(tp_it->mptcp->path_index)) {
			if (tp->srtt_us < tp_it->srtt_us && inet_csk((struct sock *)tp_it)->icsk_ca_state == TCP_CA_Open) {
				u32 prior_cwnd = tp_it->snd_cwnd;

				tp_it->snd_cwnd = max(tp_it->snd_cwnd >> 1U, 1U);

				/* If in slow start, do not reduce the ssthresh */
				if (prior_cwnd >= tp_it->snd_ssthresh)
					tp_it->snd_ssthresh = max(tp_it->snd_ssthresh >> 1U, 2U);

				ecf_p->last_rbuf_opti = tcp_jiffies32;
			}
		}
	}

retrans:

	/* Segment not yet injected into this path? Take it!!! */
	if (!(TCP_SKB_CB(skb_head)->path_mask & mptcp_pi_to_flag(tp->mptcp->path_index))) {
		bool do_retrans = false;
		mptcp_for_each_sub(tp->mpcb, mptcp) {
			struct tcp_sock *tp_it = mptcp->tp;

			if (tp_it != tp &&
			    TCP_SKB_CB(skb_head)->path_mask & mptcp_pi_to_flag(tp_it->mptcp->path_index)) {
				if (tp_it->snd_cwnd <= 4) {
					do_retrans = true;
					break;
				}

				if (4 * tp->srtt_us >= tp_it->srtt_us) {
					do_retrans = false;
					break;
				} else {
					do_retrans = true;
				}
			}
		}

		if (do_retrans && mptcp_is_available(sk, skb_head, false)) {
			//trace_mptcp_retransmit(sk, skb_head);
			return skb_head;
		}
	}
	return NULL;
}

/* copy from mptcp_sched.c: __mptcp_next_segment */
/* Returns the next segment to be sent from the mptcp meta-queue.
 * (chooses the reinject queue if any segment is waiting in it, otherwise,
 * chooses the normal write queue).
 * Sets *@reinject to 1 if the returned segment comes from the
 * reinject queue. Sets it to 0 if it is the regular send-head of the meta-sk,
 * and sets it to -1 if it is a meta-level retransmission to optimize the
 * receive-buffer.
 */
static struct sk_buff *__mptcp_ecf_next_segment(struct sock *meta_sk, int *reinject)
{
	const struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct sk_buff *skb = NULL;

	*reinject = 0;

	/* If we are in fallback-mode, just take from the meta-send-queue */
	if (mpcb->infinite_mapping_snd || mpcb->send_infinite_mapping)
		return tcp_send_head(meta_sk);

	skb = skb_peek(&mpcb->reinject_queue);

	if (skb) {
		*reinject = 1;
	} else {
		skb = tcp_send_head(meta_sk);

		if (!skb && meta_sk->sk_socket &&
		    test_bit(SOCK_NOSPACE, &meta_sk->sk_socket->flags) &&
		    sk_stream_wspace(meta_sk) < sk_stream_min_wspace(meta_sk)) {
			struct sock *subsk = ecf_get_available_subflow(meta_sk, NULL,
								   false);
			if (!subsk)
				return NULL;

			skb = mptcp_ecf_rcv_buf_optimization(subsk, 0);
			if (skb)
				*reinject = -1;
		}
	}
	return skb;
}

/* copy from mptcp_sched.c: mptcp_next_segment */
static struct sk_buff *mptcp_ecf_next_segment(struct sock *meta_sk,
					      int *reinject,
					      struct sock **subsk,
					      unsigned int *limit)
{
	struct sk_buff *skb = __mptcp_ecf_next_segment(meta_sk, reinject);
	unsigned int mss_now;
	struct tcp_sock *subtp;
	u16 gso_max_segs;
	u32 max_len, max_segs, window, needed;

	/* As we set it, we have to reset it as well. */
	*limit = 0;

	if (!skb)
		return NULL;

	*subsk = ecf_get_available_subflow(meta_sk, skb, false);
	if (!*subsk)
		return NULL;

	subtp = tcp_sk(*subsk);
	mss_now = tcp_current_mss(*subsk);

	if (!*reinject && unlikely(!tcp_snd_wnd_test(tcp_sk(meta_sk), skb, mss_now))) {
		skb = mptcp_ecf_rcv_buf_optimization(*subsk, 1);
		if (skb)
			*reinject = -1;
		else
			return NULL;
	}

	/* No splitting required, as we will only send one single segment */
	if (skb->len <= mss_now)
		return skb;

	/* The following is similar to tcp_mss_split_point, but
	 * we do not care about nagle, because we will anyways
	 * use TCP_NAGLE_PUSH, which overrides this.
	 *
	 * So, we first limit according to the cwnd/gso-size and then according
	 * to the subflow's window.
	 */

	gso_max_segs = (*subsk)->sk_gso_max_segs;
	if (!gso_max_segs) /* No gso supported on the subflow's NIC */
		gso_max_segs = 1;
	max_segs = min_t(unsigned int, tcp_cwnd_test(subtp, skb), gso_max_segs);
	if (!max_segs)
		return NULL;

	max_len = mss_now * max_segs;
	window = tcp_wnd_end(subtp) - subtp->write_seq;

	needed = min(skb->len, window);
	if (max_len <= skb->len)
		/* Take max_win, which is actually the cwnd/gso-size */
		*limit = max_len;
	else
		/* Or, take the window */
		*limit = needed;

	return skb;
}

static void ecfsched_init(struct sock *sk)
{
	struct ecfsched_priv *ecf_p = ecfsched_get_priv(tcp_sk(sk));
	struct ecfsched_cb *ecf_cb = ecfsched_get_cb(tcp_sk(mptcp_meta_sk(sk)));

	ecf_p->last_rbuf_opti = tcp_jiffies32;
	ecf_cb->switching_margin = 0;
}

struct mptcp_sched_ops mptcp_sched_ecf = {
	.get_subflow = ecf_get_available_subflow,
	.next_segment = mptcp_ecf_next_segment,
	.init = ecfsched_init,
	.name = "ecf",
	.owner = THIS_MODULE,
};

static int __init ecf_register(void)
{
	BUILD_BUG_ON(sizeof(struct ecfsched_priv) > MPTCP_SCHED_SIZE);
	BUILD_BUG_ON(sizeof(struct ecfsched_cb) > MPTCP_SCHED_DATA_SIZE);

	if (mptcp_register_scheduler(&mptcp_sched_ecf))
		return -1;

	return 0;
}

static void ecf_unregister(void)
{
	mptcp_unregister_scheduler(&mptcp_sched_ecf);
}

module_init(ecf_register);
module_exit(ecf_unregister);

MODULE_AUTHOR("Yeon-sup Lim, Daniel Weber");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ECF (Earliest Completion First) scheduler for MPTCP, based on default minimum RTT scheduler");
MODULE_VERSION("0.95");
