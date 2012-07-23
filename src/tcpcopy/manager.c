#include "../communication/msg.h"
#include "../util/util.h"
#include "../log/log.h"
#include "../event/select_server.h"
#include "address.h"
#include "send.h"
#include "manager.h"
#include "session.h"
#if (TCPCOPY_OFFLINE)
#include <pcap.h>
#endif


static int       raw_sock  = -1;
static uint64_t  event_cnt = 0, raw_packs = 0, valid_raw_packs = 0;
static uint32_t  localhost;
#if (TCPCOPY_OFFLINE)
static pcap_t    *pcap = NULL;
static struct timeval first_pack_time, last_pack_time, base_time, cur_time;
#endif

static void process_packet(bool backup, char *packet, int length){
    char tmp_packet[RECV_BUF_SIZE];
    if(!backup){
        process(packet);
    }else{
        memcpy(tmp_packet, packet, length);
        process(tmp_packet);
    }
}

static void set_nonblock(int socket)
{
    int flags;
    flags = fcntl(socket, F_GETFL, 0);
    fcntl(socket, F_SETFL, flags | O_NONBLOCK);
}

/* Initiate input raw socket */
static int init_input_raw_socket()
{
    int       sock, recv_buf_opt, ret;
    socklen_t opt_len;
#if (COPY_LINK_PACKETS)
    /* 
     * AF_PACKET
     * Packet sockets are used to receive or send raw packets 
     * at the device driver level.They allow the user to 
     * implement protocol modules in user space on top of 
     * the physical layer. 
     * ETH_P_IP
     * Internet Protocol packet that is related to the Ethernet 
     */
    sock = socket(AF_PACKET, SOCK_DGRAM, htons(ETH_P_IP));
#else 
    /* Copy ip datagram from IP layer*/
    sock = socket(AF_INET, SOCK_RAW, IPPROTO_TCP);
#endif
    if(-1 == sock){
        perror("socket");
        log_info(LOG_ERR, "%s", strerror(errno));   
    }
    set_nonblock(sock);
    recv_buf_opt   = 67108864;
    opt_len = sizeof(int);
    ret = setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &recv_buf_opt, opt_len);
    if(-1 == ret){
        perror("setsockopt");
        log_info(LOG_ERR, "setsockopt:%s", strerror(errno));    
    }

    return sock;
}

/* Replicate packets for multiple-copying */
static int replicate_packs(char *packet, int length, int replica_num)
{
    int           i;
    struct tcphdr *tcp_header;
    struct iphdr  *ip_header;
    uint32_t      size_ip;
    uint16_t      orig_port, addition, dest_port, rand_port;
    
    ip_header  = (struct iphdr*)packet;
    size_ip    = ip_header->ihl << 2;
    tcp_header = (struct tcphdr*)((char *)ip_header + size_ip);
    orig_port  = ntohs(tcp_header->source);

#if (DEBUG_TCPCOPY)
    log_info(LOG_DEBUG, "orig port:%u", orig_port);
#endif
    rand_port = clt_settings.rand_port_shifted;
    for(i = 1; i < replica_num; i++){
        addition   = (((i << 1)-1) << 5) + rand_port;
        dest_port  = get_appropriate_port(orig_port, addition);
#if (DEBUG_TCPCOPY)
        log_info(LOG_DEBUG, "new port:%u,add:%u", dest_port, addition);
#endif
        tcp_header->source = htons(dest_port);
        process_packet(true, packet, length);
    }
    return 0;

}

/*
 * Retrieve raw packets
 */
static int retrieve_raw_sockets(int sock)
{
    char     *packet, recv_buf[RECV_BUF_SIZE], tmp_buf[RECV_BUF_SIZE];
    int      replica_num, i, last, err, recv_len, packet_num, max_payload;
    int      index, payload_len;
    uint16_t id, size_ip, size_tcp, tot_len, cont_len, pack_len, head_len;
    uint32_t seq;

    struct tcphdr *tcp_header;
    struct iphdr  *ip_header;

    while(1){
        recv_len = recvfrom(sock, recv_buf, RECV_BUF_SIZE, 0, NULL, NULL);
        if(recv_len < 0){
            err = errno;
            if(EAGAIN == err){
                break;
            }
            perror("recvfrom");
            log_info(LOG_ERR, "recvfrom:%s", strerror(errno));
        }
        if(0 == recv_len){
            log_info(LOG_ERR, "recv len is 0");
            break;
        }
        raw_packs++;
        if(recv_len > RECV_BUF_SIZE){
            log_info(LOG_ERR, "recv_len:%d ,it is too long", recv_len);
            break;
        }
        packet = recv_buf;
        if(is_packet_needed((const char *)packet)){
            valid_raw_packs++;
            replica_num = clt_settings.replica_num;
            packet_num = 1;
            ip_header   = (struct iphdr*)packet;
            if(localhost == ip_header->saddr){
                if(0 != clt_settings.lo_tf_ip){
                    ip_header->saddr = clt_settings.lo_tf_ip;
                }
            }
            /* 
             * If packet length larger than MTU, then we split it. 
             * This is to solve the ip fragmentation problem
             */
            if(recv_len > clt_settings.mtu){
                /* Calculate number of packets */
                size_ip     = ip_header->ihl << 2;
                tot_len     = ntohs(ip_header -> tot_len);
                if(tot_len != recv_len){
                    log_info(LOG_WARN, "packet len:%u, recv len:%u",
                            tot_len, recv_len);
                    break;
                }
                tcp_header  = (struct tcphdr*)((char *)ip_header + size_ip);
                size_tcp    = tcp_header->doff << 2;
                cont_len    = tot_len - size_tcp - size_ip;
                head_len    = size_ip + size_tcp;
                max_payload = clt_settings.mtu - head_len;
                packet_num  = (cont_len + max_payload - 1)/max_payload;
                seq         = ntohl(tcp_header->seq);
                last        = packet_num - 1;
                id          = ip_header->id;
#if (DEBUG_TCPCOPY)
                strace_pack(LOG_NOTICE, CLIENT_FLAG, ip_header, tcp_header);
                log_info(LOG_INFO, "recv len:%d, more than MTU", recv_len);
#endif
                index = head_len;
                for(i = 0 ; i < packet_num; i++){
                    tcp_header->seq = htonl(seq + i * max_payload);
                    if(i != last){
                        pack_len = clt_settings.mtu;
                    }else{
                        pack_len += (cont_len - packet_num * max_payload);
                    }
                    payload_len = pack_len - head_len;
                    ip_header->tot_len = htons(pack_len);
                    ip_header->id = id++;
                    /* Copy header here */
                    memcpy(tmp_buf, recv_buf, head_len);
                    /* Copy payload here */
                    memcpy(tmp_buf + head_len, recv_buf + index, payload_len);
                    index = index + payload_len;
                    if(replica_num > 1){
                        process_packet(true, tmp_buf, pack_len);
                        replicate_packs(tmp_buf, pack_len, replica_num);
                    }else{
                        process_packet(false, tmp_buf, pack_len);
                    }
                }
            }else{
                if(replica_num > 1){
                    process_packet(true, packet, recv_len);
                    replicate_packs(packet, recv_len, replica_num);
                }else{
                    process_packet(false, packet, recv_len);
                }
            }
        }

        if(raw_packs%100000 == 0){
            log_info(LOG_NOTICE, "raw packets:%llu, valid :%llu",
                    raw_packs, valid_raw_packs);
        }
    }
    return 0;
}

/* Check resource usage, such as memory usage and cpu usage */
static void check_resource_usage()
{
    int           who = RUSAGE_SELF;
    struct rusage usage;
    int           ret;
    ret = getrusage(who, &usage);
    if(-1 == ret){
        perror("getrusage");
        log_info(LOG_ERR, "getrusage:%s", strerror(errno)); 
    }
    /* Total amount of user time used */
    log_info(LOG_NOTICE, "user time used:%ld", usage.ru_utime.tv_sec);
    /* Total amount of system time used */
    log_info(LOG_NOTICE, "sys  time used:%ld", usage.ru_stime.tv_sec);
    /* Maximum resident set size (in kilobytes) */
    /* This is only valid since Linux 2.6.32 */
    log_info(LOG_NOTICE, "max memory size:%ld", usage.ru_maxrss);
    if(usage.ru_maxrss > clt_settings.max_rss){
        log_info(LOG_WARN, "occupies too much memory,limit:%ld",
                clt_settings.max_rss);
    }
}

#if (TCPCOPY_OFFLINE)
static 
long timeval_diff(struct timeval *start, struct timeval *cur)
{
    long msec;
    msec=(cur->tv_sec-start->tv_sec)*1000;
    msec+=(cur->tv_usec-start->tv_usec)/1000;
    return msec;
}

static 
bool check_read_stop()
{
    long history_diff = timeval_diff(&last_pack_time, &first_pack_time);
    long cur_diff     = timeval_diff(&cur_time, &base_time);
    if(history_diff < cur_diff){
        return false;
    }
    return true;
}

static int get_l2_len(const unsigned char *packet, const int pkt_len)
{
    struct ethernet_hdr *eth;
    eth = (struct ethernet_hdr *)packet;
    switch (ntohs(eth->ether_type)) {
        case ETHERTYPE_VLAN:
            return 18;
            break;

        default:
            return 14;
            break;
    }
    log_info(LOG_WARN, "bug in my code");
    return -1;
}

static unsigned char pcap_ip_buf[65536];

static 
unsigned char *get_ip_data(unsigned char *packet, const int pkt_len)
{
    int    l2_len;
    u_char *ptr;

    l2_len = get_l2_len(packet, pkt_len);

    if (pkt_len <= l2_len){
        return NULL;
    }
#ifdef FORCE_ALIGN
    if (l2_len % 4 == 0) {
        ptr = (&(packet)[l2_len]);
    } else {
        ptr = pcap_ip_buf;
        memcpy(ptr, (&(packet)[l2_len]), pkt_len - l2_len);
    }
#else
    ptr = (&(packet)[l2_len]);
#endif
    return ptr;

}

static void send_packets_from_pcap()
{
    struct pcap_pkthdr  pkt_hdr;  
    unsigned char       *pkt_data;
    bool                stop = false;
    gettimeofday(&cur_time, NULL);
    while(!stop){
        pkt_data = (u_char *)pcap_next(pcap, &pkt_hdr);
        if(pkt_data != NULL){
            last_pack_time = pkt_hdr.ts;
            stop = check_read_stop();
            if(pkt_hdr.caplen < pkt_hdr.len){
                log_info(LOG_WARN, "truncated packets,drop");
            }else{
                process((char*)(get_ip_data(pkt_data, pkt_hdr.caplen)));
            }
        }
    }
}

#endif

/* Dispose one event*/
static void dispose_event(int fd)
{
    struct msg_server_s *msg;

    event_cnt++;
    if(fd == raw_sock){
        retrieve_raw_sockets(fd);
    }else{
        msg = msg_client_recv(fd);
        if(NULL == msg ){
            fprintf(stderr, "NULL msg :\n");
            log_info(LOG_ERR, "NULL msg from msg_client_recv");
            exit(EXIT_FAILURE);
        }   
        process((char*)msg);
    }   
#if (TCPCOPY_OFFLINE)
    send_packets_from_pcap();
#endif
    if((event_cnt%1000000) == 0){
        check_resource_usage();
    }
}

void tcp_copy_exit()
{
    int i;
    fprintf(stderr, "exit tcpcopy\n");
    destroy_for_sessions();
    if(-1 != raw_sock){
        close(raw_sock);
        raw_sock = -1;
    }
    send_close();
    address_close_sock();
    log_end();
#ifdef TCPCOPY_MYSQL_ADVANCED
    release_mysql_user_pwd_info();
#endif
    if(clt_settings.transfer.mappings != NULL){
        for(i = 0; i < clt_settings.transfer.num; i++){
            free(clt_settings.transfer.mappings[i]);
        }
        free(clt_settings.transfer.mappings);
        clt_settings.transfer.mappings = NULL;
    }
    exit(EXIT_SUCCESS);

}

void tcp_copy_over(const int sig)
{
    long int pid   = (long int)syscall(224);
    log_info(LOG_WARN, "sig %d received, pid=%ld", sig, pid);
    sync();
    exit(EXIT_SUCCESS);
}


/* Initiate tcpcopy client */
int tcp_copy_init()
{
    int                    i;
#if (TCPCOPY_OFFLINE)
    char                  *pcap_file;
    char                   ebuf[PCAP_ERRBUF_SIZE];
#endif
    uint16_t               online_port, target_port;
    uint32_t               target_ip;
    ip_port_pair_mapping_t *pair;
    ip_port_pair_mapping_t **mappings;

    select_server_set_callback(dispose_event);

    /* Init session table*/
    init_for_sessions();
    localhost = inet_addr("127.0.0.1"); 

    /* Init input raw socket info */
    
#if (!TCPCOPY_OFFLINE)
    raw_sock = init_input_raw_socket();
#endif
    if(raw_sock != -1){
        /* Add the input raw socket to select */
        select_server_add(raw_sock);
        /* Init output raw socket info */
        send_init();
        /* Add connections to the tested server for exchanging info */
        mappings = clt_settings.transfer.mappings;
        for(i = 0; i < clt_settings.transfer.num; i++){
            pair = mappings[i];
            online_port = pair->online_port;
            target_ip   = pair->target_ip;
            target_port = pair->target_port;
            address_add_msg_conn(online_port, target_ip, 
                    clt_settings.srv_port);
            log_info(LOG_NOTICE, "add a tunnel for exchanging info:%u",
                    ntohs(target_port));
        }
        return SUCCESS;
    }else{
#if (TCPCOPY_OFFLINE)
        pcap_file = clt_settings.pcap_file;
        if(pcap_file != NULL){
            if ((pcap = pcap_open_offline(pcap_file, ebuf)) == NULL){
                log_info(LOG_ERR, "open %s,%s", pcap_file, ebuf);
            }else{
                gettimeofday(&base_time, NULL);
                send_packets_from_pcap();
                return SUCCESS;
            }
        }
#endif
        return FAILURE;
    }
}

