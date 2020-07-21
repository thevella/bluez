//
// Created by ruundii on 20/07/2020.
//

#include "host.h"
#include <errno.h>
#include <gmodule.h>
#include "lib/sdp.h"
#include "src/adapter.h"
#include "src/device.h"
#include "src/log.h"


struct input_host{
    struct btd_device	*device;
    char			*path;
    bdaddr_t		src;
    bdaddr_t		dst;
    GIOChannel		*ctrl_io;
    GIOChannel		*intr_io;
    guint			ctrl_watch;
    guint			intr_watch;

/*
	guint			sec_watch;

    enum reconnect_mode_t	reconnect_mode;
    guint			reconnect_timer;
    uint32_t		reconnect_attempt;
    struct bt_uhid		*uhid;
    bool			uhid_created;
    uint8_t			report_req_pending;
    guint			report_req_timer;
    uint32_t		report_rsp_id;*/

};

static GSList* hosts = NULL;

static int cmp_host_addr(gconstpointer a, gconstpointer dst)
{
    const struct input_host *host = a;
    return bacmp(&host->dst, dst);
}

static struct input_host *find_host(const bdaddr_t *src, const bdaddr_t *dst, bool create_if_not_found){
    GSList *host_list_item = g_slist_find_custom(hosts, dst, cmp_host_addr);
    struct btd_device *device = btd_adapter_find_device(adapter_find(src), dst, BDADDR_BREDR);

    if (device == NULL)
        return NULL;

    if (host_list_item == NULL) {
        if(!create_if_not_found) return NULL;
        //create a new host
        const char *path = device_get_path(device);
        struct input_host *new_host = g_new0(struct input_host, 1);
        new_host->path = g_strdup(path);
        bacpy(&new_host->src, src);
        bacpy(&new_host->dst, dst);
        new_host->device = btd_device_ref(device);
        hosts= g_slist_append(hosts, new_host);
        return new_host;
    }

    return host_list_item->data;
}

static gboolean ctrl_watch_cb(GIOChannel *chan, GIOCondition cond, gpointer data) {
    if (cond & G_IO_IN) {
        DBG("ctrl data is in ");
    }
    else if (cond & G_IO_PRI) {
        DBG("ctrl prio data is in ");
    }
    else if (cond & G_IO_HUP) {
        DBG("ctrl hang up ");
    }
    else if (cond & G_IO_ERR) {
        DBG("ctrl error ");
    }
    else if (cond & G_IO_NVAL) {
        DBG("ctrl data is invalid ");
    }
}

static gboolean intr_watch_cb(GIOChannel *chan, GIOCondition cond, gpointer data) {
    if (cond & G_IO_IN) {
        DBG("intr data is in ");
    }
    else if (cond & G_IO_PRI) {
        DBG("intr prio data is in ");
    }
    else if (cond & G_IO_HUP) {
        DBG("intr hang up ");
    }
    else if (cond & G_IO_ERR) {
        DBG("intr error ");
    }
    else if (cond & G_IO_NVAL) {
        DBG("intr data is invalid ");
    }
}


int input_host_set_channel(const bdaddr_t *src, const bdaddr_t *dst, int psm, GIOChannel *io) {
    struct input_host *host = find_host(src, dst, TRUE);
    if (host == NULL)
        return -ENOENT;

    GIOCondition cond = G_IO_IN | G_IO_PRI | G_IO_HUP | G_IO_ERR | G_IO_NVAL;

    switch (psm) {
        case L2CAP_PSM_HIDP_CTRL:
            if (host->ctrl_io)
                return -EALREADY;
            host->ctrl_io = g_io_channel_ref(io);
            host->ctrl_watch = g_io_add_watch(host->ctrl_io, cond,
                                              ctrl_watch_cb, host);
            break;
        case L2CAP_PSM_HIDP_INTR:
            if (host->intr_io)
                return -EALREADY;
            host->intr_io = g_io_channel_ref(io);
            host->intr_watch = g_io_add_watch(host->intr_io, cond,
                                              intr_watch_cb, host);
            break;
    }
    return 0;

}

//since we are not getting device remove notification, remove host when connection attempt unsuccessful
int input_host_remove(const bdaddr_t *src, const bdaddr_t *dst)
{
    struct input_host *host = find_host(src, dst, FALSE);
    if (host == NULL)
        return -ENOENT;
    char address[18];
    ba2str(dst, address);
    DBG("Removing input host %s", address);

    if (host->intr_io) {
        g_io_channel_shutdown(host->intr_io, TRUE, NULL);
        g_io_channel_unref(host->intr_io);
        host->intr_io = NULL;
    }

    if (host->ctrl_io) {
        g_io_channel_shutdown(host->ctrl_io, TRUE, NULL);
        g_io_channel_unref(host->ctrl_io);
        host->ctrl_io = NULL;
    }
    btd_adapter_remove_device(adapter_find(&host->src), host->device);
    btd_device_unref(host->device);
    g_free(host->path);

    hosts = g_slist_remove(hosts, host);
    g_free(host);
    return 0;
}

//TODO: host removal on channel disconnect or device removal ?