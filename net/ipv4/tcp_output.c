/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Implementation of the Transmission Control Protocol(TCP).
 *
 * Authors:	Ross Biro
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Mark Evans, <evansmp@uhura.aston.ac.uk>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *		Florian La Roche, <flla@stud.uni-sb.de>
 *		Charles Hedrick, <hedrick@klinzhai.rutgers.edu>
 *		Linus Torvalds, <torvalds@cs.helsinki.fi>
 *		Alan Cox, <gw4pts@gw4pts.ampr.org>
 *		Matthew Dillon, <dillon@apollo.west.oic.com>
 *		Arnt Gulbrandsen, <agulbra@nvg.unit.no>
 *		Jorge Cwik, <jorge@laser.satlink.net>
 */

/*
 * Changes:	Pedro Roque	:	Retransmit queue handled by TCP.
 *				:	Fragmentation on mtu decrease
 *				:	Segment collapse on retransmit
 *				:	AF independence
 *
 *		Linus Torvalds	:	send_delayed_ack
 *		David S. Miller	:	Charge memory using the right skb
 *					during syn/ack processing.
 *		David S. Miller :	Output engine completely rewritten.
 *		Andrea Arcangeli:	SYNACK carry ts_recent in tsecr.
 *		Cacophonix Gaul :	draft-minshall-nagle-01
 *		J Hadi Salim	:	ECN support
 *
 */

#include <net/tcp.h>

#include <linux/compiler.h>
#include <linux/module.h>
#include <linux/tcp_probe.h>

/* People can turn this off for buggy TCP's found in printers etc. */
int sysctl_tcp_retrans_collapse __read_mostly = 1;

/* People can turn this on to  work with those rare, broken TCPs that
 * interpret the window field as a signed quantity.
 */
int sysctl_tcp_workaround_signed_windows __read_mostly = 0;

/* This limits the percentage of the congestion window which we
 * will allow a single TSO frame to consume.  Building TSO frames
 * which are too large can cause TCP streams to be bursty.
 */
int sysctl_tcp_tso_win_divisor __read_mostly = 3;

int sysctl_tcp_mtu_probing __read_mostly = 0;
int sysctl_tcp_base_mss __read_mostly = 512;

/* By default, RFC2861 behavior.  */
int sysctl_tcp_slow_start_after_idle __read_mostly = 1;

/*TODEL*/
int tocheck=0;
struct sk_buff *check_skb;
struct sock *check_sk;

static void tcp_event_new_data_sent(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);
	unsigned int prior_packets = tp->packets_out;
	int meta_sk=is_meta_tp(tp);

	if (tocheck) {
		BUG_ON(check_skb==skb);
		BUG_ON(tcp_send_head(check_sk)!=check_skb);
	}

	check_send_head(sk,2);
	BUG_ON(tcp_send_head(sk)!=skb);
	check_pkts_out(sk);
	tcp_advance_send_head(sk, skb);
	check_send_head(sk,3);
	if (tocheck)
		BUG_ON(tcp_send_head(check_sk)!=check_skb);
	tp->snd_nxt = meta_sk?TCP_SKB_CB(skb)->end_data_seq:
		TCP_SKB_CB(skb)->end_seq;

	/* Don't override Nagle indefinitely with F-RTO */
	if (tp->frto_counter == 2)
		tp->frto_counter = 3;

	tp->packets_out += tcp_skb_pcount(skb);
	if (!prior_packets && !meta_sk) {
		tcpprobe_logmsg(sk,"setting RTO to %d ms",
				inet_csk(sk)->icsk_rto*1000/HZ);
		inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
					  inet_csk(sk)->icsk_rto, TCP_RTO_MAX);
	}
	if (tocheck)
		BUG_ON(tcp_send_head(check_sk)!=check_skb);
	
	check_pkts_out(sk);
	check_send_head(sk,5);
}

/* SND.NXT, if window was not shrunk.
 * If window has been shrunk, what should we make? It is not clear at all.
 * Using SND.UNA we will fail to open window, SND.NXT is out of window. :-(
 * Anything in between SND.UNA...SND.UNA+SND.WND also can be already
 * invalid. OK, let's make this for now:
 */
static inline __u32 tcp_acceptable_seq(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	/*We do not call tcp_wnd_end(..,1) here, 
	  because even when MPTCP is used, 
	  we exceptionnaly want here to consider the send window as related to
	  the seqnums, not the dataseqs. The reason is that we have no dataseq
	  nums in non-data segments (this function is only called for the
	  construction of non-data segments, e.g. acks), and the dataseq is now
	  the only field that can be checked by the receiver. The seqnum we
	  choose here ensure that we are accepted as well by middleboxes
	  that are not aware of MPTCP stuff.*/
	
	if (!before(tcp_wnd_end(tp,0), tp->snd_nxt))
		return tp->snd_nxt;
	else
		return tcp_wnd_end(tp,0);
}

/* Calculate mss to advertise in SYN segment.
 * RFC1122, RFC1063, draft-ietf-tcpimpl-pmtud-01 state that:
 *
 * 1. It is independent of path mtu.
 * 2. Ideally, it is maximal possible segment size i.e. 65535-40.
 * 3. For IPv4 it is reasonable to calculate it from maximal MTU of
 *    attached devices, because some buggy hosts are confused by
 *    large MSS.
 * 4. We do not make 3, we advertise MSS, calculated from first
 *    hop device mtu, but allow to raise it to ip_rt_min_advmss.
 *    This may be overridden via information stored in routing table.
 * 5. Value 65535 for MSS is valid in IPv6 and means "as large as possible,
 *    probably even Jumbo".
 */
static __u16 tcp_advertise_mss(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct dst_entry *dst = __sk_dst_get(sk);
	int mss = tp->advmss;

	if (dst && dst_metric(dst, RTAX_ADVMSS) < mss) {
		mss = dst_metric(dst, RTAX_ADVMSS);
		tp->advmss = mss;
#ifdef CONFIG_MTCP
		tp->mss_too_low=1;
#endif
	}

	return (__u16)mss;
}

/* RFC2861. Reset CWND after idle period longer RTO to "restart window".
 * This is the first part of cwnd validation mechanism. */
static void tcp_cwnd_restart(struct sock *sk, struct dst_entry *dst)
{
	struct tcp_sock *tp = tcp_sk(sk);
	s32 delta = tcp_time_stamp - tp->lsndtime;
	u32 restart_cwnd = tcp_init_cwnd(tp, dst);
	u32 cwnd = tp->snd_cwnd;

	tcp_ca_event(sk, CA_EVENT_CWND_RESTART);

	tp->snd_ssthresh = tcp_current_ssthresh(sk);
	restart_cwnd = min(restart_cwnd, cwnd);

	while ((delta -= inet_csk(sk)->icsk_rto) > 0 && cwnd > restart_cwnd)
		cwnd >>= 1;
	tp->snd_cwnd = max(cwnd, restart_cwnd);
	tp->snd_cwnd_stamp = tcp_time_stamp;
	tp->snd_cwnd_used = 0;
}

static void tcp_event_data_sent(struct tcp_sock *tp,
				struct sk_buff *skb, struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	const u32 now = tcp_time_stamp;

	if (sysctl_tcp_slow_start_after_idle &&
	    (!tp->packets_out && (s32)(now - tp->lsndtime) > icsk->icsk_rto))
		tcp_cwnd_restart(sk, __sk_dst_get(sk));

	tp->lsndtime = now;

	/* If it is a reply for ato after last received
	 * packet, enter pingpong mode.
	 */
	if ((u32)(now - icsk->icsk_ack.lrcvtime) < icsk->icsk_ack.ato)
		icsk->icsk_ack.pingpong = 1;
}

static inline void tcp_event_ack_sent(struct sock *sk, unsigned int pkts)
{
	tcp_dec_quickack_mode(sk, pkts);
	inet_csk_clear_xmit_timer(sk, ICSK_TIME_DACK);
}

/* Determine a window scaling and initial window to offer.
 * Based on the assumption that the given amount of space
 * will be offered. Store the results in the tp structure.
 * NOTE: for smooth operation initial space offering should
 * be a multiple of mss if possible. We assume here that mss >= 1.
 * This MUST be enforced by all callers.
 */
void tcp_select_initial_window(int __space, __u32 mss,
			       __u32 *rcv_wnd, __u32 *window_clamp,
			       int wscale_ok, __u8 *rcv_wscale)
{
	unsigned int space = (__space < 0 ? 0 : __space);

	/* If no clamp set the clamp to the max possible scaled window */
	if (*window_clamp == 0)
		(*window_clamp) = (65535 << 14);
	space = min(*window_clamp, space);

	/* Quantize space offering to a multiple of mss if possible. */
	if (space > mss)
		space = (space / mss) * mss;

	/* NOTE: offering an initial window larger than 32767
	 * will break some buggy TCP stacks. If the admin tells us
	 * it is likely we could be speaking with such a buggy stack
	 * we will truncate our initial window offering to 32K-1
	 * unless the remote has sent us a window scaling option,
	 * which we interpret as a sign the remote TCP is not
	 * misinterpreting the window field as a signed quantity.
	 */
	if (sysctl_tcp_workaround_signed_windows)
		(*rcv_wnd) = min(space, MAX_TCP_WINDOW);
	else
		(*rcv_wnd) = space;

	(*rcv_wscale) = 0;
	if (wscale_ok) {
		/* Set window scaling on max possible window
		 * See RFC1323 for an explanation of the limit to 14
		 */
		space = max_t(u32, sysctl_tcp_rmem[2], sysctl_rmem_max);
		space = min_t(u32, space, *window_clamp);
		while (space > 65535 && (*rcv_wscale) < 14) {
			space >>= 1;
			(*rcv_wscale)++;
		}
	}

	/* Set initial window to value enough for senders,
	 * following RFC2414. Senders, not following this RFC,
	 * will be satisfied with 2.
	 */
	if (mss > (1 << *rcv_wscale)) {
		int init_cwnd = 4;
		if (mss > 1460 * 3)
			init_cwnd = 2;
		else if (mss > 1460)
			init_cwnd = 3;
		if (*rcv_wnd > init_cwnd * mss)
			*rcv_wnd = init_cwnd * mss;
	}

	/* Set the clamp no higher than max representable value */
	(*window_clamp) = min(65535U << (*rcv_wscale), *window_clamp);
}

/* Chose a new window to advertise, update state in tcp_sock for the
 * socket, and return result with RFC1323 scaling applied.  The return
 * value can be stuffed directly into th->window for an outgoing
 * frame.
 */
static u16 tcp_select_window(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 cur_win = tcp_receive_window(tp);
	u32 new_win = __tcp_select_window(sk);

	BUG_ON(is_meta_sk(sk));

	/* Never shrink the offered window */
	if (new_win < cur_win) {
		/* Danger Will Robinson!
		 * Don't update rcv_wup/rcv_wnd here or else
		 * we will not be able to advertise a zero
		 * window in time.  --DaveM
		 *
		 * Relax Will Robinson.
		 */
		new_win = ALIGN(cur_win, 1 << tp->rx_opt.rcv_wscale);
	}
	if (tp->mpcb && tp->mpc) {
		struct tcp_sock *mpcb_tp=(struct tcp_sock*)(tp->mpcb);
		mpcb_tp->rcv_wnd = new_win;
		mpcb_tp->rcv_wup = mpcb_tp->rcv_nxt;
		/*the subsock rcv_wup must still be updated,
		  because it is used to decide when to echo the timestamp
		  and when to delay the acks*/
		tp->rcv_wup=tp->rcv_nxt;
	}
	else {
		tp->rcv_wnd = new_win;
		tp->rcv_wup = tp->rcv_nxt;
	}

	/* Make sure we do not exceed the maximum possible
	 * scaled window.
	 */
	if (!tp->rx_opt.rcv_wscale && sysctl_tcp_workaround_signed_windows)
		new_win = min(new_win, MAX_TCP_WINDOW);
	else
		new_win = min(new_win, (65535U << tp->rx_opt.rcv_wscale));

	/* RFC1323 scaling applied */
	new_win >>= tp->rx_opt.rcv_wscale;

	/* If we advertise zero window, disable fast path. */
	if (new_win == 0)
		tp->pred_flags = 0;

	sk->sk_debug=0;
	return new_win;
}

static inline void TCP_ECN_send_synack(struct tcp_sock *tp, struct sk_buff *skb)
{
	TCP_SKB_CB(skb)->flags &= ~TCPCB_FLAG_CWR;
	if (!(tp->ecn_flags & TCP_ECN_OK))
		TCP_SKB_CB(skb)->flags &= ~TCPCB_FLAG_ECE;
}

static inline void TCP_ECN_send_syn(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);

	tp->ecn_flags = 0;
	if (sysctl_tcp_ecn) {
		TCP_SKB_CB(skb)->flags |= TCPCB_FLAG_ECE | TCPCB_FLAG_CWR;
		tp->ecn_flags = TCP_ECN_OK;
	}
}

static __inline__ void
TCP_ECN_make_synack(struct request_sock *req, struct tcphdr *th)
{
	if (inet_rsk(req)->ecn_ok)
		th->ece = 1;
}

static inline void TCP_ECN_send(struct sock *sk, struct sk_buff *skb,
				int tcp_header_len)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (tp->ecn_flags & TCP_ECN_OK) {
		/* Not-retransmitted data segment: set ECT and inject CWR. */
		if (skb->len != tcp_header_len &&
		    !before(TCP_SKB_CB(skb)->seq, tp->snd_nxt)) {
			INET_ECN_xmit(sk);
			if (tp->ecn_flags & TCP_ECN_QUEUE_CWR) {
				tp->ecn_flags &= ~TCP_ECN_QUEUE_CWR;
				tcp_hdr(skb)->cwr = 1;
				skb_shinfo(skb)->gso_type |= SKB_GSO_TCP_ECN;
			}
		} else {
			/* ACK or retransmitted segment: clear ECT|CE */
			INET_ECN_dontxmit(sk);
		}
		if (tp->ecn_flags & TCP_ECN_DEMAND_CWR)
			tcp_hdr(skb)->ece = 1;
	}
}

/* Constructs common control bits of non-data skb. If SYN/FIN is present,
 * auto increment end seqno.
 */
void tcp_init_nondata_skb(struct sk_buff *skb, u32 seq, u8 flags)
{
	skb->csum = 0;

	TCP_SKB_CB(skb)->flags = flags;
	TCP_SKB_CB(skb)->sacked = 0;

	skb_shinfo(skb)->gso_segs = 1;
	skb_shinfo(skb)->gso_size = 0;
	skb_shinfo(skb)->gso_type = 0;

	TCP_SKB_CB(skb)->seq = seq;
	if (flags & (TCPCB_FLAG_SYN | TCPCB_FLAG_FIN))
		seq++;
	TCP_SKB_CB(skb)->end_seq = seq;
}

static inline int tcp_urg_mode(const struct tcp_sock *tp)
{
	return tp->snd_una != tp->snd_up;
}


/* Beware: Something in the Internet is very sensitive to the ordering of
 * TCP options, we learned this through the hard way, so be careful here.
 * Luckily we can at least blame others for their non-compliance but from
 * inter-operatibility perspective it seems that we're somewhat stuck with
 * the ordering which we have been using if we want to keep working with
 * those broken things (not that it currently hurts anybody as there isn't
 * particular reason why the ordering would need to be changed).
 *
 * At least SACK_PERM as the first option is known to lead to a disaster
 * (but it may well be that other scenarios fail similarly).
 */
void tcp_options_write(__be32 *ptr, struct tcp_sock *tp,
		       const struct tcp_out_options *opts,
		       __u8 **md5_hash) {
	if (unlikely(OPTION_MD5 & opts->options)) {
		*ptr++ = htonl((TCPOPT_NOP << 24) |
			       (TCPOPT_NOP << 16) |
			       (TCPOPT_MD5SIG << 8) |
			       TCPOLEN_MD5SIG);
		*md5_hash = (__u8 *)ptr;
		ptr += 4;
	} else {
		*md5_hash = NULL;
	}

	if (unlikely(opts->mss)) {
		*ptr++ = htonl((TCPOPT_MSS << 24) |
			       (TCPOLEN_MSS << 16) |
			       opts->mss);
	}

	if (likely(OPTION_TS & opts->options)) {
		if (unlikely(OPTION_SACK_ADVERTISE & opts->options)) {
			*ptr++ = htonl((TCPOPT_SACK_PERM << 24) |
				       (TCPOLEN_SACK_PERM << 16) |
				       (TCPOPT_TIMESTAMP << 8) |
				       TCPOLEN_TIMESTAMP);
		} else {
			*ptr++ = htonl((TCPOPT_NOP << 24) |
				       (TCPOPT_NOP << 16) |
				       (TCPOPT_TIMESTAMP << 8) |
				       TCPOLEN_TIMESTAMP);
		}
		*ptr++ = htonl(opts->tsval);
		*ptr++ = htonl(opts->tsecr);
	}

	if (unlikely(OPTION_SACK_ADVERTISE & opts->options &&
		     !(OPTION_TS & opts->options))) {
		*ptr++ = htonl((TCPOPT_NOP << 24) |
			       (TCPOPT_NOP << 16) |
			       (TCPOPT_SACK_PERM << 8) |
			       TCPOLEN_SACK_PERM);
	}

	if (unlikely(opts->ws)) {
		*ptr++ = htonl((TCPOPT_NOP << 24) |
			       (TCPOPT_WINDOW << 16) |
			       (TCPOLEN_WINDOW << 8) |
			       opts->ws);
	}

	if (unlikely(opts->num_sack_blocks)) {
		struct tcp_sack_block *sp = tp->rx_opt.dsack ?
			tp->duplicate_sack : tp->selective_acks;
		int this_sack;

		*ptr++ = htonl((TCPOPT_NOP  << 24) |
			       (TCPOPT_NOP  << 16) |
			       (TCPOPT_SACK <<  8) |
			       (TCPOLEN_SACK_BASE + (opts->num_sack_blocks *
						     TCPOLEN_SACK_PERBLOCK)));

		for (this_sack = 0; this_sack < opts->num_sack_blocks;
		     ++this_sack) {
			*ptr++ = htonl(sp[this_sack].start_seq);
			*ptr++ = htonl(sp[this_sack].end_seq);
		}

		if (tp->rx_opt.dsack) {
			tp->rx_opt.dsack = 0;
			tp->rx_opt.eff_sacks = tp->rx_opt.num_sacks;
		}
	}
#ifdef CONFIG_MTCP
	if (unlikely(OPTION_MPC & opts->options)) {
#ifdef CONFIG_MTCP_PM
		*ptr++ = htonl((TCPOPT_NOP  << 24) |
			       (TCPOPT_MPC << 16) |
			       (TCPOLEN_MPC << 8));
		*ptr++ = htonl(opts->token);
#else
		*ptr++ = htonl((TCPOPT_MPC << 24) |
			       (TCPOLEN_MPC << 16));
#endif
	}

#ifdef CONFIG_MTCP_PM
	if (unlikely((OPTION_ADDR & opts->options) && opts->num_addr4)) {
		uint8_t *ptr8=(uint8_t*)ptr; /*We need a per-byte pointer here*/
		int i;
		for (i=TCPOLEN_ADDR(opts->num_addr4);
		     i<TCPOLEN_ADDR_ALIGNED(opts->num_addr4);i++)
			*ptr8++ = TCPOPT_NOP;
		*ptr8++ = TCPOPT_ADDR;
		*ptr8++ = TCPOLEN_ADDR(opts->num_addr4);
		for (i=0;i<opts->num_addr4;i++) {
			*ptr8++ = opts->addr4[i].id;
			*ptr8++ = 64;
			*((__be32*)ptr8)=opts->addr4[i].addr.s_addr;
			ptr8+=sizeof(struct in_addr);
		}
		ptr = (__be32*)ptr8;
	}

	if (unlikely(OPTION_JOIN & opts->options)) {
		*ptr++ = htonl((TCPOPT_NOP << 24) |
			       (TCPOPT_JOIN << 16) |
			       (TCPOLEN_JOIN << 8) |
			       (opts->token >> 24));
		*ptr++ = htonl((opts->token<<8) |
			       opts->addr_id);
	}
#endif
	if (OPTION_DSN & opts->options) {
		*ptr++ = htonl((TCPOPT_DSN << 24) |
			       (TCPOLEN_DSN << 16) |
			       opts->data_len);
		*ptr++ = htonl(opts->sub_seq);
		*ptr++ = htonl(opts->data_seq);
	}
	if (OPTION_DATA_ACK & opts->options) {
		*ptr++ = htonl((TCPOPT_NOP << 24) |
			       (TCPOPT_NOP << 16) |
			       (TCPOPT_DATA_ACK << 8) |
			       (TCPOLEN_DATA_ACK));
		*ptr++ = htonl(opts->data_ack);
	}
	if (OPTION_DFIN & opts->options) {
		*ptr++ = htonl((TCPOPT_NOP << 24) |
			       (TCPOPT_NOP << 16) |
			       (TCPOPT_DFIN << 8) |
			       (TCPOLEN_DFIN));
	}
#endif
}

static unsigned tcp_syn_options(struct sock *sk, struct sk_buff *skb,
				struct tcp_out_options *opts,
				struct tcp_md5sig_key **md5) 
{
	struct tcp_sock *tp = tcp_sk(sk);
	unsigned size = 0;

#ifdef CONFIG_TCP_MD5SIG
	*md5 = tp->af_specific->md5_lookup(sk, sk);
	if (*md5) {
		opts->options |= OPTION_MD5;
		size += TCPOLEN_MD5SIG_ALIGNED;
	}
#else
	*md5 = NULL;
#endif

	/* We always get an MSS option.  The option bytes which will be seen in
	 * normal data packets should timestamps be used, must be in the MSS
	 * advertised.  But we subtract them from tp->mss_cache so that
	 * calculations in tcp_sendmsg are simpler etc.  So account for this
	 * fact here if necessary.  If we don't do this correctly, as a
	 * receiver we won't recognize data packets as being full sized when we
	 * should, and thus we won't abide by the delayed ACK rules correctly.
	 * SACKs don't matter, we never delay an ACK when we have any of those
	 * going out.  */
	opts->mss = tcp_advertise_mss(sk);
	size += TCPOLEN_MSS_ALIGNED;

	if (likely(sysctl_tcp_timestamps && *md5 == NULL)) {
		opts->options |= OPTION_TS;
		opts->tsval = TCP_SKB_CB(skb)->when;
		opts->tsecr = tp->rx_opt.ts_recent;
		size += TCPOLEN_TSTAMP_ALIGNED;
	}
	if (likely(sysctl_tcp_window_scaling)) {
		opts->ws = tp->rx_opt.rcv_wscale;
		if(likely(opts->ws))
			size += TCPOLEN_WSCALE_ALIGNED;
	}
	if (likely(sysctl_tcp_sack)) {
		opts->options |= OPTION_SACK_ADVERTISE;
		if (unlikely(!(OPTION_TS & opts->options)))
			size += TCPOLEN_SACKPERM_ALIGNED;
	}
#ifdef CONFIG_MTCP	
	if (is_master_sk(tp)) {
		struct multipath_pcb *mpcb=mpcb_from_tcpsock(tp);

		opts->options |= OPTION_MPC;
		size+=TCPOLEN_MPC_ALIGNED;
#ifdef CONFIG_MTCP_PM
		opts->token=tp->mtcp_loc_token;
#endif
		
		/*We arrive here either when sending a SYN or a
		  SYN+ACK when in SYN_SENT state (that is, tcp_synack_options
		  is only called for syn+ack replied by a server, while this
		  function is called when SYNs are sent by both parties and 
		  are crossed)
		  Due to this possibility, a slave subsocket may arrive here,
		  and does not need to set the dataseq options, since
		  there is no data in the segment*/
		BUG_ON(!mpcb);
	}
#ifdef CONFIG_MTCP_PM
	else {
		struct multipath_pcb *mpcb=mpcb_from_tcpsock(tp);
		opts->options |= OPTION_JOIN;
		size+=TCPOLEN_JOIN_ALIGNED;
		opts->token=tp->rx_opt.mtcp_rem_token;
		opts->addr_id=mtcp_get_loc_addrid(mpcb, tp->path_index);
	}
#endif
#endif

	return size;
}

static unsigned tcp_synack_options(struct sock *sk,
				   struct request_sock *req,
				   unsigned mss, struct sk_buff *skb,
				   struct tcp_out_options *opts,
				   struct tcp_md5sig_key **md5)
{
	unsigned size = 0;
	struct inet_request_sock *ireq = inet_rsk(req);
	char doing_ts;

#ifdef CONFIG_TCP_MD5SIG
	*md5 = tcp_rsk(req)->af_specific->md5_lookup(sk, req);
	if (*md5) {
		opts->options |= OPTION_MD5;
		size += TCPOLEN_MD5SIG_ALIGNED;
	}
#else
	*md5 = NULL;
#endif

	/* we can't fit any SACK blocks in a packet with MD5 + TS
	   options. There was discussion about disabling SACK rather than TS in
	   order to fit in better with old, buggy kernels, but that was deemed
	   to be unnecessary. */
	doing_ts = ireq->tstamp_ok && !(*md5 && ireq->sack_ok);

	opts->mss = mss;
	size += TCPOLEN_MSS_ALIGNED;

	if (likely(ireq->wscale_ok)) {
		opts->ws = ireq->rcv_wscale;
		if(likely(opts->ws))
			size += TCPOLEN_WSCALE_ALIGNED;
	}
	if (likely(doing_ts)) {
		opts->options |= OPTION_TS;
		opts->tsval = TCP_SKB_CB(skb)->when;
		opts->tsecr = req->ts_recent;
		size += TCPOLEN_TSTAMP_ALIGNED;
	}
	if (likely(ireq->sack_ok)) {
		opts->options |= OPTION_SACK_ADVERTISE;
		if (unlikely(!doing_ts))
			size += TCPOLEN_SACKPERM_ALIGNED;
	}


#ifdef CONFIG_MTCP
/*For the SYNACK, the mpcb is normally not yet initialized
  (to protect against SYN DoS attack)
  So we cannot use it here.*/
	
	opts->options |= OPTION_MPC;
	size+=TCPOLEN_MPC_ALIGNED;
#ifdef CONFIG_MTCP_PM
	opts->token = req->mtcp_loc_token;
#endif
	opts->options |= OPTION_DSN;
	size+=TCPOLEN_DSN_ALIGNED;
	opts->data_seq=0;
#endif
	return size;
}


/*if skb is NULL, then we are evaluating the MSS, thus, we take into account
 * ALL potential options. */
static unsigned tcp_established_options(struct sock *sk, struct sk_buff *skb,
					struct tcp_out_options *opts,
					struct tcp_md5sig_key **md5) {
	struct tcp_skb_cb *tcb = skb ? TCP_SKB_CB(skb) : NULL;
	struct tcp_sock *tp = tcp_sk(sk);
	unsigned size = 0;
#ifdef CONFIG_MTCP
	struct multipath_pcb *mpcb;
	int release_mpcb=0;
#endif

#ifdef CONFIG_TCP_MD5SIG
	*md5 = tp->af_specific->md5_lookup(sk, sk);
	if (unlikely(*md5)) {
		opts->options |= OPTION_MD5;
		size += TCPOLEN_MD5SIG_ALIGNED;
	}
#else
	*md5 = NULL;
#endif

	if (likely(tp->rx_opt.tstamp_ok)) {
		opts->options |= OPTION_TS;
		opts->tsval = tcb ? tcb->when : 0;
		opts->tsecr = tp->rx_opt.ts_recent;
		size += TCPOLEN_TSTAMP_ALIGNED;
	}

#ifdef CONFIG_MTCP
	mpcb = tp->mpcb;
	if (tp->pending && !is_master_sk(tp) && tp->mpc) {
		mpcb=mtcp_hash_find(tp->mtcp_loc_token);
		if (!mpcb) {
			printk(KERN_ERR "mpcb not found, token %#x,"
			       "master_sk:%d,pending:%d," NIPQUAD_FMT 
			       "->" NIPQUAD_FMT "\n",
			       tp->mtcp_loc_token,is_master_sk(tp), 
			       tp->pending, NIPQUAD(inet_sk(sk)->saddr),
			       NIPQUAD(inet_sk(sk)->daddr));
			BUG();
		}
		else release_mpcb=1;
	}

	if (tp->mpc && (!skb || skb->len!=0 ||  
			(tcb->flags & TCPCB_FLAG_FIN))) {
		if (tcb && tcb->data_len) { /*Ignore dataseq if data_len is 0*/
			opts->data_seq=tcb->data_seq;
			opts->data_len=tcb->data_len;
			opts->sub_seq=tcb->sub_seq-tp->snt_isn;
		}
		opts->options |= OPTION_DSN;
		size += TCPOLEN_DSN_ALIGNED;		
	}
	/*we can have mpc==1 and mpcb==NULL if tp is the master_sk
	  and is established but not yet accepted.*/
	if (tp->mpc && mpcb && test_bit(MPCB_FLAG_FIN_ENQUEUED,
				&mpcb->flags) &&
	    (!skb || TCP_SKB_CB(skb)->end_data_seq==mpcb->tp.write_seq)) {
		opts->options |= OPTION_DFIN;
		size += TCPOLEN_DFIN_ALIGNED;		
	}
	if (tp->mpc) {
		/*If we are at the server side, and the accept syscall has not
		  yet been called, the received data is still enqueued in the 
		  subsock receive queue, but we must still
		  send a data ack. The value of the ack is based on the 
		  subflow ack since at this step there is necessarily only 
		  one subflow.*/
		u32 rcv_nxt=(tp->pending && is_master_sk(tp))?
			tp->rcv_nxt-tp->rcv_isn-1:
			mpcb->tp.rcv_nxt;		
		opts->data_ack=rcv_nxt;
		opts->options |= OPTION_DATA_ACK;
		size += TCPOLEN_DATA_ACK_ALIGNED;
	}
#ifdef CONFIG_MTCP_PM
	if (tp->mpc && mpcb) {
		if (unlikely(mpcb->addr_unsent)) {
			const unsigned remaining = MAX_TCP_OPTION_SPACE - size;
			if (remaining<TCPOLEN_ADDR_BASE)
				opts->num_addr4=0;
			else
				opts->num_addr4=min_t(unsigned, 
						      mpcb->addr_unsent,
						      (remaining-
						       TCPOLEN_ADDR_BASE) /
						      TCPOLEN_ADDR_PERBLOCK);
			/*If no space to send the option, just wait next
			  segment*/
			if (opts->num_addr4) {
				opts->options |= OPTION_ADDR;
				opts->addr4=mpcb->addr4+mpcb->num_addr4-
					mpcb->addr_unsent;
				if (skb) mpcb->addr_unsent-=opts->num_addr4;
				size += TCPOLEN_ADDR_ALIGNED(opts->num_addr4);
			}
		}
	}
	BUG_ON(!mpcb && !tp->pending);
#endif
	if (release_mpcb)
		mpcb_put(mpcb);
#endif

	if (unlikely(tp->rx_opt.eff_sacks)) {
		const unsigned remaining = MAX_TCP_OPTION_SPACE - size;
		if (remaining<TCPOLEN_SACK_BASE_ALIGNED)
			opts->num_sack_blocks=0;
		else
			opts->num_sack_blocks =
				min_t(unsigned, tp->rx_opt.eff_sacks,
				      (remaining - TCPOLEN_SACK_BASE_ALIGNED) /
				      TCPOLEN_SACK_PERBLOCK);
		if (opts->num_sack_blocks)
			size += TCPOLEN_SACK_BASE_ALIGNED +
				opts->num_sack_blocks * TCPOLEN_SACK_PERBLOCK;
	}

	if (size>MAX_TCP_OPTION_SPACE) {
		printk(KERN_ERR "exceeded option space, options:%#x\n",
		       opts->options);
		BUG();
	}
	return size;
}

/* This routine actually transmits TCP packets queued in by
 * tcp_do_sendmsg().  This is used by both the initial
 * transmission and possible later retransmissions.
 * All SKB's seen here are completely headerless.  It is our
 * job to build the TCP header, and pass the packet down to
 * IP so it can do the same plus pass the packet off to the
 * device.
 *
 * We are working here with either a clone of the original
 * SKB, or a fresh unique copy made by the retransmit engine.
 */
static int tcp_transmit_skb(struct sock *sk, struct sk_buff *skb, int clone_it,
			    gfp_t gfp_mask)
{
	const struct inet_connection_sock *icsk = inet_csk(sk);
	struct inet_sock *inet;
	struct tcp_sock *tp;
	struct tcp_skb_cb *tcb;
	struct tcp_out_options opts;
	unsigned tcp_options_size, tcp_header_size;
	struct tcp_md5sig_key *md5;
	__u8 *md5_hash_location;
	struct tcphdr *th;
	int err;

	BUG_ON(is_meta_sk(sk));
	check_pkts_out(sk);

	if(!skb || !tcp_skb_pcount(skb)) {
		printk(KERN_ERR "tcp_skb_pcount:%d,skb->len:%d\n",
		       tcp_skb_pcount(skb),skb->len);
		BUG();
	}

	tcpprobe_transmit_skb(sk,skb,clone_it,gfp_mask);

	/* If congestion control is doing timestamping, we must
	 * take such a timestamp before we potentially clone/copy.
	 */
	if (icsk->icsk_ca_ops->flags & TCP_CONG_RTT_STAMP)
		__net_timestamp(skb);

	if (likely(clone_it)) {
		if (unlikely(skb_cloned(skb)))
			skb = pskb_copy(skb, gfp_mask);
		else
			skb = skb_clone(skb, gfp_mask);
		if (unlikely(!skb)) {
			printk(KERN_ERR "transmit_skb, clone failed\n");
			return -ENOBUFS;
		}
	}

	inet = inet_sk(sk);
	tp = tcp_sk(sk);
	tcb = TCP_SKB_CB(skb);
	memset(&opts, 0, sizeof(opts));

	if (tp->mpc)
		skb->count_dsn=1;
	
	if (unlikely(tcb->flags & TCPCB_FLAG_SYN))
		tcp_options_size = tcp_syn_options(sk, skb, &opts, &md5);
	else
		tcp_options_size = tcp_established_options(sk, skb, &opts,
							   &md5);
	tcp_header_size = tcp_options_size + sizeof(struct tcphdr);

	if (tcp_packets_in_flight(tp) == 0)
		tcp_ca_event(sk, CA_EVENT_TX_START);

	skb_push(skb, tcp_header_size);
	skb_reset_transport_header(skb);
	skb_set_owner_w(skb, sk);

	/* Build TCP header and checksum it. */
	th = tcp_hdr(skb);
	th->source		= inet->sport;
	th->dest		= inet->dport;
	th->seq			= htonl(tcb->seq);
	th->ack_seq		= htonl(tp->rcv_nxt);
	*(((__be16 *)th) + 6)	= htons(((tcp_header_size >> 2) << 12) |
					tcb->flags);

	if (unlikely(tcb->flags & TCPCB_FLAG_SYN)) {
		/* RFC1323: The window in SYN & SYN/ACK segments
		 * is never scaled.
		 */
		th->window	= htons(min(tp->rcv_wnd, 65535U));
	} else
		th->window	= htons(tcp_select_window(sk));
	th->check		= 0;
	th->urg_ptr		= 0;

	/* The urg_mode check is necessary during a below snd_una win probe */
	if (unlikely(tcp_urg_mode(tp) &&
		     between(tp->snd_up, tcb->seq + 1, tcb->seq + 0xFFFF))) {
		th->urg_ptr		= htons(tp->snd_up - tcb->seq);
		th->urg			= 1;
	}
	
	tcp_options_write((__be32 *)(th + 1), tp, &opts, &md5_hash_location);
	if (likely((tcb->flags & TCPCB_FLAG_SYN) == 0))
		TCP_ECN_send(sk, skb, tcp_header_size);

#ifdef CONFIG_TCP_MD5SIG
	/* Calculate the MD5 hash, as we have all we need now */
	if (md5) {
		sk->sk_route_caps &= ~NETIF_F_GSO_MASK;
		tp->af_specific->calc_md5_hash(md5_hash_location,
					       md5, sk, NULL, skb);
	}
#endif

	icsk->icsk_af_ops->send_check(sk, skb->len, skb);

	if (likely(tcb->flags & TCPCB_FLAG_ACK))
		tcp_event_ack_sent(sk, tcp_skb_pcount(skb));

	if (skb->len != tcp_header_size)
		tcp_event_data_sent(tp, skb, sk);

	if (after(tcb->end_seq, tp->snd_nxt) || tcb->seq == tcb->end_seq)
		TCP_INC_STATS(sock_net(sk), TCP_MIB_OUTSEGS);

	skb->path_index=tp->path_index;
	
	err = icsk->icsk_af_ops->queue_xmit(skb, 0);

	check_pkts_out(sk);

	if (likely(err <= 0)) {
		if (err<0) 
			mtcp_debug("%s:error %d\n",__FUNCTION__,err);
		return err;
	}

	tcp_enter_cwr(sk, 1);

	return net_xmit_eval(err);
}

/* This routine just queue's the buffer
 *
 * NOTE: probe0 timer is not checked, do not forget tcp_push_pending_frames,
 * otherwise socket can stall.
 */
void tcp_queue_skb(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);

	/* Advance write_seq and place onto the write_queue. */
	if (is_meta_sk(sk))
		tp->write_seq = TCP_SKB_CB(skb)->end_data_seq;
	else
		tp->write_seq = TCP_SKB_CB(skb)->end_seq;
	skb_header_release(skb);
	tcp_add_write_queue_tail(sk, skb);
	sk->sk_wmem_queued += skb->truesize;
	sk_mem_charge(sk, skb->truesize);
}

static void tcp_set_skb_tso_segs(struct sock *sk, struct sk_buff *skb,
				 unsigned int mss_now)
{
	if (skb->len <= mss_now || !sk_can_gso(sk)) {
		/* Avoid the costly divide in the normal
		 * non-TSO case.
		 */
		skb_shinfo(skb)->gso_segs = 1;
		skb_shinfo(skb)->gso_size = 0;
		skb_shinfo(skb)->gso_type = 0;
	} else {
		skb_shinfo(skb)->gso_segs = DIV_ROUND_UP(skb->len, mss_now);
		skb_shinfo(skb)->gso_size = mss_now;
		skb_shinfo(skb)->gso_type = sk->sk_gso_type;
	}
}

/* When a modification to fackets out becomes necessary, we need to check
 * skb is counted to fackets_out or not.
 */
static void tcp_adjust_fackets_out(struct sock *sk, struct sk_buff *skb,
				   int decr)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (!tp->sacked_out || tcp_is_reno(tp))
		return;

	if (after(tcp_highest_sack_seq(tp), TCP_SKB_CB(skb)->seq))
		tp->fackets_out -= decr;
}

/* Function to create two new TCP segments.  Shrinks the given segment
 * to the specified size and appends a new segment with the rest of the
 * packet to the list.  This won't be called frequently, I hope.
 * Remember, these are still headerless SKBs at this point.
 */
int tcp_fragment(struct sock *sk, struct sk_buff *skb, u32 len,
		 unsigned int mss_now)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *buff;
	int nsize, old_factor;
	int nlen;
	u16 flags;

	BUG_ON(len > skb->len);

	tcp_clear_retrans_hints_partial(tp);
	nsize = skb_headlen(skb) - len;
	if (nsize < 0)
		nsize = 0;

	if (skb_cloned(skb) &&
	    skb_is_nonlinear(skb) &&
	    pskb_expand_head(skb, 0, 0, GFP_ATOMIC))
		return -ENOMEM;

	/* Get a new skb... force flag on. */
	buff = sk_stream_alloc_skb(sk, nsize, GFP_ATOMIC);
	if (buff == NULL)
		return -ENOMEM; /* We'll just try again later. */

	sk->sk_wmem_queued += buff->truesize;
	sk_mem_charge(sk, buff->truesize);
	nlen = skb->len - len - nsize;
	buff->truesize += nlen;
	skb->truesize -= nlen;

	/* Correct the sequence numbers. */
	TCP_SKB_CB(buff)->seq = TCP_SKB_CB(skb)->seq + len;
	TCP_SKB_CB(buff)->end_seq = TCP_SKB_CB(skb)->end_seq;
	TCP_SKB_CB(skb)->end_seq = TCP_SKB_CB(buff)->seq;
#ifdef CONFIG_MTCP
	TCP_SKB_CB(buff)->data_seq=TCP_SKB_CB(skb)->data_seq + len;
	TCP_SKB_CB(buff)->end_data_seq = TCP_SKB_CB(skb)->end_data_seq;
	TCP_SKB_CB(buff)->sub_seq = TCP_SKB_CB(skb)->sub_seq + len;
	TCP_SKB_CB(buff)->data_len=TCP_SKB_CB(skb)->data_len - len;
	TCP_SKB_CB(skb)->data_len=len;
	TCP_SKB_CB(skb)->end_data_seq = TCP_SKB_CB(buff)->data_seq;
#endif

	/* PSH and FIN should only be set in the second packet. */
	flags = TCP_SKB_CB(skb)->flags;
	TCP_SKB_CB(skb)->flags = flags & ~(TCPCB_FLAG_FIN | TCPCB_FLAG_PSH);
	TCP_SKB_CB(buff)->flags = flags;
	TCP_SKB_CB(buff)->sacked = TCP_SKB_CB(skb)->sacked;

	if (!skb_shinfo(skb)->nr_frags && skb->ip_summed != CHECKSUM_PARTIAL) {
		/* Copy and checksum data tail into the new buffer. */
		buff->csum = csum_partial_copy_nocheck(skb->data + len,
						       skb_put(buff, nsize),
						       nsize, 0);

		skb_trim(skb, len);

		skb->csum = csum_block_sub(skb->csum, buff->csum, len);
	} else {
		skb->ip_summed = CHECKSUM_PARTIAL;
		skb_split(skb, buff, len);
	}

	buff->ip_summed = skb->ip_summed;

	/* Looks stupid, but our code really uses when of
	 * skbs, which it never sent before. --ANK
	 */
	TCP_SKB_CB(buff)->when = TCP_SKB_CB(skb)->when;
	buff->tstamp = skb->tstamp;

	old_factor = tcp_skb_pcount(skb);

	/* Fix up tso_factor for both original and new SKB.  */
	tcp_set_skb_tso_segs(sk, skb, mss_now);
	tcp_set_skb_tso_segs(sk, buff, mss_now);

	/* If this packet has been sent out already, we must
	 * adjust the various packet counters.
	 */
	if (!before(tp->snd_nxt, TCP_SKB_CB(buff)->end_seq)) {
		int diff = old_factor - tcp_skb_pcount(skb) -
			tcp_skb_pcount(buff);

		tp->packets_out -= diff;

		if (TCP_SKB_CB(skb)->sacked & TCPCB_SACKED_ACKED)
			tp->sacked_out -= diff;
		if (TCP_SKB_CB(skb)->sacked & TCPCB_SACKED_RETRANS)
			tp->retrans_out -= diff;

		if (TCP_SKB_CB(skb)->sacked & TCPCB_LOST)
			tp->lost_out -= diff;

		/* Adjust Reno SACK estimate. */
		if (tcp_is_reno(tp) && diff > 0) {
			tcp_dec_pcount_approx_int(&tp->sacked_out, diff);
			tcp_verify_left_out(tp);
		}
		tcp_adjust_fackets_out(sk, skb, diff);
	}

	/* Link BUFF into the send queue. */
	skb_header_release(buff);
	tcp_insert_write_queue_after(skb, buff, sk);

	return 0;
}

/* This is similar to __pskb_pull_head() (it will go to core/skbuff.c
 * eventually). The difference is that pulled data not copied, but
 * immediately discarded.
 */
static void __pskb_trim_head(struct sk_buff *skb, int len)
{
	int i, k, eat;

	eat = len;
	k = 0;
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		if (skb_shinfo(skb)->frags[i].size <= eat) {
			put_page(skb_shinfo(skb)->frags[i].page);
			eat -= skb_shinfo(skb)->frags[i].size;
		} else {
			skb_shinfo(skb)->frags[k] = skb_shinfo(skb)->frags[i];
			if (eat) {
				skb_shinfo(skb)->frags[k].page_offset += eat;
				skb_shinfo(skb)->frags[k].size -= eat;
				eat = 0;
			}
			k++;
		}
	}
	skb_shinfo(skb)->nr_frags = k;

	skb_reset_tail_pointer(skb);
	skb->data_len -= len;
	skb->len = skb->data_len;
}

int tcp_trim_head(struct sock *sk, struct sk_buff *skb, u32 len)
{
	if (skb_cloned(skb) && pskb_expand_head(skb, 0, 0, GFP_ATOMIC))
		return -ENOMEM;

	/* If len == headlen, we avoid __skb_pull to preserve alignment. */
	if (unlikely(len < skb_headlen(skb)))
		__skb_pull(skb, len);
	else
		__pskb_trim_head(skb, len - skb_headlen(skb));

	TCP_SKB_CB(skb)->seq += len;
#ifdef CONFIG_MTCP
	TCP_SKB_CB(skb)->data_seq += len;
	TCP_SKB_CB(skb)->sub_seq += len;
	TCP_SKB_CB(skb)->data_len -= len;
#endif

	skb->ip_summed = CHECKSUM_PARTIAL;

	skb->truesize	     -= len;
	sk->sk_wmem_queued   -= len;
	sk_mem_uncharge(sk, len);
	sock_set_flag(sk, SOCK_QUEUE_SHRUNK);

	/* Any change of skb->len requires recalculation of tso
	 * factor and mss.
	 */
	if (tcp_skb_pcount(skb) > 1)
		tcp_set_skb_tso_segs(sk, skb, tcp_current_mss(sk, 1));

	return 0;
}

/* Not accounting for SACKs here. */
int tcp_mtu_to_mss(struct sock *sk, int pmtu)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);
	int mss_now;

	/* Calculate base mss without TCP options:
	   It is MMS_S - sizeof(tcphdr) of rfc1122
	 */
	mss_now = pmtu - icsk->icsk_af_ops->net_header_len - sizeof(struct tcphdr);

	/* Clamp it (mss_clamp does not include tcp options) */
	if (mss_now > tp->rx_opt.mss_clamp)
		mss_now = tp->rx_opt.mss_clamp;

	/* Now subtract optional transport overhead */
	mss_now -= icsk->icsk_ext_hdr_len;

	/* Then reserve room for full set of TCP options and 8 bytes of data */
	if (mss_now < 48)
		mss_now = 48;

	/* Now subtract TCP options size, not including SACKs */
	mss_now -= tp->tcp_header_len - sizeof(struct tcphdr);

	return mss_now;
}

/* Inverse of above */
int tcp_mss_to_mtu(struct sock *sk, int mss)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);
	int mtu;

	mtu = mss +
	      tp->tcp_header_len +
	      icsk->icsk_ext_hdr_len +
	      icsk->icsk_af_ops->net_header_len;

	return mtu;
}

void tcp_mtup_init(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);

	icsk->icsk_mtup.enabled = sysctl_tcp_mtu_probing > 1;
	icsk->icsk_mtup.search_high = tp->rx_opt.mss_clamp + sizeof(struct tcphdr) +
			       icsk->icsk_af_ops->net_header_len;
	icsk->icsk_mtup.search_low = tcp_mss_to_mtu(sk, sysctl_tcp_base_mss);
	icsk->icsk_mtup.probe_size = 0;
}

/* Bound MSS / TSO packet size with the half of the window */
static int tcp_bound_to_half_wnd(struct tcp_sock *tp, int pktsize)
{
	if (tp->max_window && pktsize > (tp->max_window >> 1))
		return max(tp->max_window >> 1, 68U - tp->tcp_header_len);
	else
		return pktsize;
}

/* This function synchronize snd mss to current pmtu/exthdr set.

   tp->rx_opt.user_mss is mss set by user by TCP_MAXSEG. It does NOT counts
   for TCP options, but includes only bare TCP header.

   tp->rx_opt.mss_clamp is mss negotiated at connection setup.
   It is minimum of user_mss and mss received with SYN.
   It also does not include TCP options.

   inet_csk(sk)->icsk_pmtu_cookie is last pmtu, seen by this function.

   tp->mss_cache is current effective sending mss, including
   all tcp options except for SACKs. It is evaluated,
   taking into account current pmtu, but never exceeds
   tp->rx_opt.mss_clamp.

   NOTE1. rfc1122 clearly states that advertised MSS
   DOES NOT include either tcp or ip options.

   NOTE2. inet_csk(sk)->icsk_pmtu_cookie and tp->mss_cache
   are READ ONLY outside this function.		--ANK (980731)
 */
unsigned int tcp_sync_mss(struct sock *sk, u32 pmtu)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);
	int mss_now;

	if (icsk->icsk_mtup.search_high > pmtu)
		icsk->icsk_mtup.search_high = pmtu;

	mss_now = tcp_mtu_to_mss(sk, pmtu);
	mss_now = tcp_bound_to_half_wnd(tp, mss_now);

	/* And store cached results */
	icsk->icsk_pmtu_cookie = pmtu;
	if (icsk->icsk_mtup.enabled)
		mss_now = min(mss_now, tcp_mtu_to_mss(sk, icsk->icsk_mtup.search_low));
	tp->mss_cache = mss_now;

	return mss_now;
}

/* Compute the current effective MSS, taking SACKs and IP options,
 * and even PMTU discovery events into account.
 */
unsigned int tcp_current_mss(struct sock *sk, int large_allowed)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct dst_entry *dst = __sk_dst_get(sk);
	u32 mss_now;
	u16 xmit_size_goal;
	int doing_tso = 0;
	unsigned header_len;
	struct tcp_out_options opts;
	struct tcp_md5sig_key *md5;

	/*if sk is the meta-socket, return the common MSS*/
	if (is_meta_tp(tp)) return sysctl_mptcp_mss;

	mss_now = tp->mss_cache;

	if (large_allowed && sk_can_gso(sk))
		doing_tso = 1;

	if (dst) {
		u32 mtu = dst_mtu(dst);
		if (mtu != inet_csk(sk)->icsk_pmtu_cookie)
			mss_now = tcp_sync_mss(sk, mtu);
	}
	memset(&opts, 0, sizeof(opts));
	header_len = tcp_established_options(sk, NULL, &opts, &md5) +
		     sizeof(struct tcphdr);
	/* The mss_cache is sized based on tp->tcp_header_len, which assumes
	 * some common options. If this is an odd packet (because we have SACK
	 * blocks etc) then our calculated header_len will be different, and
	 * we have to adjust mss_now correspondingly */
	if (header_len != tp->tcp_header_len) {
		int delta = (int) header_len - tp->tcp_header_len;
		mss_now -= delta;
	}

	xmit_size_goal = mss_now;

	if (doing_tso) {
		xmit_size_goal = ((sk->sk_gso_max_size - 1) -
				  inet_csk(sk)->icsk_af_ops->net_header_len -
				  inet_csk(sk)->icsk_ext_hdr_len -
				  tp->tcp_header_len);

		xmit_size_goal = tcp_bound_to_half_wnd(tp, xmit_size_goal);
		xmit_size_goal -= (xmit_size_goal % mss_now);
	}
	tp->xmit_size_goal = xmit_size_goal;

	return mss_now;
}

/* Congestion window validation. (RFC2861) */
static void tcp_cwnd_validate(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (tp->packets_out >= tp->snd_cwnd) {
		/* Network is fed fully. */
		tp->snd_cwnd_used = 0;
		tp->snd_cwnd_stamp = tcp_time_stamp;
	} else {
		/* Network starves. */
		if (tp->packets_out > tp->snd_cwnd_used)
			tp->snd_cwnd_used = tp->packets_out;

		if (sysctl_tcp_slow_start_after_idle &&
		    (s32)(tcp_time_stamp - tp->snd_cwnd_stamp) >= inet_csk(sk)->icsk_rto)
			tcp_cwnd_application_limited(sk);
	}
}

/* Can at least one segment of SKB be sent right now, according to the
 * congestion window rules?  If so, return how many segments are allowed.
 */
static inline unsigned int tcp_cwnd_test(struct tcp_sock *tp,
				  struct sk_buff *skb)
{
	u32 in_flight, cwnd;
	struct sock *sk=(struct sock*)tp;
	struct inet_connection_sock *icsk=inet_csk(sk);

	BUG_ON(is_meta_tp(tp));

	/* Don't be strict about the congestion window for the final FIN.  */
	if ((TCP_SKB_CB(skb)->flags & TCPCB_FLAG_FIN) &&
	    tcp_skb_pcount(skb) == 1)
		return 1;

	in_flight = tcp_packets_in_flight(tp);
	if (icsk->icsk_ca_state==TCP_CA_Loss)
		tcpprobe_logmsg(sk,"tp %d: in_flight is %d",tp->path_index,
				in_flight);
	cwnd = tp->snd_cwnd;
	if (in_flight < cwnd)
		return (cwnd - in_flight);
	
	return 0;
}

/* This must be invoked the first time we consider transmitting
 * SKB onto the wire.
 */
static int tcp_init_tso_segs(struct sock *sk, struct sk_buff *skb,
			     unsigned int mss_now)
{
	int tso_segs = tcp_skb_pcount(skb);

	if (!tso_segs || (tso_segs > 1 && tcp_skb_mss(skb) != mss_now)) {
		tcp_set_skb_tso_segs(sk, skb, mss_now);
		tso_segs = tcp_skb_pcount(skb);
	}
	return tso_segs;
}

static inline int tcp_minshall_check(const struct tcp_sock *tp)
{
	return after(tp->snd_sml,tp->snd_una) &&
		!after(tp->snd_sml, tp->snd_nxt);
}

/* Return 0, if packet can be sent now without violation Nagle's rules:
 * 1. It is full sized.
 * 2. Or it contains FIN. (already checked by caller)
 * 3. Or TCP_NODELAY was set.
 * 4. Or TCP_CORK is not set, and all sent packets are ACKed.
 *    With Minshall's modification: all sent small packets are ACKed.
 */
static inline int tcp_nagle_check(const struct tcp_sock *tp,
				  const struct sk_buff *skb,
				  unsigned mss_now, int nonagle)
{
	return (skb->len < mss_now &&
		((nonagle & TCP_NAGLE_CORK) ||
		 (!nonagle && tp->packets_out && tcp_minshall_check(tp))));
}

/* Return non-zero if the Nagle test allows this packet to be
 * sent now.
 */
static inline int tcp_nagle_test(struct tcp_sock *tp, struct sk_buff *skb,
				 unsigned int cur_mss, int nonagle)
{
	/* Nagle rule does not apply to frames, which sit in the middle of the
	 * write_queue (they have no chances to get new data).
	 *
	 * This is implemented in the callers, where they modify the 'nonagle'
	 * argument based upon the location of SKB in the send queue.
	 */
	if (nonagle & TCP_NAGLE_PUSH)
		return 1;

	/* Don't use the nagle rule for urgent data (or for the final FIN).
	 * Nagle can be ignored during F-RTO too (see RFC4138).
	 */
	if (tcp_urg_mode(tp) || (tp->frto_counter == 2) ||
	    (TCP_SKB_CB(skb)->flags & TCPCB_FLAG_FIN))
		return 1;

	if (!tcp_nagle_check(tp, skb, cur_mss, nonagle))
		return 1;

	return 0;
}

/* Does at least the first segment of SKB fit into the send window? */
static inline int tcp_snd_wnd_test(struct tcp_sock *tp, struct sk_buff *skb,
				   unsigned int cur_mss)
{
	u32 end_seq = (tp->mpc)?TCP_SKB_CB(skb)->end_data_seq:
		TCP_SKB_CB(skb)->end_seq;
	
	if (skb->len > cur_mss)
		end_seq = ((tp->mpc)?TCP_SKB_CB(skb)->data_seq:
			   TCP_SKB_CB(skb)->seq) + cur_mss;
	if (after(end_seq, tcp_wnd_end(tp,tp->mpc)) && 
	    (TCP_SKB_CB(skb)->flags & TCPCB_FLAG_FIN)) {
		mtcp_debug("FIN refused for sndwnd, fin end dsn %#x,"
			   "tcp_wnd_end: %#x, mpc:%d,mpcb:%p,snd_una:%#x,"
			   "snd_wnd:%d, mpcb write_seq:%#x, "
			   "mpcb queue len:%d\n",
			   end_seq,tcp_wnd_end(tp,tp->mpc),
			   tp->mpc,tp->mpcb,tp->mpcb->tp.snd_una,
			   tp->mpcb->tp.snd_wnd,tp->mpcb->tp.write_seq,
			   ((struct sock*)&tp->mpcb->tp)->sk_write_queue.qlen);
	}	
	
	return !after(end_seq, tcp_wnd_end(tp,tp->mpc));
}

/* This checks if the data bearing packet SKB (usually tcp_send_head(sk))
 * should be put on the wire right now.  If so, it returns the number of
 * packets allowed by the congestion window.
 */
static unsigned int tcp_snd_test(struct sock *subsk, struct sk_buff *skb,
				 unsigned int cur_mss, int nonagle)
{
	struct tcp_sock *subtp = tcp_sk(subsk);
	unsigned int cwnd_quota;
	struct multipath_pcb *mpcb=subtp->mpcb;
	struct tcp_sock *mpcb_tp=&mpcb->tp;
	
	BUG_ON(tcp_skb_pcount(skb)>1);
	if (!mpcb)
		mpcb_tp=subtp;
	
	if (!tcp_nagle_test(mpcb_tp, skb, cur_mss, nonagle))
		return 0;

	cwnd_quota = tcp_cwnd_test(subtp, skb);

	if (cwnd_quota && !tcp_snd_wnd_test(subtp, skb, cur_mss))
		cwnd_quota = 0;

	return cwnd_quota;
}

int tcp_may_send_now(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb = tcp_send_head(sk);
	int mss;

	if (tp->mpc)
		mss=sysctl_mptcp_mss;
	else
		mss=tcp_current_mss(sk, 1);

	return (skb &&
		tcp_snd_test(sk, skb, mss,
			     (tcp_skb_is_last(sk, skb) ?
			      tp->nonagle : TCP_NAGLE_PUSH)));
}

/* Trim TSO SKB to LEN bytes, put the remaining data into a new packet
 * which is put after SKB on the list.  It is very much like
 * tcp_fragment() except that it may make several kinds of assumptions
 * in order to speed up the splitting operation.  In particular, we
 * know that all the data is in scatter-gather pages, and that the
 * packet has never been sent out before (and thus is not cloned).
 */
static int tso_fragment(struct sock *sk, struct sk_buff *skb, unsigned int len,
			unsigned int mss_now)
{
	struct sk_buff *buff;
	int nlen = skb->len - len;
	u16 flags;

	mtcp_debug("Entering %s\n",__FUNCTION__);

	BUG_ON(len==0); /*This would create an empty segment*/

	/* All of a TSO frame must be composed of paged data.  */
	if (skb->len != skb->data_len)
		return tcp_fragment(sk, skb, len, mss_now);

	buff = sk_stream_alloc_skb(sk, 0, GFP_ATOMIC);
	if (unlikely(buff == NULL))
		return -ENOMEM;

	sk->sk_wmem_queued += buff->truesize;
	sk_mem_charge(sk, buff->truesize);
	buff->truesize += nlen;
	skb->truesize -= nlen;

	/* Correct the sequence numbers. */
	TCP_SKB_CB(buff)->seq = TCP_SKB_CB(skb)->seq + len;
	TCP_SKB_CB(buff)->end_seq = TCP_SKB_CB(skb)->end_seq;
	TCP_SKB_CB(skb)->end_seq = TCP_SKB_CB(buff)->seq;
#ifdef CONFIG_MTCP
	TCP_SKB_CB(buff)->data_seq=TCP_SKB_CB(skb)->data_seq + len;
	TCP_SKB_CB(buff)->end_data_seq = TCP_SKB_CB(skb)->end_data_seq;
	TCP_SKB_CB(buff)->sub_seq = TCP_SKB_CB(skb)->sub_seq + len;
	TCP_SKB_CB(buff)->data_len=TCP_SKB_CB(skb)->data_len - len;
	TCP_SKB_CB(skb)->data_len=len;
	TCP_SKB_CB(skb)->end_data_seq = TCP_SKB_CB(buff)->data_seq;
#endif

	/* PSH and FIN should only be set in the second packet. */
	flags = TCP_SKB_CB(skb)->flags;
	TCP_SKB_CB(skb)->flags = flags & ~(TCPCB_FLAG_FIN | TCPCB_FLAG_PSH);
	TCP_SKB_CB(buff)->flags = flags;

	/* This packet was never sent out yet, so no SACK bits. */
	TCP_SKB_CB(buff)->sacked = 0;

	buff->ip_summed = skb->ip_summed = CHECKSUM_PARTIAL;
	skb_split(skb, buff, len);

	/* Fix up tso_factor for both original and new SKB.  */
	tcp_set_skb_tso_segs(sk, skb, mss_now);
	tcp_set_skb_tso_segs(sk, buff, mss_now);

	/* Link BUFF into the send queue. */
	skb_header_release(buff);
	tcp_insert_write_queue_after(skb, buff, sk);

	return 0;
}

/* Create a new MTU probe if we are ready.
 * Returns 0 if we should wait to probe (no cwnd available),
 *         1 if a probe was sent,
 *         -1 otherwise
 */
static int tcp_mtu_probe(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct sk_buff *skb, *nskb, *next;
	int len;
	int probe_size;
	int size_needed;
	int copy;
	int mss_now;
	u32 snd_wnd=(tp->mpc)?tp->mpcb->tp.snd_wnd:tp->snd_wnd;

	/* Not currently probing/verifying,
	 * not in recovery,
	 * have enough cwnd, and
	 * not SACKing (the variable headers throw things off) */
	if (!icsk->icsk_mtup.enabled ||
	    icsk->icsk_mtup.probe_size ||
	    inet_csk(sk)->icsk_ca_state != TCP_CA_Open ||
	    tp->snd_cwnd < 11 ||
	    tp->rx_opt.eff_sacks)
		return -1;

	/* Very simple search strategy: just double the MSS. */
	mss_now = tcp_current_mss(sk, 0);
	probe_size = 2 * tp->mss_cache;
	size_needed = probe_size + (tp->reordering + 1) * tp->mss_cache;
	if (probe_size > tcp_mtu_to_mss(sk, icsk->icsk_mtup.search_high)) {
		/* TODO: set timer for probe_converge_event */
		return -1;
	}

	/* Have enough data in the send queue to probe? */
	if (tp->write_seq - tp->snd_nxt < size_needed)
		return -1;

	if (snd_wnd < size_needed)
		return -1;
	if (after(tp->snd_nxt + size_needed, tcp_wnd_end(tp,0)))
		return 0;

	/* Do we need to wait to drain cwnd? With none in flight, don't stall */
	if (tcp_packets_in_flight(tp) + 2 > tp->snd_cwnd) {
		if (!tcp_packets_in_flight(tp))
			return -1;
		else
			return 0;
	}

	/* We're allowed to probe.  Build it now. */
	if ((nskb = sk_stream_alloc_skb(sk, probe_size, GFP_ATOMIC)) == NULL)
		return -1;
	sk->sk_wmem_queued += nskb->truesize;
	sk_mem_charge(sk, nskb->truesize);

	skb = tcp_send_head(sk);

	TCP_SKB_CB(nskb)->seq = TCP_SKB_CB(skb)->seq;
	TCP_SKB_CB(nskb)->end_seq = TCP_SKB_CB(skb)->seq + probe_size;
	TCP_SKB_CB(nskb)->flags = TCPCB_FLAG_ACK;
	TCP_SKB_CB(nskb)->sacked = 0;
	nskb->csum = 0;
	nskb->ip_summed = skb->ip_summed;

	tcp_insert_write_queue_before(nskb, skb, sk);

	len = 0;
	tcp_for_write_queue_from_safe(skb, next, sk) {
		copy = min_t(int, skb->len, probe_size - len);
		if (nskb->ip_summed)
			skb_copy_bits(skb, 0, skb_put(nskb, copy), copy);
		else
			nskb->csum = skb_copy_and_csum_bits(skb, 0,
							    skb_put(nskb, copy),
							    copy, nskb->csum);

		if (skb->len <= copy) {
			/* We've eaten all the data from this skb.
			 * Throw it away. */
			TCP_SKB_CB(nskb)->flags |= TCP_SKB_CB(skb)->flags;
			tcp_unlink_write_queue(skb, sk);
			sk_wmem_free_skb(sk, skb);
		} else {
			TCP_SKB_CB(nskb)->flags |= TCP_SKB_CB(skb)->flags &
						   ~(TCPCB_FLAG_FIN|TCPCB_FLAG_PSH);
			if (!skb_shinfo(skb)->nr_frags) {
				skb_pull(skb, copy);
				if (skb->ip_summed != CHECKSUM_PARTIAL)
					skb->csum = csum_partial(skb->data,
								 skb->len, 0);
			} else {
				__pskb_trim_head(skb, copy);
				tcp_set_skb_tso_segs(sk, skb, mss_now);
			}
			TCP_SKB_CB(skb)->seq += copy;
#ifdef CONFIG_MTCP
			TCP_SKB_CB(skb)->data_seq += copy;
#endif
		}

		len += copy;

		if (len >= probe_size)
			break;
	}
	tcp_init_tso_segs(sk, nskb, nskb->len);

	/* We're ready to send.  If this fails, the probe will
	 * be resegmented into mss-sized pieces by tcp_write_xmit(). */
	TCP_SKB_CB(nskb)->when = tcp_time_stamp;
	if (!tcp_transmit_skb(sk, nskb, 1, GFP_ATOMIC)) {
		/* Decrement cwnd here because we are sending
		 * effectively two packets. */
		tp->snd_cwnd--;
		tcp_event_new_data_sent(sk, nskb);

		icsk->icsk_mtup.probe_size = tcp_mss_to_mtu(sk, nskb->len);
		tp->mtu_probe.probe_seq_start = TCP_SKB_CB(nskb)->seq;
		tp->mtu_probe.probe_seq_end = TCP_SKB_CB(nskb)->end_seq;

		return 1;
	}

	return -1;
}

extern int tcp_close_state(struct sock *sk);

/* This routine writes packets to the network.  It advances the
 * send_head.  This happens as incoming acks open up the remote
 * window for us.
 *
 * LARGESEND note: !tcp_urg_mode is overkill, only frames between
 * snd_up-64k-mss .. snd_up cannot be large. However, taking into
 * account rare use of URG, this is not a big flaw.
 *
 * Returns 1, if no segments are in flight and we have queued segments, but
 * cannot send anything now because of SWS or another problem.
 */
static int tcp_write_xmit(struct sock *sk, unsigned int mss_now, int nonagle)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sock *mpcb_sk=(struct sock*)tp->mpcb;
	struct sk_buff *skb;
	unsigned int tso_segs, sent_pkts;
	int cwnd_quota;
	int reinject;
	int result;
	
	if (sk->sk_in_write_xmit) {
		printk(KERN_ERR "sk in write xmit, meta_sk:%d\n",
		       is_meta_sk(sk));
		BUG();
	}
	/*We can be recursively called only in TCP_FIN_WAIT1 state (because
	  the very last segment calls tcp_send_fin() on all subflows)*/
	if(tp->mpcb && mpcb_sk->sk_in_write_xmit
	   && ((1<<mpcb_sk->sk_state) & ~(TCPF_FIN_WAIT1|TCPF_LAST_ACK))) {
		printk(KERN_ERR "meta-sk in write xmit, meta-sk:%d,"
		       "state of mpcb_sk:%d, of subsk:%d\n",
		       is_meta_sk(sk),((struct sock*)tp->mpcb)->sk_state,
		       sk->sk_state);
		BUG();
	}

	sk->sk_in_write_xmit=1;
	
	if (tp->mpc) {
		if (mss_now!=sysctl_mptcp_mss) {
			printk(KERN_ERR "write xmit-mss_now %d, mptcp mss:%d\n",
			       mss_now,sysctl_mptcp_mss);
			BUG();
		}
	}
	
	/* If we are closed, the bytes will have to remain here.
	 * In time closedown will finish, we empty the write queue and all
	 * will be happy.
	 */
	if (unlikely(sk->sk_state == TCP_CLOSE)) {
		sk->sk_in_write_xmit=0;
		return 0;
	}

	sent_pkts = 0;

	/* Do MTU probing. */	
	if ((result=tcp_mtu_probe(sk)) == 0) {
		sk->sk_in_write_xmit=0;
		tcpprobe_logmsg(sk,"mtu forces us out of write_xmit");
		return 0;
	}
	else if (result > 0) {
		sent_pkts = 1;
	}

	while ((skb=mtcp_next_segment(sk,&reinject))) {
		unsigned int limit;
		int err;
		struct sock *subsk;
		struct tcp_sock *subtp;
		struct sk_buff *subskb;

		if (reinject && !after(TCP_SKB_CB(skb)->end_data_seq,
				       tp->snd_una)) {
			/*another copy of the segment already reached
			  the peer, just discard this one.*/
			skb_unlink(skb,&tp->mpcb->reinject_queue);
			kfree_skb(skb);
			continue;
		}
		
		if (is_meta_tp(tp)) {
			int pf=0;
			subsk=get_available_subflow(tp->mpcb,skb,&pf);
			if (!subsk)
				break;
			subtp=tcp_sk(subsk);
		}
		else {
			subsk=sk; subtp=tp;
		}

		/*Since all subsocks are locked before calling the scheduler,
		  the tcp_send_head should not change.*/
		BUG_ON(!reinject && tcp_send_head(sk)!=skb);

		/*This must be invoked even if we don't want
		  to support TSO at the moment*/
		tso_segs=tcp_init_tso_segs(sk,skb,mss_now);
		BUG_ON(!tso_segs);
		/*At the moment we do not support tso, hence 
		  tso_segs must be 1*/
		BUG_ON(tp->mpc && tso_segs!=1);

		/*decide to which subsocket we give the skb*/
		
		cwnd_quota = tcp_cwnd_test(subtp, skb);
		if (!cwnd_quota) {
			/*Should not happen, since mptcp must have
			  chosen a subsock with open cwnd*/
			if (sk!=subsk) BUG();
			if (reinject) printk(KERN_ERR "reinj: line %d\n", 
					     __LINE__);
			break;
		}

		if (unlikely(!tcp_snd_wnd_test(subtp, skb, mss_now))) {
			if (reinject) printk(KERN_ERR "reinj: line %d\n", 
					     __LINE__);
			break;
		}
		
		if (unlikely(!tcp_nagle_test(tp, skb, mss_now,
					     (tcp_skb_is_last(sk, skb) ?
					      nonagle : 
					      TCP_NAGLE_PUSH)))) {
			if (reinject) printk(KERN_ERR "reinj: line %d\n", 
					     __LINE__);
			break;
		}
		
		limit = mss_now;

		if (skb->len > limit &&
		    unlikely(tso_fragment(sk, skb, limit, mss_now))) {
			if (reinject) printk(KERN_ERR "reinj: line %d\n", __LINE__);
			break;
		}

		if (sk!=subsk) {
			if (tp->path_index) 
				skb->path_mask|=PI_TO_FLAG(tp->path_index);
			/*If the segment is reinjected, the clone is done 
			  already*/
			if (!reinject)
				subskb=skb_clone(skb,GFP_ATOMIC);
			else {
				skb_unlink(skb,&tp->mpcb->reinject_queue);
				subskb=skb;
			}
			if (!subskb) {
				if (reinject) printk(KERN_ERR "reinj: line %d\n", __LINE__);
				break;
			}
			BUG_ON(tcp_send_head(subsk));
			mtcp_skb_entail(subsk, subskb);
			if (reinject) {
				tcpprobe_logmsg(sk,"reinj:seq is %#x",
						TCP_SKB_CB(subskb)->seq);
			}
		}
		else
			subskb=skb;
		

		TCP_SKB_CB(subskb)->when = tcp_time_stamp;
		if (unlikely(err=tcp_transmit_skb(subsk, subskb, 1, 
						  GFP_ATOMIC))) {
 			if (sk!=subsk) {
				/*Remove the skb from the subsock*/
				tcp_advance_send_head(subsk,subskb);
				tcp_unlink_write_queue(subskb,subsk);
				subtp->write_seq-=subskb->len;
				mtcp_wmem_free_skb(subsk, subskb);
				/*If we entered CWR, just try to give
				  that same skb to another subflow,
				  by querying again the scheduler,
				  we need however to ensure that the
				  same subflow is not selected again by
				  the scheduler, to avoid looping*/
				if (err>0 && tp->mpcb->cnt_subflows>1) {
					tp->mpcb->noneligible|=
						PI_TO_FLAG(subtp->path_index);
					continue;
				}
			}
			break;
		}	
		
		/* Advance the send_head.  This one is sent out.
		 * This call will increment packets_out.
		 */
		if(!reinject && tcp_send_head(sk)!=skb) {
			printk(KERN_ERR "sock_owned_by_user:%d\n",
			       sock_owned_by_user(sk));
			BUG();
			       
		}
		if (sk!=subsk && !reinject)
			tocheck=1;
		check_skb=skb;
		check_sk=sk;
		tcp_event_new_data_sent(subsk, subskb);
 		if (sk!=subsk) BUG_ON(tcp_send_head(subsk));
		tocheck=0;
		if (sk!=subsk && !reinject) {
			BUG_ON(tcp_send_head(sk)!=skb);
			tcp_event_new_data_sent(sk,skb);
		}
		
		if (sk!=subsk &&
		    (TCP_SKB_CB(skb)->flags & TCPCB_FLAG_FIN)) {
			struct sock *sk_it, *sk_tmp;
			BUG_ON(!tcp_close_state(subsk));
			/*App close: we have sent every app-level byte,
			  send now the FIN on all subflows.
			  if the FIN was triggered by mtcp_close(),
			  then the SHUTDOWN_MASK is set and we call
			  tcp_close() on all subsocks. Otherwise
			  only sk_shutdown has been called, and 
			  we just send the fin on all subflows.*/
			mtcp_for_each_sk_safe(tp->mpcb,sk_it,sk_tmp) {
				if (sk->sk_shutdown == SHUTDOWN_MASK)
					tcp_close(sk_it,-1);
				else if (sk_it!=subsk && 
					 tcp_close_state(sk_it)) {
					tcp_send_fin(sk_it);
				}
			}
		}
		

		tcp_minshall_update(tp, mss_now, skb);
		sent_pkts++;

		tcp_cwnd_validate(subsk);
	}

	if (tp->mpcb) tp->mpcb->noneligible=0;

	if (likely(sent_pkts)) {
		sk->sk_in_write_xmit=0;
		return 0;
	}

	sk->sk_in_write_xmit=0;
	return !tp->packets_out && tcp_send_head(sk);
}

/* Push out any pending frames which were held back due to
 * TCP_CORK or attempt at coalescing tiny packets.
 * The socket must be locked by the caller.
 */
void __tcp_push_pending_frames(struct sock *sk, unsigned int cur_mss,
			       int nonagle)
{
	struct sk_buff *skb = mtcp_next_segment(sk,NULL);

	if (skb) {
		if (tcp_write_xmit(sk, cur_mss, nonagle)) {
			if (!is_meta_sk(sk))
				tcp_check_probe_timer(sk);
			else {
				struct sock *sk_it;
				struct tcp_sock *tp_it;
				mtcp_for_each_sk(tcp_sk(sk)->mpcb,sk_it,
						 tp_it)
					tcp_check_probe_timer(sk_it);
			}
		}
	}
	else 	tcpprobe_logmsg(sk,"not running write_xmit");
}

/* Send _single_ skb sitting at the send head. This function requires
 * true push pending frames to setup probe timer etc.
 */
void tcp_push_one(struct sock *sk, unsigned int mss_now)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int reinject;
	struct sk_buff *skb;
	unsigned int tso_segs, cwnd_quota;
	struct sock *subsk;
	struct tcp_sock *subtp;
	int err;

again:
	skb=mtcp_next_segment(sk,&reinject);
	BUG_ON(!skb);

	while (reinject && !after(TCP_SKB_CB(skb)->end_data_seq,
			       tp->snd_una)) {
		/*another copy of the segment already reached
		  the peer, just discard this one.*/
		skb_unlink(skb,&tp->mpcb->reinject_queue);
		kfree_skb(skb);
		skb=mtcp_next_segment(sk,&reinject);
	}
	
	BUG_ON(!skb);

	if (is_meta_tp(tp)) {
		subsk=get_available_subflow(tp->mpcb,skb,NULL);
		subtp=tcp_sk(subsk);
		if (!subsk)
			goto out;
		subsk->sk_debug=4;		
	}
	else 
		subsk=sk; subtp=tp;
	
	BUG_ON(!reinject && tcp_send_head(sk)!=skb);

	if (skb->len<mss_now) {
		printk(KERN_ERR "skb->len:%d,mss_now:%d\n",skb->len,
		       mss_now);
		BUG();
	}
	
	tso_segs = tcp_init_tso_segs(sk,skb,mss_now);

	cwnd_quota = tcp_snd_test(subsk, skb, mss_now, TCP_NAGLE_PUSH);

	if (likely(cwnd_quota)) {
		unsigned int limit;
		struct sk_buff *subskb;

		BUG_ON(!tso_segs);
		/*At the moment we do not support tso, hence 
		  tso_segs must be 1*/
		BUG_ON(tp->mpc && tso_segs!=1);

		limit = mss_now;

		BUG_ON(tp->mpc && skb->len>limit);

		if (skb->len > limit &&
		    unlikely(tso_fragment(sk, skb, limit, mss_now))) {
			mtcp_debug("NOT SENDING TCP SEGMENT\n");
			goto out;
		}

		/* Send it out now. */

		if (sk!=subsk) {
			if (tp->path_index)
				skb->path_mask|=PI_TO_FLAG(tp->path_index);
			if (!reinject) {
				subskb=skb_clone(skb,GFP_KERNEL);
			}
			else {
				skb_unlink(skb,&tp->mpcb->reinject_queue);
				subskb=skb;
			}
			if (!subskb) {
				printk(KERN_ERR "skb_clone failed\n");
				goto out;
			}
			BUG_ON(tcp_send_head(subsk));
			mtcp_skb_entail(subsk, subskb);
		}
		else
			subskb=skb;

		BUG_ON(tcp_send_head(sk)!=skb);

		TCP_SKB_CB(subskb)->when = tcp_time_stamp;
		if (likely(!(err=tcp_transmit_skb(subsk, subskb, 1, 
						  subsk->sk_allocation)))) {
			if (TCP_SKB_CB(skb)->flags & TCPCB_FLAG_FIN) {
				struct sock *sk_it;
				struct tcp_sock *tp_it;
				/*App close: we have sent every app-level byte,
				  send now the FIN on all subflows.*/
				mtcp_for_each_sk(tp->mpcb,sk_it,tp_it)
					if (sk_it!=subsk)
						tcp_send_fin(sk_it);
			}
			tcp_event_new_data_sent(subsk, subskb);
			BUG_ON(tcp_send_head(subsk));
			if (sk!=subsk && !reinject)
				tcp_event_new_data_sent(sk,skb);
			tcp_cwnd_validate(subsk);
		}
		else if (sk!=subsk) {
			/*Remove the skb from the subsock*/
			tcp_advance_send_head(subsk,subskb);
			tcp_unlink_write_queue(subskb,subsk);
			subtp->write_seq-=subskb->len;
			mtcp_wmem_free_skb(subsk, subskb);
			if (err>0 && tp->mpcb->cnt_subflows>1) {
				tp->mpcb->noneligible|=
					PI_TO_FLAG(subtp->path_index);
				goto again;
			}
		}
	}
out:
	if (tp->mpcb) tp->mpcb->noneligible=0;
}

/* This function returns the amount that we can raise the
 * usable window based on the following constraints
 *
 * 1. The window can never be shrunk once it is offered (RFC 793)
 * 2. We limit memory per socket
 *
 * RFC 1122:
 * "the suggested [SWS] avoidance algorithm for the receiver is to keep
 *  RECV.NEXT + RCV.WIN fixed until:
 *  RCV.BUFF - RCV.USER - RCV.WINDOW >= min(1/2 RCV.BUFF, MSS)"
 *
 * i.e. don't raise the right edge of the window until you can raise
 * it at least MSS bytes.
 *
 * Unfortunately, the recommended algorithm breaks header prediction,
 * since header prediction assumes th->window stays fixed.
 *
 * Strictly speaking, keeping th->window fixed violates the receiver
 * side SWS prevention criteria. The problem is that under this rule
 * a stream of single byte packets will cause the right side of the
 * window to always advance by a single byte.
 *
 * Of course, if the sender implements sender side SWS prevention
 * then this will not be a problem.
 *
 * BSD seems to make the following compromise:
 *
 *	If the free space is less than the 1/4 of the maximum
 *	space available and the free space is less than 1/2 mss,
 *	then set the window to 0.
 *	[ Actually, bsd uses MSS and 1/4 of maximal _window_ ]
 *	Otherwise, just prevent the window from shrinking
 *	and from being larger than the largest representable value.
 *
 * This prevents incremental opening of the window in the regime
 * where TCP is limited by the speed of the reader side taking
 * data out of the TCP receive queue. It does nothing about
 * those cases where the window is constrained on the sender side
 * because the pipeline is full.
 *
 * BSD also seems to "accidentally" limit itself to windows that are a
 * multiple of MSS, at least until the free space gets quite small.
 * This would appear to be a side effect of the mbuf implementation.
 * Combining these two algorithms results in the observed behavior
 * of having a fixed window size at almost all times.
 *
 * Below we obtain similar behavior by forcing the offered window to
 * a multiple of the mss when it is feasible to do so.
 *
 * Note, we don't "adjust" for TIMESTAMP or SACK option bytes.
 * Regular options like TIMESTAMP are taken into account.
 */

#ifndef CONFIG_MTCP
#define __tcp_select_window_fallback __tcp_select_window
#endif

u32 __tcp_select_window_fallback(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	/* MSS for the peer's data.  Previous versions used mss_clamp
	 * here.  I don't know if the value based on our guesses
	 * of peer's MSS is better for the performance.  It's more correct
	 * but may be worse for the performance because of rcv_mss
	 * fluctuations.  --SAW  1998/11/1
	 */
	int mss = icsk->icsk_ack.rcv_mss;
	int free_space = tcp_space(sk);
	int full_space = min_t(int, tp->window_clamp, tcp_full_space(sk));
	int window;

	if (mss > full_space)
		mss = full_space;

	if (free_space < (full_space >> 1)) {
		icsk->icsk_ack.quick = 0;

		if (tcp_memory_pressure) {
			tp->rcv_ssthresh = min(tp->rcv_ssthresh,
					       4U * tp->advmss);
		}

		if (free_space < mss)
			return 0;
	}

	if (free_space > tp->rcv_ssthresh) {
		free_space = tp->rcv_ssthresh;
	}

	/* Don't do rounding if we are using window scaling, since the
	 * scaled window will not line up with the MSS boundary anyway.
	 */
	window = tp->rcv_wnd;
	if (tp->rx_opt.rcv_wscale) {
		window = free_space;

		/* Advertise enough space so that it won't get scaled away.
		 * Import case: prevent zero window announcement if
		 * 1<<rcv_wscale > mss.
		 */
		if (((window >> tp->rx_opt.rcv_wscale) << tp->rx_opt.rcv_wscale) != window)
			window = (((window >> tp->rx_opt.rcv_wscale) + 1)
				  << tp->rx_opt.rcv_wscale);
	} else {
		/* Get the largest window that is a nice multiple of mss.
		 * Window clamp already applied above.
		 * If our current window offering is within 1 mss of the
		 * free space we just keep it. This prevents the divide
		 * and multiply from happening most of the time.
		 * We also don't do any window rounding when the free space
		 * is too small.
		 */
		if (window <= free_space - mss || window > free_space)
			window = (free_space / mss) * mss;
		else if (mss == full_space &&
			 free_space > window + (full_space >> 1))
			window = free_space;
	}

	return window;
}

#ifdef CONFIG_MTCP
u32 __tcp_select_window(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	struct multipath_pcb *mpcb = tp->mpcb;
	int mss,free_space,full_space,window;

	BUG_ON(!tp->mpcb && !tp->pending);
	if (!tp->mpc || !tp->mpcb) return __tcp_select_window_fallback(sk);

	/* MSS for the peer's data.  Previous versions used mss_clamp
	 * here.  I don't know if the value based on our guesses
	 * of peer's MSS is better for the performance.  It's more correct
	 * but may be worse for the performance because of rcv_mss
	 * fluctuations.  --SAW  1998/11/1
	 */
	mss = icsk->icsk_ack.rcv_mss;
	free_space = mtcp_space(sk);
	full_space = min_t(int, mpcb->tp.window_clamp, mtcp_full_space(sk));

	if (mss > full_space)
		mss = full_space;

	if (free_space < (full_space >> 1)) {
		icsk->icsk_ack.quick = 0;

		if (tcp_memory_pressure) {
			tp->rcv_ssthresh = min(tp->rcv_ssthresh,
					       4U * tp->advmss);
			mtcp_update_window_clamp(mpcb);
		}

		if (free_space < mss)
			return 0;
	}

	if (free_space > mpcb->tp.rcv_ssthresh) {
		free_space = mpcb->tp.rcv_ssthresh;
	}

	/* Don't do rounding if we are using window scaling, since the
	 * scaled window will not line up with the MSS boundary anyway.
	 */
	window = tp->rcv_wnd;
	if (tp->rx_opt.rcv_wscale) {
		window = free_space;

		/* Advertise enough space so that it won't get scaled away.
		 * Import case: prevent zero window announcement if
		 * 1<<rcv_wscale > mss.
		 */
		if (((window >> tp->rx_opt.rcv_wscale) << tp->rx_opt.rcv_wscale) != window)
			window = (((window >> tp->rx_opt.rcv_wscale) + 1)
				  << tp->rx_opt.rcv_wscale);
	} else {
		/* Get the largest window that is a nice multiple of mss.
		 * Window clamp already applied above.
		 * If our current window offering is within 1 mss of the
		 * free space we just keep it. This prevents the divide
		 * and multiply from happening most of the time.
		 * We also don't do any window rounding when the free space
		 * is too small.
		 */
		if (window <= free_space - mss || window > free_space)
			window = (free_space / mss) * mss;
		else if (mss == full_space &&
			 free_space > window + (full_space >> 1))
			window = free_space;
	}

	return window;
}
#endif

/* Attempt to collapse two adjacent SKB's during retransmission. */
static void tcp_retrans_try_collapse(struct sock *sk, struct sk_buff *skb,
				     int mss_now)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *next_skb = tcp_write_queue_next(sk, skb);
	int skb_size, next_skb_size;
	u16 flags;

	/* The first test we must make is that neither of these two
	 * SKB's are still referenced by someone else.
	 */
	if (skb_cloned(skb) || skb_cloned(next_skb))
		return;

	skb_size = skb->len;
	next_skb_size = next_skb->len;
	flags = TCP_SKB_CB(skb)->flags;

	/* Also punt if next skb has been SACK'd. */
	if (TCP_SKB_CB(next_skb)->sacked & TCPCB_SACKED_ACKED)
		return;

	/* Next skb is out of window. */
	if (!tp->mpc && after(TCP_SKB_CB(next_skb)->end_seq, tcp_wnd_end(tp,0)))
		return;
	if (tp->mpc && after(TCP_SKB_CB(next_skb)->end_data_seq, 
			     tcp_wnd_end(tp,1)))
		return;
	
	/* Punt if not enough space exists in the first SKB for
	 * the data in the second, or the total combined payload
	 * would exceed the MSS.
	 */
	if ((next_skb_size > skb_tailroom(skb)) ||
	    ((skb_size + next_skb_size) > mss_now))
		return;

	BUG_ON(tcp_skb_pcount(skb) != 1 || tcp_skb_pcount(next_skb) != 1);

	tcp_highest_sack_combine(sk, next_skb, skb);

	/* Ok.	We will be able to collapse the packet. */
	tcp_unlink_write_queue(next_skb, sk);

	skb_copy_from_linear_data(next_skb, skb_put(skb, next_skb_size),
				  next_skb_size);

	if (next_skb->ip_summed == CHECKSUM_PARTIAL)
		skb->ip_summed = CHECKSUM_PARTIAL;

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		skb->csum = csum_block_add(skb->csum, next_skb->csum, skb_size);

	/* Update sequence range on original skb. */
	TCP_SKB_CB(skb)->end_seq = TCP_SKB_CB(next_skb)->end_seq;
	/*For the dsn space, we need to make an addition and not just
	  copy the end_seq, because if the next_skb is a pure FIN (with
	  no data), the len is 1 and the data_len is 0, as well as
	  the end_data_seq of the FIN. Using an addition takes this
	  difference into account*/
	TCP_SKB_CB(skb)->end_data_seq += TCP_SKB_CB(next_skb)->data_len;
	TCP_SKB_CB(skb)->data_len += TCP_SKB_CB(next_skb)->data_len;

	/* Merge over control information. */
	flags |= TCP_SKB_CB(next_skb)->flags; /* This moves PSH/FIN etc. over */
	TCP_SKB_CB(skb)->flags = flags;

	/* All done, get rid of second SKB and account for it so
	 * packet counting does not break.
	 */
	TCP_SKB_CB(skb)->sacked |= TCP_SKB_CB(next_skb)->sacked & TCPCB_EVER_RETRANS;
	if (TCP_SKB_CB(next_skb)->sacked & TCPCB_SACKED_RETRANS)
		tp->retrans_out -= tcp_skb_pcount(next_skb);
	if (TCP_SKB_CB(next_skb)->sacked & TCPCB_LOST)
		tp->lost_out -= tcp_skb_pcount(next_skb);
	/* Reno case is special. Sigh... */
	if (tcp_is_reno(tp) && tp->sacked_out)
		tcp_dec_pcount_approx(&tp->sacked_out, next_skb);

	tcp_adjust_fackets_out(sk, next_skb, tcp_skb_pcount(next_skb));
	tp->packets_out -= tcp_skb_pcount(next_skb);

	/* changed transmit queue under us so clear hints */
	tcp_clear_retrans_hints_partial(tp);
	if (next_skb == tp->retransmit_skb_hint)
		tp->retransmit_skb_hint = skb;

	sk_wmem_free_skb(sk, next_skb);
}

/* Do a simple retransmit without using the backoff mechanisms in
 * tcp_timer. This is used for path mtu discovery.
 * The socket is already locked here.
 */
void tcp_simple_retransmit(struct sock *sk)
{
	const struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb;
	unsigned int mss = tcp_current_mss(sk, 0);
	u32 prior_lost = tp->lost_out;

	tcp_for_write_queue(skb, sk) {
		if (skb == tcp_send_head(sk))
			break;
		if (skb->len > mss &&
		    !(TCP_SKB_CB(skb)->sacked & TCPCB_SACKED_ACKED)) {
			if (TCP_SKB_CB(skb)->sacked & TCPCB_SACKED_RETRANS) {
				TCP_SKB_CB(skb)->sacked &= ~TCPCB_SACKED_RETRANS;
				tp->retrans_out -= tcp_skb_pcount(skb);
			}
			tcp_skb_mark_lost_uncond_verify(tp, skb);
		}
	}

	tcp_clear_retrans_hints_partial(tp);

	if (prior_lost == tp->lost_out)
		return;

	if (tcp_is_reno(tp))
		tcp_limit_reno_sacked(tp);

	tcp_verify_left_out(tp);

	/* Don't muck with the congestion window here.
	 * Reason is that we do not increase amount of _data_
	 * in network, but units changed and effective
	 * cwnd/ssthresh really reduced now.
	 */
	if (icsk->icsk_ca_state != TCP_CA_Loss) {
		tp->high_seq = tp->snd_nxt;
		tp->snd_ssthresh = tcp_current_ssthresh(sk);
		tp->prior_ssthresh = 0;
		tp->undo_marker = 0;
		tcp_set_ca_state(sk, TCP_CA_Loss);
	}
	tcp_xmit_retransmit_queue(sk);
}

/* This retransmits one SKB.  Policy decisions and retransmit queue
 * state updates are done by the caller.  Returns non-zero if an
 * error occurred which prevented the send.
 */
int tcp_retransmit_skb(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);
	unsigned int cur_mss;
	int err;

	BUG_ON(!skb);
	
	/*In case of RTO (loss state), we reinject data on another subflow*/
	if (icsk->icsk_ca_state == TCP_CA_Loss &&
	    tp->mpc && sk->sk_state==TCP_ESTABLISHED &&
	    tp->path_index) {
		mtcp_reinject_data(sk);
	}
	
	/* Inconclusive MTU probe */
	if (icsk->icsk_mtup.probe_size) {
		icsk->icsk_mtup.probe_size = 0;
	}

	/* Do not sent more than we queued. 1/4 is reserved for possible
	 * copying overhead: fragmentation, tunneling, mangling etc.
	 */
	if (atomic_read(&sk->sk_wmem_alloc) >
	    min(sk->sk_wmem_queued + (sk->sk_wmem_queued >> 2), sk->sk_sndbuf))
		return -EAGAIN;

	if (before(TCP_SKB_CB(skb)->seq, tp->snd_una)) {
		if (before(TCP_SKB_CB(skb)->end_seq, tp->snd_una))
			BUG();
		if (tcp_trim_head(sk, skb, tp->snd_una - TCP_SKB_CB(skb)->seq))
			return -ENOMEM;
	}

	if (inet_csk(sk)->icsk_af_ops->rebuild_header(sk))
		return -EHOSTUNREACH; /* Routing failure or similar. */

#ifdef CONFIG_MTCP
	cur_mss = sysctl_mptcp_mss;
#else
	cur_mss = tcp_current_mss(sk, 0);
#endif

	/* If receiver has shrunk his window, and skb is out of
	 * new window, do not retransmit it. The exception is the
	 * case, when window is shrunk to zero. In this case
	 * our retransmit serves as a zero window probe.
	 */
	if (!before((tp->mpc)?TCP_SKB_CB(skb)->data_seq:
		    TCP_SKB_CB(skb)->seq, tcp_wnd_end(tp,tp->mpc))
	    && TCP_SKB_CB(skb)->seq != tp->snd_una)
		return -EAGAIN;

	if (skb->len > cur_mss) {
		if (tcp_fragment(sk, skb, cur_mss, cur_mss))
			return -ENOMEM; /* We'll try again later. */
	}

	/* Collapse two adjacent packets if worthwhile and we can. */
	if (!(TCP_SKB_CB(skb)->flags & TCPCB_FLAG_SYN) &&
	    (skb->len < (cur_mss >> 1)) &&
	    (!tcp_skb_is_last(sk, skb)) &&
	    (tcp_write_queue_next(sk, skb) != tcp_send_head(sk)) &&
	    (skb_shinfo(skb)->nr_frags == 0 &&
	     skb_shinfo(tcp_write_queue_next(sk, skb))->nr_frags == 0) &&
	    (tcp_skb_pcount(skb) == 1 &&
	     tcp_skb_pcount(tcp_write_queue_next(sk, skb)) == 1) &&
	    (sysctl_tcp_retrans_collapse != 0))
		tcp_retrans_try_collapse(sk, skb, cur_mss);

	/* Some Solaris stacks overoptimize and ignore the FIN on a
	 * retransmit when old data is attached.  So strip it off
	 * since it is cheap to do so and saves bytes on the network.
	 */
	if (skb->len > 0 &&
	    (TCP_SKB_CB(skb)->flags & TCPCB_FLAG_FIN) &&
	    tp->snd_una == (TCP_SKB_CB(skb)->end_seq - 1)) {
		if (!pskb_trim(skb, 0)) {
			/* Reuse, even though it does some unnecessary work */
			tcp_init_nondata_skb(skb, TCP_SKB_CB(skb)->end_seq - 1,
					     TCP_SKB_CB(skb)->flags);
			skb->ip_summed = CHECKSUM_NONE;
		}
	}

	/* Make a copy, if the first transmission SKB clone we made
	 * is still in somebody's hands, else make a clone.
	 */
	TCP_SKB_CB(skb)->when = tcp_time_stamp;

	err = tcp_transmit_skb(sk, skb, 1, GFP_ATOMIC);

	if (err == 0) {
		/* Update global TCP statistics. */
		TCP_INC_STATS(sock_net(sk), TCP_MIB_RETRANSSEGS);

		tp->total_retrans++;

		if (!tp->retrans_out)
			tp->lost_retrans_low = tp->snd_nxt;

		TCP_SKB_CB(skb)->sacked |= TCPCB_RETRANS;
		tp->retrans_out += tcp_skb_pcount(skb);

		/* Save stamp of the first retransmit. */
		if (!tp->retrans_stamp)
			tp->retrans_stamp = TCP_SKB_CB(skb)->when;

		tp->undo_retrans++;

		/* snd_nxt is stored to detect loss of retransmitted segment,
		 * see tcp_input.c tcp_sacktag_write_queue().
		 */
		TCP_SKB_CB(skb)->ack_seq = tp->snd_nxt;
	}
	return err;
}

static int tcp_can_forward_retransmit(struct sock *sk)
{
	const struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);

	/* Forward retransmissions are possible only during Recovery. */
	if (icsk->icsk_ca_state != TCP_CA_Recovery)
		return 0;

	/* No forward retransmissions in Reno are possible. */
	if (tcp_is_reno(tp))
		return 0;

	/* Yeah, we have to make difficult choice between forward transmission
	 * and retransmission... Both ways have their merits...
	 *
	 * For now we do not retransmit anything, while we have some new
	 * segments to send. In the other cases, follow rule 3 for
	 * NextSeg() specified in RFC3517.
	 */

	if (tcp_may_send_now(sk))
		return 0;

	return 1;
}

/* This gets called after a retransmit timeout, and the initially
 * retransmitted data is acknowledged.  It tries to continue
 * resending the rest of the retransmit queue, until either
 * we've sent it all or the congestion window limit is reached.
 * If doing SACK, the first ACK which comes back for a timeout
 * based retransmit packet might feed us FACK information again.
 * If so, we use it to avoid unnecessarily retransmissions.
 */
void tcp_xmit_retransmit_queue(struct sock *sk)
{
	const struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb;
	struct sk_buff *hole = NULL;
	u32 last_lost;
	int mib_idx;
	int fwd_rexmitting = 0;

	BUG_ON(is_meta_sk(sk));

	if (!tp->lost_out)
		tp->retransmit_high = tp->snd_una;

	if (tp->retransmit_skb_hint) {
		skb = tp->retransmit_skb_hint;
		last_lost = TCP_SKB_CB(skb)->end_seq;
		if (after(last_lost, tp->retransmit_high))
			last_lost = tp->retransmit_high;
	} else {
		skb = tcp_write_queue_head(sk);
		last_lost = tp->snd_una;
	}

	/* First pass: retransmit lost packets. */
	tcp_for_write_queue_from(skb, sk) {
		__u8 sacked = TCP_SKB_CB(skb)->sacked;

		if (skb == tcp_send_head(sk))
			break;
		/* we could do better than to assign each time */
		if (hole == NULL)
			tp->retransmit_skb_hint = skb;

		/* Assume this retransmit will generate
		 * only one packet for congestion window
		 * calculation purposes.  This works because
		 * tcp_retransmit_skb() will chop up the
		 * packet to be MSS sized and all the
		 * packet counting works out.
		 */
		if (tcp_packets_in_flight(tp) >= tp->snd_cwnd)
			return;

		if (fwd_rexmitting) {
begin_fwd:
			if (!before(TCP_SKB_CB(skb)->seq, tcp_highest_sack_seq(tp)))
				break;
			mib_idx = LINUX_MIB_TCPFORWARDRETRANS;

		} else if (!before(TCP_SKB_CB(skb)->seq, tp->retransmit_high)) {
			tp->retransmit_high = last_lost;
			if (!tcp_can_forward_retransmit(sk))
				break;
			/* Backtrack if necessary to non-L'ed skb */
			if (hole != NULL) {
				skb = hole;
				hole = NULL;
			}
			fwd_rexmitting = 1;
			goto begin_fwd;

		} else if (!(sacked & TCPCB_LOST)) {
			if (hole == NULL && !(sacked & TCPCB_SACKED_RETRANS))
				hole = skb;
			continue;

		} else {
			last_lost = TCP_SKB_CB(skb)->end_seq;
			if (icsk->icsk_ca_state != TCP_CA_Loss)
				mib_idx = LINUX_MIB_TCPFASTRETRANS;
			else
				mib_idx = LINUX_MIB_TCPSLOWSTARTRETRANS;
		}

		if (sacked & (TCPCB_SACKED_ACKED|TCPCB_SACKED_RETRANS))
			continue;

		if (tcp_retransmit_skb(sk, skb))
			return;
		NET_INC_STATS_BH(sock_net(sk), mib_idx);

		if (skb == tcp_write_queue_head(sk))
			inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
						  inet_csk(sk)->icsk_rto,
						  TCP_RTO_MAX);
	}
}

/* Send a fin.  The caller locks the socket for us.  This cannot be
 * allowed to fail queueing a FIN frame under any circumstances.
 */
void tcp_send_fin(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb = tcp_write_queue_tail(sk);
	int mss_now;

	/* Optimization, tack on the FIN if we have a queue of
	 * unsent frames.  But be careful about outgoing SACKS
	 * and IP options.
	 */
	if (!tp->mpc) mss_now = tcp_current_mss(sk, 1);
	else mss_now = sysctl_mptcp_mss;

	if (tcp_send_head(sk) != NULL) {
		TCP_SKB_CB(skb)->flags |= TCPCB_FLAG_FIN;
		TCP_SKB_CB(skb)->end_seq++;
		tp->write_seq++;	       
	}
	else {
		/* Socket is locked, keep trying until memory is available. 
		   Due to the possible call from tcp_write_xmit, we might
		   be called from interrupt context, hence the following cond.*/
		if (!in_interrupt())
			for (;;) {
				skb = alloc_skb_fclone(MAX_TCP_HEADER, 
						       GFP_KERNEL);
				if (skb)
					break;
				yield();
			}
		else
			skb = alloc_skb_fclone(MAX_TCP_HEADER, 
					       GFP_ATOMIC);

		/* Reserve space for headers and prepare control bits. */
		skb_reserve(skb, MAX_TCP_HEADER);
		/* FIN eats a sequence byte, write_seq advanced by 
		   tcp_queue_skb(). */
		tcp_init_nondata_skb(skb, tp->write_seq,
				     TCPCB_FLAG_ACK | TCPCB_FLAG_FIN);
		tcp_queue_skb(sk, skb);
	}
	__tcp_push_pending_frames(sk, mss_now, TCP_NAGLE_OFF);
}

/* We get here when a process closes a file descriptor (either due to
 * an explicit close() or as a byproduct of exit()'ing) and there
 * was unread data in the receive queue.  This behavior is recommended
 * by RFC 2525, section 2.17.  -DaveM
 */
void tcp_send_active_reset(struct sock *sk, gfp_t priority)
{
	struct sk_buff *skb;

	/* NOTE: No TCP options attached and we never retransmit this. */
	skb = alloc_skb(MAX_TCP_HEADER, priority);
	if (!skb) {
		NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPABORTFAILED);
		return;
	}

	/* Reserve space for headers and prepare control bits. */
	skb_reserve(skb, MAX_TCP_HEADER);
	tcp_init_nondata_skb(skb, tcp_acceptable_seq(sk),
			     TCPCB_FLAG_ACK | TCPCB_FLAG_RST);
	/* Send it off. */
	TCP_SKB_CB(skb)->when = tcp_time_stamp;
	if (tcp_transmit_skb(sk, skb, 0, priority))
		NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPABORTFAILED);

	TCP_INC_STATS(sock_net(sk), TCP_MIB_OUTRSTS);
}

/* WARNING: This routine must only be called when we have already sent
 * a SYN packet that crossed the incoming SYN that caused this routine
 * to get called. If this assumption fails then the initial rcv_wnd
 * and rcv_wscale values will not be correct.
 */
int tcp_send_synack(struct sock *sk)
{
	struct sk_buff *skb;

	skb = tcp_write_queue_head(sk);
	if (skb == NULL || !(TCP_SKB_CB(skb)->flags & TCPCB_FLAG_SYN)) {
		printk(KERN_DEBUG "tcp_send_synack: wrong queue state\n");
		return -EFAULT;
	}
	if (!(TCP_SKB_CB(skb)->flags & TCPCB_FLAG_ACK)) {
		if (skb_cloned(skb)) {
			struct sk_buff *nskb = skb_copy(skb, GFP_ATOMIC);
			if (nskb == NULL)
				return -ENOMEM;
			tcp_unlink_write_queue(skb, sk);
			skb_header_release(nskb);
			__tcp_add_write_queue_head(sk, nskb);
			sk_wmem_free_skb(sk, skb);
			sk->sk_wmem_queued += nskb->truesize;
			sk_mem_charge(sk, nskb->truesize);
			skb = nskb;
		}

		TCP_SKB_CB(skb)->flags |= TCPCB_FLAG_ACK;
		TCP_ECN_send_synack(tcp_sk(sk), skb);
	}
	TCP_SKB_CB(skb)->when = tcp_time_stamp;
	return tcp_transmit_skb(sk, skb, 1, GFP_ATOMIC);
}

/*
 * Prepare a SYN-ACK.
 */
struct sk_buff *tcp_make_synack(struct sock *sk, struct dst_entry *dst,
				struct request_sock *req)
{
	struct inet_request_sock *ireq = inet_rsk(req);
	struct tcp_sock *tp = tcp_sk(sk);
	struct tcphdr *th;
	int tcp_header_size;
	struct tcp_out_options opts;
	struct sk_buff *skb;
	struct tcp_md5sig_key *md5;
	__u8 *md5_hash_location;
	int mss;

	skb = sock_wmalloc(sk, MAX_TCP_HEADER + 15, 1, GFP_ATOMIC);
	if (skb == NULL)
		return NULL;

	/* Reserve space for headers. */
	skb_reserve(skb, MAX_TCP_HEADER);

	skb->dst = dst_clone(dst);
#ifdef CONFIG_MTCP
	mss = sysctl_mptcp_mss;
#else
	mss = dst_metric(dst, RTAX_ADVMSS);
#endif

	if (tp->rx_opt.user_mss && tp->rx_opt.user_mss < mss)
		mss = tp->rx_opt.user_mss;

	if (req->rcv_wnd == 0) { /* ignored for retransmitted syns */
		__u8 rcv_wscale;
		/* Set this up on the first call only */
		req->window_clamp = tp->window_clamp ? : dst_metric(dst, RTAX_WINDOW);
		/* tcp_full_space because it is guaranteed to be the first packet */
#ifdef CONFIG_MTCP
		tcp_select_initial_window(mtcp_full_space(sk),
					  mss - (ireq->tstamp_ok ? 
						 TCPOLEN_TSTAMP_ALIGNED : 0),
					  &req->rcv_wnd,
					  &req->window_clamp,
					  ireq->wscale_ok,
					  &rcv_wscale);
#else
		tcp_select_initial_window(tcp_full_space(sk),
					  mss - (ireq->tstamp_ok ? 
						 TCPOLEN_TSTAMP_ALIGNED : 0),
					  &req->rcv_wnd,
					  &req->window_clamp,
					  ireq->wscale_ok,
					  &rcv_wscale);
#endif
		ireq->rcv_wscale = rcv_wscale;
	}

	memset(&opts, 0, sizeof(opts));
#ifdef CONFIG_SYN_COOKIES
	if (unlikely(req->cookie_ts))
		TCP_SKB_CB(skb)->when = cookie_init_timestamp(req);
	else
#endif
	TCP_SKB_CB(skb)->when = tcp_time_stamp;
	tcp_header_size = tcp_synack_options(sk, req, mss,
					     skb, &opts, &md5) +
		sizeof(struct tcphdr);       
	
	skb_push(skb, tcp_header_size);
	skb_reset_transport_header(skb);

	th = tcp_hdr(skb);
	memset(th, 0, sizeof(struct tcphdr));
	th->syn = 1;
	th->ack = 1;
	TCP_ECN_make_synack(req, th);
	th->source = ireq->loc_port;
	th->dest = ireq->rmt_port;
	/* Setting of flags are superfluous here for callers (and ECE is
	 * not even correctly set)
	 */
	tcp_init_nondata_skb(skb, tcp_rsk(req)->snt_isn,
			     TCPCB_FLAG_SYN | TCPCB_FLAG_ACK);
	th->seq = htonl(TCP_SKB_CB(skb)->seq);
	th->ack_seq = htonl(tcp_rsk(req)->rcv_isn + 1);

	/* RFC1323: The window in SYN & SYN/ACK segments is never scaled. */
	th->window = htons(min(req->rcv_wnd, 65535U));
	tcp_options_write((__be32 *)(th + 1), tp, &opts, &md5_hash_location);
	th->doff = (tcp_header_size >> 2);
	TCP_INC_STATS(sock_net(sk), TCP_MIB_OUTSEGS);

#ifdef CONFIG_TCP_MD5SIG
	/* Okay, we have all we need - do the md5 hash if needed */
	if (md5) {
		tp->af_specific->calc_md5_hash(md5_hash_location,
					       md5, NULL, req, skb);
	}
#endif

	return skb;
}

/*
 * Do all connect socket setups that can be done AF independent.
 */
static void tcp_connect_init(struct sock *sk)
{
	struct dst_entry *dst = __sk_dst_get(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	__u8 rcv_wscale;

	/* We'll fix this up when we get a response from the other end.
	 * See tcp_input.c:tcp_rcv_state_process case TCP_SYN_SENT.
	 */
	tp->tcp_header_len = sizeof(struct tcphdr) +
		(sysctl_tcp_timestamps ? TCPOLEN_TSTAMP_ALIGNED : 0);

#ifdef CONFIG_TCP_MD5SIG
	if (tp->af_specific->md5_lookup(sk, sk) != NULL)
		tp->tcp_header_len += TCPOLEN_MD5SIG_ALIGNED;
#endif

	/* If user gave his TCP_MAXSEG, record it to clamp */
	if (tp->rx_opt.user_mss)
		tp->rx_opt.mss_clamp = tp->rx_opt.user_mss;
	tp->max_window = 0;
	tcp_mtup_init(sk);
	tcp_sync_mss(sk, dst_mtu(dst));

	if (!tp->window_clamp)
		tp->window_clamp = dst_metric(dst, RTAX_WINDOW);
#ifdef CONFIG_MTCP
	tp->advmss = sysctl_mptcp_mss;
	if (tp->advmss>dst_metric(dst,RTAX_ADVMSS))
		tp->mss_too_low=1;
#else
	tp->advmss = dst_metric(dst, RTAX_ADVMSS);
#endif
	if (tp->rx_opt.user_mss && tp->rx_opt.user_mss < tp->advmss)
		tp->advmss = tp->rx_opt.user_mss;

	tcp_initialize_rcv_mss(sk);

#ifdef CONFIG_MTCP
	tcp_select_initial_window(mtcp_full_space(sk),
				  tp->advmss - (tp->rx_opt.ts_recent_stamp ? tp->tcp_header_len - sizeof(struct tcphdr) : 0),
				  &tp->rcv_wnd,
				  &tp->window_clamp,
				  sysctl_tcp_window_scaling,
				  &rcv_wscale);

	mtcp_update_window_clamp(tp->mpcb);
#else
	tcp_select_initial_window(tcp_full_space(sk),
				  tp->advmss - (tp->rx_opt.ts_recent_stamp 
						? tp->tcp_header_len - 
						sizeof(struct tcphdr) : 0),
				  &tp->rcv_wnd,
				  &tp->window_clamp,
				  sysctl_tcp_window_scaling,
				  &rcv_wscale);
#endif

	tp->rx_opt.rcv_wscale = rcv_wscale;
	tp->rcv_ssthresh = tp->rcv_wnd;

	sk->sk_err = 0;
	sock_reset_flag(sk, SOCK_DONE);
	tp->snd_wnd = 0;
	tcp_init_wl(tp, 0);
	tp->snd_una = tp->write_seq;
	tp->snd_sml = tp->write_seq;
	tp->snd_up = tp->write_seq;
	tp->rcv_nxt = 0;
	tp->rcv_wup = 0;
	tp->copied_seq = 0;

	inet_csk(sk)->icsk_rto = TCP_TIMEOUT_INIT;
	inet_csk(sk)->icsk_retransmits = 0;
	tcp_clear_retrans(tp);
}

/*
 * Build a SYN and send it off.
 */
int tcp_connect(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *buff;

	tcp_connect_init(sk);

	buff = alloc_skb_fclone(MAX_TCP_HEADER + 15, sk->sk_allocation);
	if (unlikely(buff == NULL))
		return -ENOBUFS;

	/* Reserve space for headers. */
	skb_reserve(buff, MAX_TCP_HEADER);

	tp->snd_nxt = tp->write_seq;
	tcp_init_nondata_skb(buff, tp->write_seq++, TCPCB_FLAG_SYN);
	TCP_ECN_send_syn(sk, buff);

	/* Send it off. */
	TCP_SKB_CB(buff)->when = tcp_time_stamp;
	tp->retrans_stamp = TCP_SKB_CB(buff)->when;
	skb_header_release(buff);
	__tcp_add_write_queue_tail(sk, buff);
	sk->sk_wmem_queued += buff->truesize;
	sk_mem_charge(sk, buff->truesize);
	tp->packets_out += tcp_skb_pcount(buff);
	
	tcp_transmit_skb(sk, buff, 1, GFP_KERNEL);

	/* We change tp->snd_nxt after the tcp_transmit_skb() call
	 * in order to make this packet get counted in tcpOutSegs.
	 */
	tp->snd_nxt = tp->write_seq;
	tp->pushed_seq = tp->write_seq;
	TCP_INC_STATS(sock_net(sk), TCP_MIB_ACTIVEOPENS);

	/* Timer for repeating the SYN until an answer. */
	inet_csk_reset_xmit_timer(sk, ICSK_TIME_RETRANS,
				  inet_csk(sk)->icsk_rto, TCP_RTO_MAX);
	return 0;
}

/* Send out a delayed ack, the caller does the policy checking
 * to see if we should even be here.  See tcp_input.c:tcp_ack_snd_check()
 * for details.
 */
void tcp_send_delayed_ack(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	int ato = icsk->icsk_ack.ato;
	unsigned long timeout;

	if (ato > TCP_DELACK_MIN) {
		const struct tcp_sock *tp = tcp_sk(sk);
		int max_ato = HZ / 2;

		if (icsk->icsk_ack.pingpong ||
		    (icsk->icsk_ack.pending & ICSK_ACK_PUSHED))
			max_ato = TCP_DELACK_MAX;

		/* Slow path, intersegment interval is "high". */

		/* If some rtt estimate is known, use it to bound delayed ack.
		 * Do not use inet_csk(sk)->icsk_rto here, use results of rtt measurements
		 * directly.
		 */
		if (tp->srtt) {
			int rtt = max(tp->srtt >> 3, TCP_DELACK_MIN);

			if (rtt < max_ato)
				max_ato = rtt;
		}

		ato = min(ato, max_ato);
	}

	/* Stay within the limit we were given */
	timeout = jiffies + ato;

	/* Use new timeout only if there wasn't a older one earlier. */
	if (icsk->icsk_ack.pending & ICSK_ACK_TIMER) {
		/* If delack timer was blocked or is about to expire,
		 * send ACK now.
		 */
		if (icsk->icsk_ack.blocked ||
		    time_before_eq(icsk->icsk_ack.timeout, jiffies + (ato >> 2))) {
			tcp_send_ack(sk);
			return;
		}

		if (!time_before(timeout, icsk->icsk_ack.timeout))
			timeout = icsk->icsk_ack.timeout;
	}
	icsk->icsk_ack.pending |= ICSK_ACK_SCHED | ICSK_ACK_TIMER;
	icsk->icsk_ack.timeout = timeout;
	sk_reset_timer(sk, &icsk->icsk_delack_timer, timeout);
}

/* This routine sends an ack and also updates the window. */
void tcp_send_ack(struct sock *sk)
{
	struct sk_buff *buff;

	/* If we have been reset, we may not send again. */
	if (sk->sk_state == TCP_CLOSE)
		return;

	/* We are not putting this on the write queue, so
	 * tcp_transmit_skb() will set the ownership to this
	 * sock.
	 */
	buff = alloc_skb(MAX_TCP_HEADER, GFP_ATOMIC);
	if (buff == NULL) {
		inet_csk_schedule_ack(sk);
		inet_csk(sk)->icsk_ack.ato = TCP_ATO_MIN;
		inet_csk_reset_xmit_timer(sk, ICSK_TIME_DACK,
					  TCP_DELACK_MAX, TCP_RTO_MAX);
		return;
	}

	/* Reserve space for headers and prepare control bits. */
	skb_reserve(buff, MAX_TCP_HEADER);
	tcp_init_nondata_skb(buff, tcp_acceptable_seq(sk), TCPCB_FLAG_ACK);

	/* Send it off, this clears delayed acks for us. */
	TCP_SKB_CB(buff)->when = tcp_time_stamp;
	tcp_transmit_skb(sk, buff, 0, GFP_ATOMIC);
}

/* This routine sends a packet with an out of date sequence
 * number. It assumes the other end will try to ack it.
 *
 * Question: what should we make while urgent mode?
 * 4.4BSD forces sending single byte of data. We cannot send
 * out of window data, because we have SND.NXT==SND.MAX...
 *
 * Current solution: to send TWO zero-length segments in urgent mode:
 * one is with SEG.SEQ=SND.UNA to deliver urgent pointer, another is
 * out-of-date with SND.UNA-1 to probe window.
 */
static int tcp_xmit_probe_skb(struct sock *sk, int urgent)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb;

	/* We don't queue it, tcp_transmit_skb() sets ownership. */
	skb = alloc_skb(MAX_TCP_HEADER, GFP_ATOMIC);
	if (skb == NULL)
		return -1;

	/* Reserve space for headers and set control bits. */
	skb_reserve(skb, MAX_TCP_HEADER);
	/* Use a previous sequence.  This should cause the other
	 * end to send an ack.  Don't queue or clone SKB, just
	 * send it.
	 */
	tcp_init_nondata_skb(skb, tp->snd_una - !urgent, TCPCB_FLAG_ACK);
	TCP_SKB_CB(skb)->when = tcp_time_stamp;
	return tcp_transmit_skb(sk, skb, 0, GFP_ATOMIC);
}

int tcp_write_wakeup(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb;

	if (sk->sk_state == TCP_CLOSE)
		return -1;

	if ((skb = tcp_send_head(sk)) != NULL &&
	    before((tp->mpc)?TCP_SKB_CB(skb)->data_seq:
		   TCP_SKB_CB(skb)->seq, tcp_wnd_end(tp,tp->mpc))) {
		int err;
		unsigned int mss = tcp_current_mss(sk, 0);
		unsigned int seg_size = tcp_wnd_end(tp,tp->mpc) - 
			((tp->mpc)?TCP_SKB_CB(skb)->data_seq:
			 TCP_SKB_CB(skb)->seq);

		if (before(tp->pushed_seq, TCP_SKB_CB(skb)->end_seq))
			tp->pushed_seq = TCP_SKB_CB(skb)->end_seq;

		/* We are probing the opening of a window
		 * but the window size is != 0
		 * must have been a result SWS avoidance ( sender )
		 */
		if (seg_size < TCP_SKB_CB(skb)->end_seq - TCP_SKB_CB(skb)->seq ||
		    skb->len > mss) {
			seg_size = min(seg_size, mss);
			TCP_SKB_CB(skb)->flags |= TCPCB_FLAG_PSH;
			if (tcp_fragment(sk, skb, seg_size, mss))
				return -1;
		} else if (!tcp_skb_pcount(skb))
			tcp_set_skb_tso_segs(sk, skb, mss);

		TCP_SKB_CB(skb)->flags |= TCPCB_FLAG_PSH;
		TCP_SKB_CB(skb)->when = tcp_time_stamp;
		err = tcp_transmit_skb(sk, skb, 1, GFP_ATOMIC);
		if (!err)
			tcp_event_new_data_sent(sk, skb);
		return err;
	} else {
		if (between(tp->snd_up, tp->snd_una + 1, tp->snd_una + 0xFFFF))
			tcp_xmit_probe_skb(sk, 1);
		return tcp_xmit_probe_skb(sk, 0);
	}
}

/* A window probe timeout has occurred.  If window is not closed send
 * a partial packet else a zero probe.
 */
void tcp_send_probe0(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	int err;

	err = tcp_write_wakeup(sk);

	if (tp->packets_out || !tcp_send_head(sk)) {
		/* Cancel probe timer, if it is not required. */
		icsk->icsk_probes_out = 0;
		icsk->icsk_backoff = 0;
		return;
	}

	if (err <= 0) {
		if (icsk->icsk_backoff < sysctl_tcp_retries2)
			icsk->icsk_backoff++;
		icsk->icsk_probes_out++;
		inet_csk_reset_xmit_timer(sk, ICSK_TIME_PROBE0,
					  min(icsk->icsk_rto << icsk->icsk_backoff, TCP_RTO_MAX),
					  TCP_RTO_MAX);
	} else {
		/* If packet was not sent due to local congestion,
		 * do not backoff and do not remember icsk_probes_out.
		 * Let local senders to fight for local resources.
		 *
		 * Use accumulated backoff yet.
		 */
		if (!icsk->icsk_probes_out)
			icsk->icsk_probes_out = 1;
		inet_csk_reset_xmit_timer(sk, ICSK_TIME_PROBE0,
					  min(icsk->icsk_rto << icsk->icsk_backoff,
					      TCP_RESOURCE_PROBE_INTERVAL),
					  TCP_RTO_MAX);
	}
}

EXPORT_SYMBOL(tcp_select_initial_window);
EXPORT_SYMBOL(tcp_connect);
EXPORT_SYMBOL(tcp_make_synack);
EXPORT_SYMBOL(tcp_simple_retransmit);
EXPORT_SYMBOL(tcp_sync_mss);
EXPORT_SYMBOL(tcp_mtup_init);
