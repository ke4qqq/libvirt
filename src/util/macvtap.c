/*
 * Copyright (C) 2010 IBM Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 * Authors:
 *     Stefan Berger <stefanb@us.ibm.com>
 *
 * Notes:
 * netlink: http://lovezutto.googlepages.com/netlink.pdf
 *          iproute2 package
 *
 */

#include <config.h>

#if WITH_MACVTAP

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <linux/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_tun.h>

#include "c-ctype.h"
#include "util.h"
#include "memory.h"
#include "macvtap.h"
#include "conf/domain_conf.h"
#include "virterror_internal.h"

#define VIR_FROM_THIS VIR_FROM_NET

#define ReportError(conn, code, fmt...)                                      \
        virReportErrorHelper(conn, VIR_FROM_NET, code, __FILE__,          \
                               __FUNCTION__, __LINE__, fmt)

#define MACVTAP_NAME_PREFIX	"macvtap"
#define MACVTAP_NAME_PATTERN	"macvtap%d"

static int nlOpen(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (fd < 0)
        virReportSystemError(errno,
                             "%s",_("cannot open netlink socket"));
    return fd;
}


static void nlClose(int fd)
{
    close(fd);
}


/**
 * nlComm:
 * @nlmsg: pointer to netlink message
 * @respbuf: pointer to pointer where response buffer will be allocated
 * @respbuflen: pointer to integer holding the size of the response buffer
 *      on return of the function.
 *
 * Send the given message to the netlink layer and receive response.
 * Returns 0 on success, -1 on error. In case of error, no response
 * buffer will be returned.
 */
static
int nlComm(struct nlmsghdr *nlmsg,
           char **respbuf, int *respbuflen)
{
    int rc = 0;
    struct sockaddr_nl nladdr = {
            .nl_family = AF_NETLINK,
            .nl_pid    = 0,
            .nl_groups = 0,
    };
    int rcvChunkSize = 1024; // expecting less than that
    int rcvoffset = 0;
    ssize_t nbytes;
    int fd = nlOpen();

    if (fd < 0)
        return -1;

    nlmsg->nlmsg_flags |= NLM_F_ACK;

    nbytes = sendto(fd, (void *)nlmsg, nlmsg->nlmsg_len, 0,
                    (struct sockaddr *)&nladdr, sizeof(nladdr));
    if (nbytes < 0) {
        virReportSystemError(errno,
                             "%s", _("cannot send to netlink socket"));
        rc = -1;
        goto err_exit;
    }

    while (1) {
        if (VIR_REALLOC_N(*respbuf, rcvoffset+rcvChunkSize) < 0) {
            virReportOOMError();
            rc = -1;
            goto err_exit;
        }

        socklen_t addrlen = sizeof(nladdr);
        nbytes = recvfrom(fd, &((*respbuf)[rcvoffset]), rcvChunkSize, 0,
                          (struct sockaddr *)&nladdr, &addrlen);
        if (nbytes < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            virReportSystemError(errno, "%s",
                                 _("error receiving from netlink socket"));
            rc = -1;
            goto err_exit;
        }
        rcvoffset += nbytes;
        break;
    }
    *respbuflen = rcvoffset;

err_exit:
    if (rc == -1) {
        VIR_FREE(*respbuf);
        *respbuf = NULL;
        *respbuflen = 0;
    }

    nlClose(fd);
    return rc;
}


static struct rtattr *
rtattrCreate(char *buffer, int bufsize, int type,
             const void *data, int datalen)
{
    struct rtattr *r = (struct rtattr *)buffer;
    r->rta_type = type;
    r->rta_len  = RTA_LENGTH(datalen);
    if (r->rta_len > bufsize)
        return NULL;
    memcpy(RTA_DATA(r), data, datalen);
    return r;
}


static void
nlInit(struct nlmsghdr *nlm, int flags, int type)
{
    nlm->nlmsg_len = NLMSG_LENGTH(0);
    nlm->nlmsg_flags = flags;
    nlm->nlmsg_type = type;
}


static void
nlAlign(struct nlmsghdr *nlm)
{
    nlm->nlmsg_len = NLMSG_ALIGN(nlm->nlmsg_len);
}


static void *
nlAppend(struct nlmsghdr *nlm, int totlen, const void *data, int datalen)
{
    char *pos;
    nlAlign(nlm);
    if (nlm->nlmsg_len + NLMSG_ALIGN(datalen) > totlen)
        return NULL;
    pos = (char *)nlm + nlm->nlmsg_len;
    memcpy(pos, data, datalen);
    nlm->nlmsg_len += datalen;
    nlAlign(nlm);
    return pos;
}


static int
getIfIndex(virConnectPtr conn,
           const char *ifname,
           int *idx)
{
    int rc = 0;
    struct ifreq ifreq;
    int fd = socket(PF_PACKET, SOCK_DGRAM, 0);

    if (fd < 0)
        return errno;

    if (virStrncpy(ifreq.ifr_name, ifname, strlen(ifname),
                   sizeof(ifreq.ifr_name)) == NULL) {
        if (conn)
            ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                        _("invalid interface name %s"),
                        ifname);
        rc = EINVAL;
        goto err_exit;
    }
    if (ioctl(fd, SIOCGIFINDEX, &ifreq) >= 0)
        *idx = ifreq.ifr_ifindex;
    else {
        if (conn)
            ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                        _("interface %s does not exist"),
                        ifname);
        rc = ENODEV;
    }

err_exit:
    close(fd);

    return rc;
}


/*
 * chgIfFlags: Change flags on an interface
 * @ifname : name of the interface
 * @flagclear : the flags to clear
 * @flagset : the flags to set
 *
 * The new flags of the interface will be calculated as
 * flagmask = (~0 ^ flagclear)
 * newflags = (curflags & flagmask) | flagset;
 *
 * Returns 0 on success, errno on failure.
 */
static int chgIfFlags(const char *ifname, short flagclear, short flagset) {
    struct ifreq ifr;
    int rc = 0;
    int flags;
    short flagmask = (~0 ^ flagclear);
    int fd = socket(PF_PACKET, SOCK_DGRAM, 0);

    if (fd < 0)
        return errno;

    if (virStrncpy(ifr.ifr_name,
                   ifname, strlen(ifname), sizeof(ifr.ifr_name)) == NULL) {
        rc = ENODEV;
        goto err_exit;
    }

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        rc = errno;
        goto err_exit;
    }

    flags = (ifr.ifr_flags & flagmask) | flagset;

    if (ifr.ifr_flags != flags) {
        ifr.ifr_flags = flags;

        if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0)
            rc = errno;
    }

err_exit:
    close(fd);
    return rc;
}

/*
 * ifUp
 * @name: name of the interface
 * @up: 1 for up, 0 for down
 *
 * Function to control if an interface is activated (up, 1) or not (down, 0)
 *
 * Returns 0 in case of success or an errno code in case of failure.
 */
static int
ifUp(const char *name, int up)
{
    return chgIfFlags(name,
                      (up) ? 0      : IFF_UP,
                      (up) ? IFF_UP : 0);
}


static int
link_add(virConnectPtr conn,
         const char *type,
         const unsigned char *macaddress, int macaddrsize,
         const char *ifname,
         const char *srcdev,
         uint32_t macvlan_mode,
         int *retry)
{
    int rc = 0;
    char nlmsgbuf[256];
    struct nlmsghdr *nlm = (struct nlmsghdr *)nlmsgbuf, *resp;
    struct nlmsgerr *err;
    char rtattbuf[64];
    struct rtattr *rta, *rta1, *li;
    struct ifinfomsg i = { .ifi_family = AF_UNSPEC };
    int ifindex;
    char *recvbuf = NULL;
    int recvbuflen;

    if (getIfIndex(conn, srcdev, &ifindex) != 0)
        return -1;

    *retry = 0;

    memset(&nlmsgbuf, 0, sizeof(nlmsgbuf));

    nlInit(nlm, NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL, RTM_NEWLINK);

    if (!nlAppend(nlm, sizeof(nlmsgbuf), &i, sizeof(i)))
        goto buffer_too_small;

    rta = rtattrCreate(rtattbuf, sizeof(rtattbuf), IFLA_LINK,
                       &ifindex, sizeof(ifindex));
    if (!rta)
        goto buffer_too_small;

    if (!nlAppend(nlm, sizeof(nlmsgbuf), rtattbuf, rta->rta_len))
        goto buffer_too_small;

    rta = rtattrCreate(rtattbuf, sizeof(rtattbuf), IFLA_ADDRESS,
                       macaddress, macaddrsize);
    if (!rta)
        goto buffer_too_small;

    if (!nlAppend(nlm, sizeof(nlmsgbuf), rtattbuf, rta->rta_len))
        goto buffer_too_small;

    if (ifname) {
        rta = rtattrCreate(rtattbuf, sizeof(rtattbuf), IFLA_IFNAME,
                           ifname, strlen(ifname) + 1);
        if (!rta)
            goto buffer_too_small;

        if (!nlAppend(nlm, sizeof(nlmsgbuf), rtattbuf, rta->rta_len))
            goto buffer_too_small;
    }

    rta = rtattrCreate(rtattbuf, sizeof(rtattbuf), IFLA_LINKINFO, NULL, 0);
    if (!rta)
        goto buffer_too_small;

    if (!(li = nlAppend(nlm, sizeof(nlmsgbuf), rtattbuf, rta->rta_len)))
        goto buffer_too_small;

    rta = rtattrCreate(rtattbuf, sizeof(rtattbuf), IFLA_INFO_KIND,
                       type, strlen(type));
    if (!rta)
        goto buffer_too_small;

    if (!nlAppend(nlm, sizeof(nlmsgbuf), rtattbuf, rta->rta_len))
        goto buffer_too_small;

    if (macvlan_mode > 0) {
        rta = rtattrCreate(rtattbuf, sizeof(rtattbuf), IFLA_INFO_DATA,
                           NULL, 0);
        if (!rta)
            goto buffer_too_small;

        if (!(rta1 = nlAppend(nlm, sizeof(nlmsgbuf), rtattbuf, rta->rta_len)))
            goto buffer_too_small;

        rta = rtattrCreate(rtattbuf, sizeof(rtattbuf), IFLA_MACVLAN_MODE,
                           &macvlan_mode, sizeof(macvlan_mode));
        if (!rta)
            goto buffer_too_small;

        if (!nlAppend(nlm, sizeof(nlmsgbuf), rtattbuf, rta->rta_len))
            goto buffer_too_small;

        rta1->rta_len = (char *)nlm + nlm->nlmsg_len - (char *)rta1;
    }

    li->rta_len = (char *)nlm + nlm->nlmsg_len - (char *)li;

    if (nlComm(nlm, &recvbuf, &recvbuflen) < 0)
        return -1;

    if (recvbuflen < NLMSG_LENGTH(0) || recvbuf == NULL)
        goto malformed_resp;

    resp = (struct nlmsghdr *)recvbuf;

    switch (resp->nlmsg_type) {
    case NLMSG_ERROR:
        err = (struct nlmsgerr *)NLMSG_DATA(resp);
        if (resp->nlmsg_len < NLMSG_LENGTH(sizeof(*err)))
            goto malformed_resp;

        switch (-err->error) {

        case 0:
        break;

        case EEXIST:
            *retry = 1;
            rc = -1;
        break;

        default:
            virReportSystemError(-err->error,
                                 _("error creating %s type of interface"),
                                 type);
            rc = -1;
        }
    break;

    case NLMSG_DONE:
    break;

    default:
        goto malformed_resp;
    }

    VIR_FREE(recvbuf);

    return rc;

malformed_resp:
    ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                _("malformed netlink response message"));
    VIR_FREE(recvbuf);
    return -1;

buffer_too_small:
    ReportError(conn, VIR_ERR_INTERNAL_ERROR,
                _("internal buffer is too small"));
    return -1;
}


static int
link_del(const char *type,
         const char *name)
{
    int rc = 0;
    char nlmsgbuf[256];
    struct nlmsghdr *nlm = (struct nlmsghdr *)nlmsgbuf, *resp;
    struct nlmsgerr *err;
    char rtattbuf[64];
    struct rtattr *rta, *rta1;
    struct ifinfomsg ifinfo = { .ifi_family = AF_UNSPEC };
    char *recvbuf = NULL;
    int recvbuflen;

    memset(&nlmsgbuf, 0, sizeof(nlmsgbuf));

    nlInit(nlm, NLM_F_REQUEST | NLM_F_CREATE | NLM_F_EXCL, RTM_DELLINK);

    if (!nlAppend(nlm, sizeof(nlmsgbuf), &ifinfo, sizeof(ifinfo)))
        goto buffer_too_small;

    rta = rtattrCreate(rtattbuf, sizeof(rtattbuf), IFLA_LINKINFO, NULL, 0);
    if (!rta)
        goto buffer_too_small;

    if (!(rta1 = nlAppend(nlm, sizeof(nlmsgbuf), rtattbuf, rta->rta_len)))
        goto buffer_too_small;

    rta = rtattrCreate(rtattbuf, sizeof(rtattbuf), IFLA_INFO_KIND,
                       type, strlen(type));
    if (!rta)
        goto buffer_too_small;

    if (!nlAppend(nlm, sizeof(nlmsgbuf), rtattbuf, rta->rta_len))
        goto buffer_too_small;

    rta1->rta_len = (char *)nlm + nlm->nlmsg_len - (char *)rta1;

    rta = rtattrCreate(rtattbuf, sizeof(rtattbuf), IFLA_IFNAME,
                       name, strlen(name)+1);
    if (!rta)
        goto buffer_too_small;

    if (!nlAppend(nlm, sizeof(nlmsgbuf), rtattbuf, rta->rta_len))
        goto buffer_too_small;

    if (nlComm(nlm, &recvbuf, &recvbuflen) < 0)
        return -1;

    if (recvbuflen < NLMSG_LENGTH(0) || recvbuf == NULL)
        goto malformed_resp;

    resp = (struct nlmsghdr *)recvbuf;

    switch (resp->nlmsg_type) {
    case NLMSG_ERROR:
        err = (struct nlmsgerr *)NLMSG_DATA(resp);
        if (resp->nlmsg_len < NLMSG_LENGTH(sizeof(*err)))
            goto malformed_resp;

        switch (-err->error) {
        case 0:
        break;

        default:
            virReportSystemError(-err->error,
                                 _("error destroying %s interface"),
                                 name);
            rc = -1;
        }
    break;

    case NLMSG_DONE:
    break;

    default:
        goto malformed_resp;
    }

    VIR_FREE(recvbuf);

    return rc;

malformed_resp:
    ReportError(NULL, VIR_ERR_INTERNAL_ERROR,
                "%s", _("malformed netlink response message"));
    VIR_FREE(recvbuf);
    return -1;

buffer_too_small:
    ReportError(NULL, VIR_ERR_INTERNAL_ERROR,
                "%s", _("internal buffer is too small"));
    return -1;
}


/* Open the macvtap's tap device.
 * @conn: Pointer to virConnect object
 * @name: Name of the macvtap interface
 * @retries : Number of retries in case udev for example may need to be
 *            waited for to create the tap chardev
 * Returns negative value in case of error, the file descriptor otherwise.
 */
static
int openTap(const char *ifname,
            int retries)
{
    FILE *file;
    char path[64];
    int ifindex;
    char tapname[50];
    int tapfd;

    if (snprintf(path, sizeof(path),
                 "/sys/class/net/%s/ifindex", ifname) >= sizeof(path)) {
        virReportSystemError(errno,
                             "%s",
                             _("buffer for ifindex path is too small"));
        return -1;
    }

    file = fopen(path, "r");

    if (!file) {
        virReportSystemError(errno,
                             _("cannot open macvtap file %s to determine "
                               "interface index"), path);
        return -1;
    }

    if (fscanf(file, "%d", &ifindex) != 1) {
        virReportSystemError(errno,
                             "%s",_("cannot determine macvtap's tap device "
                             "interface index"));
        fclose(file);
        return -1;
    }

    fclose(file);

    if (snprintf(tapname, sizeof(tapname),
                 "/dev/tap%d", ifindex) >= sizeof(tapname)) {
        virReportSystemError(errno,
                             "%s",
                             _("internal buffer for tap device is too small"));
        return -1;
    }

    while (1) {
        // may need to wait for udev to be done
        tapfd = open(tapname, O_RDWR);
        if (tapfd < 0 && retries > 0) {
            retries--;
            usleep(20000);
            continue;
        }
        break;
    }

    if (tapfd < 0)
        virReportSystemError(errno,
                             _("cannot open macvtap tap device %s"),
                             tapname);

    return tapfd;
}


static uint32_t
macvtapModeFromInt(enum virDomainNetdevMacvtapType mode)
{
    switch (mode) {
    case VIR_DOMAIN_NETDEV_MACVTAP_MODE_PRIVATE:
        return MACVLAN_MODE_PRIVATE;
    break;

    case VIR_DOMAIN_NETDEV_MACVTAP_MODE_BRIDGE:
        return MACVLAN_MODE_BRIDGE;
    break;

    case VIR_DOMAIN_NETDEV_MACVTAP_MODE_VEPA:
    default:
        return MACVLAN_MODE_VEPA;
    }
}


/* openMacvtapTap:
 * Create an instance of a macvtap device and open its tap character
 * device.
 * @conn: Pointer to virConnect object
 * @tgifname: Interface name that the macvtap is supposed to have. May
 *    be NULL if this function is supposed to choose a name
 * @macaddress: The MAC address for the macvtap device
 * @linkdev: The interface name of the NIC to connect to the external bridge
 * @mode_str: String describing the mode. Valid are 'bridge', 'vepa' and
 *     'private'.
 * @res_ifname: Pointer to a string pointer where the actual name of the
 *     interface will be stored into if everything succeeded. It is up
 *     to the caller to free the string.
 *
 * Returns file descriptor of the tap device in case of success,
 * negative value otherwise with error message attached to the 'conn'
 * object.
 *
 */
int
openMacvtapTap(virConnectPtr conn,
               const char *tgifname,
               const unsigned char *macaddress,
               const char *linkdev,
               int mode,
               char **res_ifname)
{
    const char *type = "macvtap";
    int c, rc;
    char ifname[IFNAMSIZ];
    int retries, do_retry = 0;
    uint32_t macvtapMode = macvtapModeFromInt(mode);
    const char *cr_ifname;
    int ifindex;

    *res_ifname = NULL;

    if (tgifname) {
        if(getIfIndex(NULL, tgifname, &ifindex) == 0) {
            if (STRPREFIX(tgifname,
                          MACVTAP_NAME_PREFIX)) {
                goto create_name;
            }
            virReportSystemError(errno,
                                 _("Interface %s already exists"), tgifname);
            return -1;
        }
        cr_ifname = tgifname;
        rc = link_add(conn, type, macaddress, 6, tgifname, linkdev,
                      macvtapMode, &do_retry);
        if (rc)
            return -1;
    } else {
create_name:
        retries = 5;
        for (c = 0; c < 8192; c++) {
            snprintf(ifname, sizeof(ifname), MACVTAP_NAME_PATTERN, c);
            if (getIfIndex(NULL, ifname, &ifindex) == ENODEV) {
                rc = link_add(conn, type, macaddress, 6, ifname, linkdev,
                              macvtapMode, &do_retry);
                if (rc == 0)
                    break;

                if (do_retry && --retries)
                    continue;
                return -1;
            }
        }
        cr_ifname = ifname;
    }

    rc = ifUp(cr_ifname, 1);
    if (rc != 0) {
        virReportSystemError(errno,
                             _("cannot 'up' interface %s"), cr_ifname);
        rc = -1;
        goto link_del_exit;
    }

    rc = openTap(cr_ifname, 10);

    if (rc > 0)
        *res_ifname = strdup(cr_ifname);
    else
        goto link_del_exit;

    return rc;

link_del_exit:
    link_del(type, ifname);

    return rc;
}


/* Delete a macvtap type of interface given the MAC address. This
 * function will delete all macvtap type of interfaces that have the
 * given MAC address.
 * @macaddress : Pointer to 6 bytes holding the MAC address of the
 *    macvtap device(s) to destroy
 *
 * Returns 0 in case of success, negative value in case of error.
 */
int
delMacvtapByMACAddress(const unsigned char *macaddress,
                       int *hasBusyDev)
{
    struct ifreq ifr;
    FILE *file;
    char *ifname, *pos;
    char buffer[1024];
    off_t oldpos = 0;
    int tapfd;

    *hasBusyDev = 0;

    file = fopen("/proc/net/dev", "r");

    if (!file) {
        virReportSystemError(errno, "%s",
                             _("cannot open file to read network interfaces "
                             "from"));
        return -1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        virReportSystemError(errno, "%s",
                             _("cannot open socket"));
        goto sock_err;
    }

    while (NULL != (ifname = fgets(buffer, sizeof(buffer), file))) {
        if (c_isspace(ifname[0]))
            ifname++;
        if ((pos = strchr(ifname, ':')) != NULL) {
            pos[0] = 0;
            if (virStrncpy(ifr.ifr_name, ifname, strlen(ifname),
                           sizeof(ifr.ifr_name)) == NULL)
                continue;
            if (ioctl(sock, SIOCGIFHWADDR, (char *)&ifr) >= 0) {
                if (memcmp(&ifr.ifr_hwaddr.sa_data[0], macaddress, 6) == 0) {
                    tapfd = openTap(ifname, 0);
                    if (tapfd > 0) {
                        close(tapfd);
                        ifUp(ifname, 0);
                        if (link_del("macvtap", ifname) == 0)
                            fseeko(file, oldpos, SEEK_SET);
                    } else {
                        *hasBusyDev = 1;
                    }
                }
            }
        }
        oldpos = ftello(file);
    }

    close(sock);
sock_err:
    fclose(file);

    return 0;
}

#endif
