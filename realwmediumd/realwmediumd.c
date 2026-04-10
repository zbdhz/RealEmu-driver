#include <netlink/netlink.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/family.h>
#include <stdint.h>
#include <getopt.h>
#include <signal.h>
#include <event.h>
#include <math.h>
#include <sys/timerfd.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>

#include "ieee80211.h"
#include "realwmediumd.h"
#include "config.h"

struct sockaddr_in serverAddr, clientAddr;
socklen_t len;
void* out_buf;
char in_buf[PAGE_SIZE];
static bool is_ap = true;

static inline int div_round(int a, int b)
{
	return (a + b - 1) / b;
}

static inline int pkt_duration(struct realwmediumd *ctx, int len, int rate)
{
	/* preamble + signal + t_sym * n_sym, rate in 100 kbps */
	return 16 + 4 + 4 * div_round((16 + 8 * len + 6) * 10, 4 * rate);
}

int w_logf(struct realwmediumd *ctx, u8 level, const char *format, ...)
{
	va_list(args);
	va_start(args, format);
	if (ctx->log_lvl >= level) {
		return vprintf(format, args);
	}
	return -1;
}

int w_flogf(struct realwmediumd *ctx, u8 level, FILE *stream, const char *format, ...)
{
	va_list(args);
	va_start(args, format);
	if (ctx->log_lvl >= level) {
		return vfprintf(stream, format, args);
	}
	return -1;
}

static void wqueue_init(struct wqueue *wqueue, int cw_min, int cw_max)
{
	INIT_LIST_HEAD(&wqueue->frames);
	wqueue->cw_min = cw_min;
	wqueue->cw_max = cw_max;
}

void station_init_queues(struct station *station)
{
	wqueue_init(&station->queues[IEEE80211_AC_BK], 15, 1023);
	wqueue_init(&station->queues[IEEE80211_AC_BE], 15, 1023);
	wqueue_init(&station->queues[IEEE80211_AC_VI], 7, 15);
	wqueue_init(&station->queues[IEEE80211_AC_VO], 3, 7);
}

bool timespec_before(struct timespec *t1, struct timespec *t2)
{
	return t1->tv_sec < t2->tv_sec ||
	       (t1->tv_sec == t2->tv_sec && t1->tv_nsec < t2->tv_nsec);
}

void timespec_add_usec(struct timespec *t, int usec)
{
	t->tv_nsec += usec * 1000;
	if (t->tv_nsec >= 1000000000) {
		t->tv_sec++;
		t->tv_nsec -= 1000000000;
	}
}

// a - b = c
static int timespec_sub(struct timespec *a, struct timespec *b,
			struct timespec *c)
{
	c->tv_sec = a->tv_sec - b->tv_sec;

	if (a->tv_nsec < b->tv_nsec) {
		c->tv_sec--;
		c->tv_nsec = 1000000000 + a->tv_nsec - b->tv_nsec;
	} else {
		c->tv_nsec = a->tv_nsec - b->tv_nsec;
	}

	return 0;
}

void rearm_timer(struct realwmediumd *ctx)
{
	struct timespec min_expires;
	struct itimerspec expires;
	struct station *station;
	struct frame *frame;
	int i;

	bool set_min_expires = false;

	/*
	 * Iterate over all the interfaces to find the next frame that
	 * will be delivered, and set the timerfd accordingly.
	 */
	list_for_each_entry(station, &ctx->stations, list) {
		for (i = 0; i < IEEE80211_NUM_ACS; i++) {
			frame = list_first_entry_or_null(&station->queues[i].frames,
							 struct frame, list);

			if (frame && (!set_min_expires ||
				      timespec_before(&frame->expires,
						      &min_expires))) {
				set_min_expires = true;
				min_expires = frame->expires;
			}
		}
	}

	if (set_min_expires) {
		memset(&expires, 0, sizeof(expires));
		expires.it_value = min_expires;
		timerfd_settime(ctx->timerfd, TFD_TIMER_ABSTIME, &expires,
				NULL);
	}
}

static inline bool frame_has_a4(struct frame *frame)
{
	struct ieee80211_hdr *hdr = (void *)frame->data;

	return (hdr->frame_control[1] & (FCTL_TODS | FCTL_FROMDS)) ==
		(FCTL_TODS | FCTL_FROMDS);
}

static inline bool frame_is_mgmt(struct frame *frame)
{
	struct ieee80211_hdr *hdr = (void *)frame->data;

	return (hdr->frame_control[0] & FCTL_FTYPE) == FTYPE_MGMT;
}

static inline bool frame_is_data(struct frame *frame)
{
	struct ieee80211_hdr *hdr = (void *)frame->data;

	return (hdr->frame_control[0] & FCTL_FTYPE) == FTYPE_DATA;
}

static inline bool frame_is_data_qos(struct frame *frame)
{
	struct ieee80211_hdr *hdr = (void *)frame->data;

	return (hdr->frame_control[0] & (FCTL_FTYPE | STYPE_QOS_DATA)) ==
		(FTYPE_DATA | STYPE_QOS_DATA);
}

static inline u8 *frame_get_qos_ctl(struct frame *frame)
{
	struct ieee80211_hdr *hdr = (void *)frame->data;

	if (frame_has_a4(frame))
		return (u8 *)hdr + 30;
	else
		return (u8 *)hdr + 24;
}

static enum ieee80211_ac_number frame_select_queue_80211(struct frame *frame)
{
	u8 *p;
	int priority;

	if (!frame_is_data(frame))
		return IEEE80211_AC_VO;

	if (!frame_is_data_qos(frame))
		return IEEE80211_AC_BE;

	p = frame_get_qos_ctl(frame);
	priority = *p & QOS_CTL_TAG1D_MASK;

	return ieee802_1d_to_ac[priority];
}

static struct station *get_station_by_addr(struct realwmediumd *ctx, u8 *addr)
{
	struct station *station;

	list_for_each_entry(station, &ctx->stations, list) {
		if (memcmp(station->addr, addr, ETH_ALEN) == 0)
			return station;
	}
	return NULL;
}

//重写该函数
//对下接口改成RealEmu的硬件接口，保留计算时间等问题，下发前检查底层队列是否满
//设置超时时间的逻辑需要修正
//需要把现在的帧结构转化成RealEmu的内部逻辑
void queue_frame(struct realwmediumd *ctx, struct station *station,
		 struct frame *frame)
{
	/* 函数定义：接收 wmediumd 上下文、发送站点和帧结构 */
	struct ieee80211_hdr *hdr = (void *)frame->data;	/* hdr：802.11 帧头 */
	u8 *dest = hdr->addr1;					/* dest：目标 MAC 地址 */
	struct timespec now, target;				/* now / target：时间相关变量 */
	struct wqueue *queue;					/* queue：帧队列 */
	struct frame *tail;					/* tail：队列末尾帧指针 */
	struct station *tmpsta, *deststa;			/* deststa：目标站点 */
	int send_time;						/* send_time：发送时间 */
	int cw;							/* cw：竞争窗口大小 */
	double error_prob;					/* error_prob：错误概率 */
	bool is_acked = false;					/* is_acked：是否收到确认 */
	bool noack = false;					/* noack：是否不需要确认 */
	int i, j;						/* 其他辅助变量 */
	int rate_idx;
	int ac;

	/* TODO configure phy parameters */
	/* 物理层参数 */
	int slot_time = 9;	/* 时隙时间（9微秒） */
	int sifs = 16;		/* 短帧间间隔（16微秒） */
	int difs = 2 * slot_time + sifs;	/* 分布式帧间间隔（2×slot_time + sifs） */

	int retries = 0;  /* 重试次数计数器 */

	clock_gettime(CLOCK_MONOTONIC, &now);  //获取当前时间 ：使用单调时钟获取当前时间，用于计算帧的过期时间
	//算 ACK 时间 ：计算 ACK 帧的传输时间（14字节）加上 SIFS 时间
	int ack_time_usec = pkt_duration(ctx, 14, index_to_rate(0, frame->freq)) +
			sifs;

	/*
	 * To determine a frame's expiration time, we compute the
	 * number of retries we might have to make due to radio conditions
	 * or contention, and add backoff time accordingly.  To that, we
	 * add the expiration time of the previous frame in the queue.
	 */

	// 择队列 ：根据 802.11 帧类型选择适当的访问类别（AC）队列
	ac = frame_select_queue_80211(frame);
	queue = &station->queues[ac];

	/* try to "send" this frame at each of the rates in the rateset */
	// 初始化发送时间 ：设置初始发送时间为0，竞争窗口为最小值
	send_time = 0;
	cw = queue->cw_min;

	int snr = SNR_DEFAULT;

	if (is_multicast_ether_addr(dest)) {
		deststa = NULL;
	} else {
		deststa = get_station_by_addr(ctx, dest);
		if (deststa) {
            w_logf(ctx, LOG_DEBUG, "Packet from " MAC_FMT "(%d|%s) to " MAC_FMT "(%d|%s)\n",
                   MAC_ARGS(station->addr), station->index, station->isap ? "AP" : "Sta",
                   MAC_ARGS(deststa->addr), deststa->index, deststa->isap ? "AP" : "Sta");
            detect_mediums(ctx,station,deststa);
			snr = ctx->get_link_snr(ctx, station, deststa) -
				get_signal_offset_by_interference(ctx,
					station->index, deststa->index);
			snr += ctx->get_fading_signal(ctx);
		}
	}
	frame->signal = snr + NOISE_LEVEL;

	noack = frame_is_mgmt(frame) || is_multicast_ether_addr(dest);
	double choice = -3.14;

	if (use_fixed_random_value(ctx))
		choice = drand48();

	for (i = 0; i < frame->tx_rates_count && !is_acked; i++) {

		rate_idx = frame->tx_rates[i].idx;

		/* no more rates in MRR */
		if (rate_idx < 0)
			break;

		error_prob = ctx->get_error_prob(ctx, snr, rate_idx,
						 frame->freq, frame->data_len,
						 station, deststa);
		for (j = 0; j < frame->tx_rates[i].count; j++) {
			send_time += difs + pkt_duration(ctx, frame->data_len,
				index_to_rate(rate_idx, frame->freq));

			retries++;

			/* skip ack/backoff/retries for noack frames */
			if (noack) {
				is_acked = true;
				break;
			}

			/* TODO TXOPs */

			/* backoff */
			if (j > 0) {
				send_time += (cw * slot_time) / 2;
				cw = (cw << 1) + 1;
				if (cw > queue->cw_max)
					cw = queue->cw_max;
			}
			if (!use_fixed_random_value(ctx))
				choice = drand48();
			if (choice > error_prob) {
				is_acked = true;
				break;
			}
			send_time += ack_time_usec;
		}
	}
	if (is_acked) {
		frame->tx_rates[i-1].count = j + 1;
		for (; i < frame->tx_rates_count; i++) {
			frame->tx_rates[i].idx = -1;
			frame->tx_rates[i].count = -1;
		}
		frame->flags |= HWSIM_TX_STAT_ACK;
	}

	/*
	 * delivery time starts after any equal or higher prio frame
	 * (or now, if none).
	 */
	target = now;
	// 计算帧的过期时间 ：
	// - 初始化为当前时间
	// - 遍历所有站点，找到同一介质上的站点
	// - 检查这些站点的同优先级或更高优先级队列
	// - 如果队列中有帧，取最后一个帧的过期时间作为当前帧的开始时间
    w_logf(ctx, LOG_DEBUG, "Sta " MAC_FMT " medium is #%d\n", MAC_ARGS(station->addr), station->medium_id);
    list_for_each_entry(tmpsta, &ctx->stations, list) {
        if (station->medium_id == tmpsta->medium_id) {
            w_logf(ctx, LOG_DEBUG, "Sta " MAC_FMT " medium is also #%d\n", MAC_ARGS(tmpsta->addr),
                   tmpsta->medium_id);
            for (i = 0; i <= ac; i++) {
                tail = list_last_entry_or_null(&tmpsta->queues[i].frames,
                                               struct frame, list);
                if (tail && timespec_before(&target, &tail->expires))
                    target = tail->expires;
            }
        } else {
            w_logf(ctx, LOG_DEBUG, "Sta " MAC_FMT " medium is not #%d, it is #%d\n", MAC_ARGS(tmpsta->addr),
                   station->medium_id, tmpsta->medium_id);
        }
    }

	timespec_add_usec(&target, send_time);

	frame->duration = send_time;
	frame->expires = target;
	list_add_tail(&frame->list, &queue->frames);
	rearm_timer(ctx);
}


/*
 * Report transmit status to the kernel.
 */
static int send_tx_info_frame_nl(struct realwmediumd *ctx, struct frame *frame)
{
	struct nl_sock *sock = ctx->sock;
	struct nl_msg *msg;
	int ret;

	msg = nlmsg_alloc();
	if (!msg) {
		w_logf(ctx, LOG_ERR, "Error allocating new message MSG!\n");
		return -1;
	}

	if (genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, ctx->family_id,
			0, NLM_F_REQUEST, HWSIM_CMD_TX_INFO_FRAME,
			VERSION_NR) == NULL) {
		w_logf(ctx, LOG_ERR, "%s: genlmsg_put failed\n", __func__);
		ret = -1;
		goto out;
	}

	if (nla_put(msg, HWSIM_ATTR_ADDR_TRANSMITTER, ETH_ALEN,
		    frame->sender->hwaddr) ||
	    nla_put_u32(msg, HWSIM_ATTR_FLAGS, frame->flags) ||
	    nla_put_u32(msg, HWSIM_ATTR_SIGNAL, frame->signal) ||
	    nla_put(msg, HWSIM_ATTR_TX_INFO,
		    frame->tx_rates_count * sizeof(struct hwsim_tx_rate),
		    frame->tx_rates) ||
	    nla_put_u64(msg, HWSIM_ATTR_COOKIE, frame->cookie)) {
			w_logf(ctx, LOG_ERR, "%s: Failed to fill a payload\n", __func__);
			ret = -1;
			goto out;
	}

	ret = nl_send_auto_complete(sock, msg);
	if (ret < 0) {
		w_logf(ctx, LOG_ERR, "%s: nl_send_auto failed\n", __func__);
		ret = -1;
		goto out;
	}
	ret = 0;

out:
	nlmsg_free(msg);
	return ret;
}

/*
 * Report transmit status to the transmitter.
 */
static int send_tx_info_frame(struct realwmediumd *ctx, struct frame *frame)
{
	if (ctx->op_mode == LOCAL)
		return send_tx_info_frame_nl(ctx, frame);
	
	int numBytes, ret;

	if (is_ap){
		numBytes = sendto(ctx->net_sock, frame, sizeof(*frame), 0, (struct sockaddr*)&clientAddr, len);
        if (numBytes < 0){
			w_flogf(ctx, LOG_ERR, stderr, "Failed to send xmit info to station: %s", strerror(errno));
			ret = -1;
			goto out;
		}
	}
	else{
		numBytes = sendto(ctx->net_sock, frame, sizeof(*frame), 0, (struct sockaddr*)&serverAddr, len);
        if (numBytes < 0){
			w_flogf(ctx, LOG_ERR, stderr, "Failed to send xmit info to AP station: %s", strerror(errno));
			ret = -1;
			goto out;
		}
	}
	ret = 0;

out:
	return ret;
}

/*
 * Send a data frame to the kernel for reception at a specific radio.
 */
// send_cloned_frame_msg 函数负责将克隆的帧通过 netlink 套接字发送给内核的 mac80211_hwsim 模块
// 模拟帧在无线介质上的传输。
int send_cloned_frame_msg(struct realwmediumd *ctx, struct station *dst,
			  u8 *data, int data_len, int rate_idx, int signal,
			  int freq)
{
	struct nl_msg *msg;
	struct nl_sock *sock = ctx->sock;
	int ret;

	msg = nlmsg_alloc();
	if (!msg) {
		w_logf(ctx, LOG_ERR, "Error allocating new message MSG!\n");
		return -1;
	}

	if (genlmsg_put(msg, NL_AUTO_PID, NL_AUTO_SEQ, ctx->family_id,
			0, NLM_F_REQUEST, HWSIM_CMD_FRAME,
			VERSION_NR) == NULL) {
		w_logf(ctx, LOG_ERR, "%s: genlmsg_put failed\n", __func__);
		ret = -1;
		goto out;
	}

	if (nla_put(msg, HWSIM_ATTR_ADDR_RECEIVER, ETH_ALEN,
		    dst->hwaddr) ||
	    nla_put(msg, HWSIM_ATTR_FRAME, data_len, data) ||
	    nla_put_u32(msg, HWSIM_ATTR_RX_RATE, rate_idx) ||
	    nla_put_u32(msg, HWSIM_ATTR_FREQ, freq) ||
	    nla_put_u32(msg, HWSIM_ATTR_SIGNAL, signal)) {
			w_logf(ctx, LOG_ERR, "%s: Failed to fill a payload\n", __func__);
			ret = -1;
			goto out;
	}

	w_logf(ctx, LOG_DEBUG, "cloned msg dest " MAC_FMT " (radio: " MAC_FMT ") len %d\n",
		   MAC_ARGS(dst->addr), MAC_ARGS(dst->hwaddr), data_len);

	ret = nl_send_auto_complete(sock, msg);
	if (ret < 0) {
		w_logf(ctx, LOG_ERR, "%s: nl_send_auto failed\n", __func__);
		ret = -1;
		goto out;
	}
	ret = 0;

out:
	nlmsg_free(msg);
	return ret;
}

void deliver_frame(struct realwmediumd *ctx, struct frame *frame)
{
	struct ieee80211_hdr *hdr = (void *) frame->data;
	struct station *station;
	u8 *dest = hdr->addr1;
	u8 *src = frame->sender->addr;

	if (frame->flags & HWSIM_TX_STAT_ACK) {
		/* rx the frame on the dest interface */
		//仅仅传输带有ACK标志的帧
		list_for_each_entry(station, &ctx->stations, list) {
			if (memcmp(src, station->addr, ETH_ALEN) == 0)
				continue;

			int rate_idx;
			if (is_multicast_ether_addr(dest)) {
				// 组播帧处理 ：
				// - 对每个站点单独计算接收情况
				// - 计算 SNR（信号噪声比）和信号强度
				// - 如果信号强度低于 CCA 阈值，跳过
				// - 设置干扰持续时间
				// - 减去干扰导致的信号偏移
				// - 计算错误概率
				// - 根据错误概率决定是否丢弃帧
				// - 如果接收成功，发送克隆帧到该站点
				int snr, signal;
				double error_prob;
				/*
				 * we may or may not receive this based on
				 * reverse link from sender -- check for
				 * each receiver.
				 */
				snr = ctx->get_link_snr(ctx, frame->sender,
							station);
				snr += ctx->get_fading_signal(ctx);
				signal = snr + NOISE_LEVEL;
				if (signal < CCA_THRESHOLD)
					continue;

				if (set_interference_duration(ctx,
					frame->sender->index, frame->duration,
					signal))
					continue;

				snr -= get_signal_offset_by_interference(ctx,
					frame->sender->index, station->index);
				rate_idx = frame->tx_rates[0].idx;
				error_prob = ctx->get_error_prob(ctx,
					(double)snr, rate_idx, frame->freq,
					frame->data_len, frame->sender,
					station);

				if (drand48() <= error_prob) {
					w_logf(ctx, LOG_INFO, "Dropped mcast from "
						   MAC_FMT " to " MAC_FMT " at receiver\n",
						   MAC_ARGS(src), MAC_ARGS(station->addr));
					continue;
				}

				send_cloned_frame_msg(ctx, station,
						      frame->data,
						      frame->data_len,
						      rate_idx, signal,
						      frame->freq);
			} else if (memcmp(dest, station->addr, ETH_ALEN) == 0) {
				// 单播帧处理 ：
				// - 找到目标地址对应的站点
				// - 设置干扰持续时间
				// - 发送克隆帧到目标站点
				if (set_interference_duration(ctx,
					frame->sender->index, frame->duration,
					frame->signal))
					continue;
				rate_idx = frame->tx_rates[0].idx;
				send_cloned_frame_msg(ctx, station,
						      frame->data,
						      frame->data_len,
						      rate_idx, frame->signal,
						      frame->freq);
  			}
		}
	} else
		// - 无 ACK 帧处理 ：
		// - 如果帧没有 ACK 标志（传输失败），只设置干扰持续时间
		set_interference_duration(ctx, frame->sender->index,
					  frame->duration, frame->signal);

	send_tx_info_frame(ctx, frame);

	free(frame);
}

void deliver_expired_frames_queue(struct realwmediumd *ctx,
				  struct list_head *queue,
				  struct timespec *now)
{
	struct frame *frame, *tmp;

	list_for_each_entry_safe(frame, tmp, queue, list) {
		if (timespec_before(&frame->expires, now)) {
			list_del(&frame->list);  //从队列中删除过期帧
			deliver_frame(ctx, frame);  //传输帧
		} else {
			break;
		}
	}
}
//考虑广播帧和直接传输帧的处理
//源程序中的逻辑在于处理过期的帧传输。
//后续考虑有定时器和底层的双重触发
//定时器用于往回的数据帧的处理（广播帧的聚合）
void deliver_expired_frames(struct realwmediumd *ctx)
{ 
	struct timespec now;        /* 当前时间 */
	struct timespec _diff;      /* 时间差 */
	struct station *station;    /* 站点指针 */
	struct list_head *l, *rand_it, *rand_start; /* 链表遍历指针 */
	int i, j, duration;         /* 循环计数器和时间变量 */
	int sta1_medium_id;         /* 站点的介质 ID */
	clock_gettime(CLOCK_MONOTONIC, &now);  //获取当前时间 ：使用单调时钟获取当前时间，用于判断帧是否过期
	
	int rand_start_cnt = rand() % ctx->num_stas;
	
	// Find the randomized starting point in the list
	rand_start = &ctx->stations;
	for (i = 0; i < rand_start_cnt; i++) {
		rand_start = rand_start->next;
	}

	// 遍历站点并处理过期帧 ：(有一种取巧的办法，默认所有的数据包进战都是不过期的，搜索未过期的站点进行数据包的匹配)
	// - 从随机起始点开始遍历所有站点
	// - 跳过链表头
	// - 使用 list_entry 获取站点结构体
	// - 计算每个访问类别（AC）队列中的帧数量
	// - 记录详细的队列状态日志
	// - 对每个队列调用 deliver_expired_frames_queue 处理过期帧
	list_for_each(rand_it, rand_start) {
		//skip if we iterate over head
		if(rand_it == &ctx->stations)
			continue;
		station = list_entry(rand_it, struct station, list);
		int q_ct[IEEE80211_NUM_ACS] = {};
		for (i = 0; i < IEEE80211_NUM_ACS; i++) {
			list_for_each(l, &station->queues[i].frames) {
				q_ct[i]++;
			}
		}
		w_logf(ctx, LOG_DEBUG, "[" TIME_FMT "] Station " MAC_FMT
					   " BK %d BE %d VI %d VO %d\n",
			   TIME_ARGS(&now), MAC_ARGS(station->addr),
			   q_ct[IEEE80211_AC_BK], q_ct[IEEE80211_AC_BE],
			   q_ct[IEEE80211_AC_VI], q_ct[IEEE80211_AC_VO]);

		for (i = 0; i < IEEE80211_NUM_ACS; i++)
			deliver_expired_frames_queue(ctx, &station->queues[i].frames, &now);
	}
	w_logf(ctx, LOG_DEBUG, "\n\n");

	// if (!ctx->intf)
	// 	return;

	// timespec_sub(&now, &ctx->intf_updated, &_diff);
	// duration = (_diff.tv_sec * 1000000) + (_diff.tv_nsec / 1000);
	// if (duration < 10000) // calc per 10 msec
	// 	return;

	// update interference（需要移除这部分的逻辑）
	// for (i = 0; i < ctx->num_stas; i++){
    //     sta1_medium_id = ctx->sta_array[i]->medium_id;
    //     for (j = 0; j < ctx->num_stas; j++) {
    //         if (i == j)
    //             continue;
    //         if (sta1_medium_id != ctx->sta_array[j]->medium_id)
    //             continue;
    //         // probability is used for next calc
    //         ctx->intf[i * ctx->num_stas + j].prob_col =
    //                 ctx->intf[i * ctx->num_stas + j].duration /
    //                 (double)duration;
    //         ctx->intf[i * ctx->num_stas + j].duration = 0;
    //     }
    // }

	clock_gettime(CLOCK_MONOTONIC, &ctx->intf_updated);
}


static int process_recvd_data(struct realwmediumd *ctx, struct nlmsghdr *nlh)
{
	struct nlattr *attrs[HWSIM_ATTR_MAX+1];  //存储 netlink 属性的数组
	/* generic netlink header*/
	struct genlmsghdr *gnlh = nlmsg_data(nlh);  //获取 netlink 消息的数据部分，即 genlmsghdr 结构体的指针

	struct station *sender; //发送帧的站点
	struct frame *frame; //无线帧结构
	struct ieee80211_hdr *hdr;  //802.11 帧头
	u8 *src;  //源 MAC 地址

	if (gnlh->cmd == HWSIM_CMD_FRAME) {
		// pthread_rwlock_rdlock(&snr_lock);
		/* we get the attributes*/
		genlmsg_parse(nlh, 0, attrs, HWSIM_ATTR_MAX, NULL);
		if (attrs[HWSIM_ATTR_ADDR_TRANSMITTER]) {
			u8 *hwaddr = (u8 *)nla_data(attrs[HWSIM_ATTR_ADDR_TRANSMITTER]);

			unsigned int data_len =
				nla_len(attrs[HWSIM_ATTR_FRAME]);
			char *data = (char *)nla_data(attrs[HWSIM_ATTR_FRAME]);
			unsigned int flags =
				nla_get_u32(attrs[HWSIM_ATTR_FLAGS]);
			unsigned int tx_rates_len =
				nla_len(attrs[HWSIM_ATTR_TX_INFO]);
			struct hwsim_tx_rate *tx_rates =
				(struct hwsim_tx_rate *)
				nla_data(attrs[HWSIM_ATTR_TX_INFO]);
			u64 cookie = nla_get_u64(attrs[HWSIM_ATTR_COOKIE]);
			u32 freq;
			freq = attrs[HWSIM_ATTR_FREQ] ?
					nla_get_u32(attrs[HWSIM_ATTR_FREQ]) : 2412;

			hdr = (struct ieee80211_hdr *)data;
			src = hdr->addr2;
			
			w_logf(ctx, LOG_DEBUG, "f: %02x%02x d: %02x%02x ",
					(u32)hdr->frame_control[0], (u32)hdr->frame_control[1], (u32)hdr->duration_id[0], (u32)hdr->duration_id[1]);
			
			if (data_len < 6 + 6 + 4)
				goto out;

			sender = get_station_by_addr(ctx, src);
			if (!sender) {
				w_flogf(ctx, LOG_ERR, stderr, "Unable to find sender station " MAC_FMT "\n", MAC_ARGS(src));
				goto out;
			}
			memcpy(sender->hwaddr, hwaddr, ETH_ALEN);

			frame = malloc(sizeof(*frame) + data_len);
			if (!frame)
				goto out;

			memcpy(frame->data, data, data_len);
			frame->data_len = data_len;
			frame->flags = flags;
			frame->cookie = cookie;
			frame->freq = freq;
			frame->sender = sender;
			sender->freq = freq;
			frame->tx_rates_count =
				tx_rates_len / sizeof(struct hwsim_tx_rate);
			memcpy(frame->tx_rates, tx_rates,
			       min(tx_rates_len, sizeof(frame->tx_rates)));
			
			w_logf(ctx, LOG_DEBUG, "a1: " MAC_FMT " a2: " MAC_FMT " a3: " MAC_FMT " sq: %02x%02x r: " MAC_FMT" len: %d cookie: %lld\n", 
					MAC_ARGS(hdr->addr1), MAC_ARGS(hdr->addr2), MAC_ARGS(hdr->addr3), (u32)hdr->seq_ctrl[0], (u32)hdr->seq_ctrl[1], 
					MAC_ARGS(frame->sender->hwaddr), data_len, cookie);
			
			queue_frame(ctx, sender, frame);
		}
out:
		// pthread_rwlock_unlock(&snr_lock);
		return 0;

	}
	return 0;
}

/*
 * Handle events from the kernel.  Process CMD_FRAME events and queue them
 * for later delivery with the scheduler.
 * 说明该函数的作用是处理来自内核的事件，特别是处理 CMD_FRAME 事件并将它们排队以便稍后由调度器传递
 */
static int process_messages_cb(struct nl_msg *msg, void *arg)
{
	//  获取消息头 ：使用 nlmsg_hdr 函数从 netlink 消息中提取消息头
	//  作用 ：消息头包含消息的类型、长度等信息，是处理消息的起点
	struct nlmsghdr *nlh = nlmsg_hdr(msg);
	//  转换参数 ：将通用参数指针转换为 realwmediumd 上下文指针
	//  作用 ：使函数能够访问 realwmediumd 的全局状态和配置
	struct realwmediumd* ctx = (struct realwmediumd*)arg;
	//  本地模式处理 ：如果 wmediumd 运行在本地模式（LOCAL），则调用 process_recvd_data 函数处理接收到的数据
	if (ctx->op_mode == LOCAL)
		return process_recvd_data(ctx, nlh);
	// 由此开始不再解释
	// 	int numBytes, ret;

	// 	out_buf = malloc(nlh->nlmsg_len);
	// 	memcpy(out_buf, nlh, nlh->nlmsg_len);

	// 	if (is_ap){
	// 		numBytes = sendto(ctx->net_sock, out_buf, nlh->nlmsg_len, 0, (struct sockaddr*)&clientAddr, len);
	//         if (numBytes < 0){
	// 			w_flogf(ctx, LOG_ERR, stderr, "Failed to send data to station: %s", strerror(errno));
	// 			ret = -1;
	// 			goto out;
	// 		}
	// 	}
	// 	else{
	// 		numBytes = sendto(ctx->net_sock, out_buf, nlh->nlmsg_len, 0, (struct sockaddr*)&serverAddr, len);
	//         if (numBytes < 0){
	// 			w_flogf(ctx, LOG_ERR, stderr, "Failed to send data to AP station: %s", strerror(errno));
	// 			ret = -1;
	// 			goto out;
	// 		}
	// 	}
	// 	ret = 0;
	// 	struct frame* tx_frame = construct_tx_info_frame(ctx, nlh);
	// 	if (tx_frame != NULL)
	// 		list_add_tail(&tx_frame->list, &ctx->pending_txinfo_frames);

	// out:
	// 	free(out_buf);
	// 	return ret;
}



/*
 * Setup netlink socket and callbacks.
 * 初始化 netlink 通信通道，连接到内核的 MAC80211_HWSIM 模块
 * 这是 realwmediumd 作为无线介质模拟器的核心功能之一，用于接收和发送 802.11 帧
 */
static int init_netlink(struct realwmediumd *ctx)
{
	struct nl_sock *sock;  //netlink 套接字指针，用于与内核通信
	int ret;  //整型变量，用于存储函数调用的返回值

	ctx->cb = nl_cb_alloc(NL_CB_CUSTOM);  //nl_cb_alloc(NL_CB_CUSTOM) ：分配自定义的 netlink 回调结构，用于处理不同类型的 netlink 消息
	if (!ctx->cb) {
		w_logf(ctx, LOG_ERR, "Error allocating netlink callbacks\n");
		return -1;
	}

	sock = nl_socket_alloc_cb(ctx->cb);  //nl_socket_alloc_cb(ctx->cb) ：使用之前分配的回调结构分配 netlink 套接字
	if (!sock) {
		w_logf(ctx, LOG_ERR, "Error allocating netlink socket\n");
		return -1;
	}

	ctx->sock = sock;

	ret = genl_connect(sock);  //genl_connect(sock) ：连接到通用 netlink 子系统，建立与内核的通信通道
	if (ret < 0) {
		w_logf(ctx, LOG_ERR, "Error connecting netlink socket ret=%d\n", ret);
		return -1;
	}

	//genl_ctrl_resolve(sock, "MAC80211_HWSIM") ：解析 "MAC80211_HWSIM" 家族的 ID，用于后续的通信
	ctx->family_id = genl_ctrl_resolve(sock, "MAC80211_HWSIM");
	if (ctx->family_id < 0) {
		w_logf(ctx, LOG_ERR, "Family MAC80211_HWSIM not registered\n");
		return -1;
	}

    //nl_cb_set(...) ：设置输入消息的回调函数 process_messages_cb ，用于处理从内核收到的消息
    //nl_cb_err(...) ：设置错误处理的回调函数 nl_err_cb ，用于处理通信过程中的错误
	nl_cb_set(ctx->cb, NL_CB_MSG_IN, NL_CB_CUSTOM, process_messages_cb, ctx);
	nl_cb_err(ctx->cb, NL_CB_CUSTOM, nl_err_cb, ctx);

	return 0;
}




/*
 *	Print the CLI help
 */
void print_help(int exval)
{
	printf("realwmediumd v%s - a fpga-based real wireless medium emulator\n", VERSION_STR);
	printf("realwmediumd [-h] [-V] [-s] [-l LOG_LVL] [-x FILE] -c FILE\n\n");

	printf("  -h              print this help and exit\n");
	printf("  -V              print version and exit\n\n");

	printf("  -l LOG_LVL      set the logging level\n");
	printf("                  LOG_LVL: RFC 5424 severity, values 0 - 7\n");
	printf("                  >= 3: errors are logged\n");
	printf("                  >= 5: startup msgs are logged\n");
	printf("                  >= 6: dropped packets are logged (default)\n");
	printf("                  == 7: all packets will be logged\n");
	printf("  -c FILE         set input config file\n");
	printf("  -x FILE         set input PER file\n");
	printf("  -s              start the server on a socket\n");
	printf("  -d              use the dynamic complex mode\n");
	printf("                  (server only with matrices for each connection)\n");

	exit(exval);
}

//该函数的上层调用后续需要修改，让底层有待处理数据时触发该函数
static void timer_cb(int fd, short what, void *data)
{
	struct realwmediumd *ctx = data;
	uint64_t u;

	// pthread_rwlock_rdlock(&snr_lock);
	read(fd, &u, sizeof(u));
	ctx->move_stations(ctx);
	deliver_expired_frames(ctx);
	rearm_timer(ctx);
	// pthread_rwlock_unlock(&snr_lock);
}


int main(int argc, char *argv[])
{
	int opt;//命令行参数
	struct realwmediumd ctx;  //上下文结构体，用于存储程序状态和配置
    char *config_file = NULL;  //配置文件路径
	char *per_file = NULL;  //PER 文件路径

	//通过设置标准输出为行缓冲模式
	setvbuf(stdout, NULL, _IOLBF, BUFSIZ);

	if (argc == 1) {
		fprintf(stderr, "This program needs arguments....\n\n");
		print_help(EXIT_FAILURE);
	}
	ctx.log_lvl = 6;
    unsigned long int parse_log_lvl;
    char* parse_end_token;
	bool start_server = false;
	bool full_dynamic = false;

	while ((opt = getopt(argc, argv, "hVc:l:x:sda:")) != -1) {
		switch (opt) {
		case 'h':
			print_help(EXIT_SUCCESS);
			break;
		case 'V':
			printf("realwmediumd v%s - a FPGA-accelerated wireless medium simulator "
			       "for mac80211_hwsim\n", VERSION_STR);
			exit(EXIT_SUCCESS);
			break;
		case 'c':
			config_file = optarg;
            printf("Input config file: %s\n", config_file);
			break;
		case 'x':
			printf("Input packet error rate file: %s\n", optarg);
			per_file = optarg;
            printf("Input packet error rate file: %s\n", per_file);
			break;
		case ':':
			printf("realwmediumd: Error - Option `%c' "
			       "needs a value\n\n", optopt);
			print_help(EXIT_FAILURE);
			break;
		case 'l':
			parse_log_lvl = strtoul(optarg, &parse_end_token, 10);
			if ((parse_log_lvl == ULONG_MAX && errno == ERANGE) ||
			     optarg == parse_end_token || parse_log_lvl > 7) {
				printf("realwmediumd: Error - Invalid RFC 5424 severity level: "
							   "%s\n\n", optarg);
				print_help(EXIT_FAILURE);
			}
			ctx.log_lvl = parse_log_lvl;
			break;
		case 'd':
			full_dynamic = true;
			break;
		case 's':
			start_server = true;
			break;
		case '?':
			printf("realwmediumd: Error - No such option: "
			       "`%c'\n\n", optopt);
			print_help(EXIT_FAILURE);
			break;
		}
    }

	//  1.检查硬件是否正常可以打开运行，如果可以正常打开则进行到下一步，打开硬件读写线程。
	init_hardware(&ctx);
	//  2.检查传入参数是否符合要求，如符合则继续调用完成参数初始化配置。
	INIT_LIST_HEAD(&ctx.stations);
	if (load_config(&ctx, config_file, per_file, full_dynamic))
		return EXIT_FAILURE;
	//  3.注册hwsim事件，开启事件处理线程（计时器触发+底层触发，如底层有数据待处理则触发）；
	/* init libevent */
	event_init();
	/* init netlink */
	if (init_netlink(&ctx) < 0)
		return EXIT_FAILURE;

	//    3.1  底层触发事件的接收节点的事件的发送：send_cloned_frame_msg
	//    3.2  计时器触发事件的发送节点的事件的发送：tx_info
	//  4.注册wserver线程，响应mininet-wifi的请求，完成内部配置
	
	//


    return EXIT_SUCCESS;
}
    