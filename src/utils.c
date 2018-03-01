/*
 * Copyright (C) 2017 Jianhui Zhao <jianhuizhao329@gmail.com>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <uhttpd/uhttpd.h>
#include <libubox/ulog.h>
#include <libubox/avl-cmp.h>
#include <libubox/avl.h>
#include <libubus.h>
#include "utils.h"
#include "config.h"

#define TEMPPASS_TIME  5000
#define CHECK_INTERNET_TIME 5000

static struct avl_tree temppass_tree;
static struct ubus_context *ubus_ctx;

struct termianl_temppass {
    char mac[18];
    struct avl_node node;
    struct uloop_timeout timer;
};

int get_iface_ip(const char *ifname, char *dst, int len)
{
    struct ifreq ifr;
    int sock = -1;

    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        ULOG_ERR("socket:%s\n", strerror(errno));
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));

    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));

    if (ioctl(sock, SIOCGIFADDR, &ifr) < 0) {
        ULOG_ERR("ioctl:%s\n", strerror(errno));
        close(sock);
        return -1;
    }

    close(sock);
    snprintf(dst, len, "%s", inet_ntoa(((struct sockaddr_in*)&(ifr.ifr_addr))->sin_addr));
    
    return 0;
}

int get_iface_mac(const char *ifname, char *dst, int len)
{
    struct ifreq ifr;
    int sock;
    uint8_t *hw;

    if ((sock = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        ULOG_ERR("socket:%s\n", strerror(errno));
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));

    ifr.ifr_addr.sa_family = AF_INET;
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));

    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        ULOG_ERR("ioctl:%s\n", strerror(errno));
        close(sock);
        return -1;
    }
    
    close(sock);

    hw = (uint8_t *)ifr.ifr_hwaddr.sa_data;
    snprintf(dst, len, "%02X%02X%02X%02X%02X%02X", hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);

    return 0;
}

int arp_get(const char *ifname, const char *ip, char *dst, int len)
{
	struct arpreq req;  
    struct sockaddr_in *sin;  
    int sock = 0;
    uint8_t *hw;
  
    memset(&req, 0, sizeof(struct arpreq));  
  
    sin = (struct sockaddr_in *)&req.arp_pa;  
    sin->sin_family = AF_INET;  
    sin->sin_addr.s_addr = inet_addr(ip);  
  
    strncpy(req.arp_dev, ifname, sizeof(req.arp_dev));  
  
    sock = socket(AF_INET, SOCK_DGRAM, 0);  
    if(sock < 0) {
        ULOG_ERR("socket:%s\n", strerror(errno));  
        return -1;  
    }  
  
    if (ioctl(sock, SIOCGARP, &req) < 0) {
        ULOG_ERR("ioctl:%s\n", strerror(errno));  
        close(sock);  
        return -1;  
    }

    close(sock);

    hw = (uint8_t *)req.arp_ha.sa_data; 
    snprintf(dst, len, "%02X:%02X:%02X:%02X:%02X:%02X", hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);

    return 0;
}

int enable_kmod(bool enable, const char *interface, int port, int ssl_port)
{
    FILE *fp = fopen("/proc/wifidog/config", "w");
    if (!fp) {
        ULOG_ERR("fopen:%s\n", strerror(errno));
        return -1;
    }

    if (enable) {
        fprintf(fp, "interface=%s\n", interface);
        fprintf(fp, "port=%d\n", port);
        fprintf(fp, "ssl_port=%d\n", ssl_port);
    }

    fprintf(fp, "enabled=%d\n", enable ? 1 : 0);
    fclose(fp);
    return 0;
}

static void temppass_timer_cb(struct uloop_timeout *t)
{
    struct termianl_temppass *termianl = container_of(t, struct termianl_temppass, timer);

    deny_termianl(termianl->mac);
    avl_delete(&temppass_tree, &termianl->node);
    free(termianl);
}

int allow_termianl(const char *mac, const char *token, bool temporary)
{
    struct termianl_temppass *termianl;

    FILE *fp = fopen("/proc/wifidog/term", "w");
    if (!fp) {
        ULOG_ERR("fopen:%s\n", strerror(errno));
        return -1;
    }

    fprintf(fp, "+%s %s\n", mac, token ? token : "");
    fclose(fp);

    ULOG_INFO("allow termianl %s: %s\n", temporary ? "temporary" : "", mac);

    termianl = avl_find_element(&temppass_tree, mac, termianl, node);
    if (termianl) {
        if (temporary) {
            uloop_timeout_set(&termianl->timer, TEMPPASS_TIME);
            return 0;
        }
        uloop_timeout_cancel(&termianl->timer);
        avl_delete(&temppass_tree, &termianl->node);
        free(termianl);
    } else if (temporary) {
        termianl = calloc(1, sizeof(struct termianl_temppass));
        if (!termianl) {
            ULOG_ERR("allow_termianl temporary FAILED: No mem\n");
            return -1;
        }

        termianl->node.key = strcpy(termianl->mac, mac);
        termianl->timer.cb = temppass_timer_cb;
        uloop_timeout_set(&termianl->timer, TEMPPASS_TIME);
        avl_insert(&temppass_tree, &termianl->node);
    }
    return 0;
}

int deny_termianl(const char *mac)
{
    FILE *fp = fopen("/proc/wifidog/term", "w");
    if (!fp) {
        ULOG_ERR("fopen:%s\n", strerror(errno));
        return -1;
    }

    fprintf(fp, "-%s\n", mac);
    fclose(fp);

    ULOG_INFO("deny termianl: %s\n", mac);
    return 0;
}

int allow_destip(const char *ip)
{
    FILE *fp = fopen("/proc/wifidog/ip", "w");
    if (!fp) {
        ULOG_ERR("fopen:%s\n", strerror(errno));
        return -1;
    }

    fprintf(fp, "+%s\n", ip);
    fclose(fp);

    ULOG_INFO("allow destip: %s\n", ip);

    return 0;
}

void termianl_temppass_init()
{
    avl_init(&temppass_tree, avl_strcmp, false, NULL);
}

/* blen is the size of buf; slen is the length of src.  The input-string need
** not be, and the output string will not be, null-terminated.  Returns the
** length of the encoded string, or -1 on error (buffer overflow) */
int urlencode(char *buf, int blen, const char *src, int slen)
{
    static const char hex[] = "0123456789abcdef";
    int i, len = 0;

    blen -= 1;

    for (i = 0; (i < slen) && (len < blen); i++) {
        if(isalnum(src[i]) || (src[i] == '-') || (src[i] == '_') ||
            (src[i] == '.') || (src[i] == '~')) {
            buf[len++] = src[i];
        } else if ((len + 3) <= blen) {
            buf[len++] = '%';
            buf[len++] = hex[(src[i] >> 4) & 15];
            buf[len++] = hex[ src[i]       & 15];
        } else {
            len = -1;
            break;
        }
    }

    if (i == slen)
        buf[slen] = 0;
    return (i == slen) ? len : -1;
}

int ubus_init()
{
    ubus_ctx = ubus_connect(NULL);
    if (!ubus_ctx) {
        ULOG_ERR("Failed to connect to ubus\n");
        return -1;
    }
    ubus_add_uloop(ubus_ctx);
    return 0;
}

enum {
    PINGCHECK_STATUS,

};

static const struct blobmsg_policy pingcheck_policy[1] = {
    [PINGCHECK_STATUS] = { .name = "status", .type = BLOBMSG_TYPE_STRING },
};

static void fetch_internet_status(struct ubus_request *req, int type, struct blob_attr *msg)
{
    struct blob_attr *tb[1];
    struct config *conf = get_config();

    blobmsg_parse(pingcheck_policy, 1, tb, blob_data(msg), blob_len(msg));

    if (tb[PINGCHECK_STATUS]) {
        if (!strcmp(blobmsg_get_string(tb[PINGCHECK_STATUS]), "ONLINE")) {
            ULOG_INFO("Internet online\n");
            enable_kmod(true, conf->gw_interface, conf->gw_port, conf->gw_ssl_port);
            return;
        }
    }
    enable_kmod(false, NULL, 0, 0);
    ULOG_INFO("Internet offline\n");
}

static void check_internet_cb(struct uloop_timeout *t)
{
    uint32_t id;

    uloop_timeout_set(t, CHECK_INTERNET_TIME);

    if (ubus_lookup_id(ubus_ctx, "pingcheck", &id)) {
        ULOG_ERR("Failed to look up pingcheck object\n");
        return;
    }

    ubus_invoke(ubus_ctx, id, "status", NULL, fetch_internet_status, NULL, 3000);
}

void start_check_internet()
{
    static struct uloop_timeout timeout = {
        .cb = check_internet_cb
    };

    uloop_timeout_set(&timeout, 0);
}
