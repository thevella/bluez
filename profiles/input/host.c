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
#include "btio/btio.h"
#include "gdbus/gdbus.h"
#include "src/dbus-common.h"
#include "src/shared/uhid.h"
#include <unistd.h>
#include "hidp_defs.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#define INPUT_HOST_INTERFACE "org.bluez.InputHost1"


struct input_host{
    struct btd_device	*device;
    char			*path;
    bdaddr_t		src;
    bdaddr_t		dst;
    char            dst_address[18];
    GIOChannel		*ctrl_io;
    GIOChannel		*intr_io;
    guint			ctrl_watch;
    guint			intr_watch;
    gchar			*socket_path_ctrl;
    gchar			*socket_path_intr;
    GIOChannel      *ctrl_io_local_listener;
    guint           ctrl_io_local_listener_watch;
    GIOChannel      *ctrl_io_local_connection;
    guint           ctrl_io_local_connection_watch;
    GIOChannel      *intr_io_local_listener;
    guint           intr_io_local_listener_watch;
    GIOChannel      *intr_io_local_connection;
    guint           intr_io_local_connection_watch;


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

static void shutdown_channels(struct input_host *host);
static void shutdown_local_connections(struct input_host *host);
static bool hidp_recv_ctrl_message(GIOChannel *chan, struct input_host *host);
static bool hidp_send_intr_message(struct input_host *host, uint8_t hdr, const uint8_t *data, size_t size);


static gboolean property_get_socket_path_ctrl(const GDBusPropertyTable *property, DBusMessageIter *iter, void *data)
{
    struct input_host *host = data;
    dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &host->socket_path_ctrl);
    return TRUE;
}

static gboolean property_get_socket_path_intr(const GDBusPropertyTable *property, DBusMessageIter *iter, void *data)
{
    struct input_host *host = data;
    dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &host->socket_path_intr);
    return TRUE;
}


static const GDBusPropertyTable input_properties[] = {
        { "SocketPathCtrl", "s", property_get_socket_path_ctrl },
        { "SocketPathIntr", "s", property_get_socket_path_intr },
        { }
};


static bool local_recv_message(GIOChannel *chan, struct input_host *host,  bool is_ctrl)
{
    int fd;
    ssize_t len;
    uint8_t hdr, type, param;
    uint8_t data[UHID_DATA_MAX + 1];

    fd = g_io_channel_unix_get_fd(chan);

    len = read(fd, data, sizeof(data));
    if (len < 0) {
        error("local socket read error: %s (%d)", strerror(errno), errno);
        return FALSE;
    }

    if (len == 0) {
        DBG("local socket read returned 0 bytes");
        return TRUE;
    }

    //send data  remote
    if(!is_ctrl){
        hdr = HIDP_TRANS_DATA | HIDP_DATA_RTYPE_INPUT;
        hidp_send_intr_message(host, hdr, data, len);
    }
    return TRUE;
}

static gboolean local_event_cb(GIOChannel *io, GIOCondition cond,
                                   gpointer user_data, bool is_ctrl) {
    struct input_host *host = user_data;
    if (cond == G_IO_IN || cond == G_IO_PRI || cond == (G_IO_IN | G_IO_PRI)) {
        DBG("local %s data is in ", is_ctrl ? "ctrl":"intr");
        local_recv_message(io, host, is_ctrl);
        return TRUE;
    }
    if(cond & G_IO_NVAL){
        DBG("local %s invalid message ", is_ctrl ? "ctrl":"intr");
        return TRUE;
    }
    //local channel disconnected - shut down local connections
    DBG("local %s channel disconnected - shutting down local connections", is_ctrl ? "ctrl":"intr");
    shutdown_local_connections(host);
    return FALSE;
}
static gboolean local_ctrl_event_cb(GIOChannel *io, GIOCondition cond,
                                    gpointer user_data) {
    return local_event_cb(io,cond,user_data,TRUE);
}

static gboolean local_intr_event_cb(GIOChannel *io, GIOCondition cond,
                                   gpointer user_data) {
    return local_event_cb(io,cond,user_data,FALSE);
}


gboolean local_channel_connect(GIOChannel *io, GIOCondition triggered_cond, bool is_ctrl, struct input_host *host) {
    /* If the user closed the connection */
    if ((triggered_cond & G_IO_NVAL) || check_nval(io)) {
        shutdown_local_connections(host);
        return TRUE;
    }

    GIOChannel *cli_io;
    int srv_sock, cli_sock;

    srv_sock = g_io_channel_unix_get_fd(io);

    cli_sock = accept(srv_sock, NULL, NULL);
    if (cli_sock < 0)
        return TRUE;


    cli_io = g_io_channel_unix_new(cli_sock);

    g_io_channel_set_close_on_unref(cli_io, TRUE);
    g_io_channel_set_flags(cli_io, G_IO_FLAG_NONBLOCK, NULL);

    GIOCondition cond = G_IO_IN | G_IO_PRI | G_IO_HUP | G_IO_ERR | G_IO_NVAL;

    if(is_ctrl){
        if (host->ctrl_io_local_connection)
            error("local ctrl socket exists");
        host->ctrl_io_local_connection = cli_io;
        host->ctrl_io_local_connection_watch = g_io_add_watch(host->ctrl_io_local_connection, cond,
                                          local_ctrl_event_cb, host);
        DBG("local ctrl channel connected");
    }
    else{
        if (host->intr_io_local_connection)
            error("local ctrl socket exists");
        host->intr_io_local_connection = cli_io;
        host->intr_io_local_connection_watch = g_io_add_watch(host->intr_io_local_connection, cond,
                                          local_intr_event_cb, host);
        DBG("local intr channel connected");
    }
    return TRUE;
}



static gboolean local_ctrl_connect(GIOChannel *io, GIOCondition cond,
                          gpointer user_data) {
    struct input_host *host = user_data;
    return local_channel_connect(io,cond, TRUE, host);
}

static gboolean local_intr_connect(GIOChannel *io, GIOCondition cond,
                                 gpointer user_data) {
    struct input_host *host = user_data;
    return local_channel_connect(io,cond, FALSE, host);
}


static GIOChannel *create_socket(struct input_host *host, bool is_ctrl){
    int sock;
    GIOChannel *io;
    struct sockaddr_un addr;

    sock = socket(PF_LOCAL, SOCK_SEQPACKET, 0);
    if (sock < 0) {
        error("Unable to open %s socket for input host %s: %s", is_ctrl ? "Ctrl":"Intr", host->path, strerror(errno));
        return NULL;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, is_ctrl ? host->socket_path_ctrl : host->socket_path_intr, 33);

    unlink(addr.sun_path);
    if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        error("Failed to bind %s socket for input host %s : %s", is_ctrl ? "Ctrl":"Intr", host->path, strerror(errno));
        close(sock);
        return NULL;
    }

    if (chmod(addr.sun_path, 0666) < 0)
        error("Failed to change mode");


    io = g_io_channel_unix_new(sock);

    g_io_channel_set_close_on_unref(io, TRUE);
    g_io_channel_set_flags(io, G_IO_FLAG_NONBLOCK, NULL);

    if (listen(sock, 5) < 0) {
        error("Failed to listen to %s socket for input host %s: %s", is_ctrl ? "Ctrl":"Intr", host->path, strerror(errno));
        g_io_channel_unref(io);
        return NULL;
    }

    if(is_ctrl){
        host->ctrl_io_local_listener_watch = g_io_add_watch_full(io, G_PRIORITY_DEFAULT_IDLE, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL, is_ctrl ? local_ctrl_connect : local_intr_connect, host, NULL);
        host->ctrl_io_local_listener = io;
    }
    else{
        host->intr_io_local_listener_watch = g_io_add_watch_full(io, G_PRIORITY_DEFAULT_IDLE, G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL, is_ctrl ? local_ctrl_connect : local_intr_connect, host, NULL);
        host->intr_io_local_listener = io;
    }
    return io;
}


static void register_socket_and_dbus_interface(struct input_host *host){
    if(create_socket(host, TRUE) == NULL){
        error("Unable to register ctrl socket %s interface", INPUT_HOST_INTERFACE);
        return;
    }
    if(create_socket(host, FALSE) == NULL){
        error("Unable to register intr socket %s interface", INPUT_HOST_INTERFACE);
        return;
    }

    if (g_dbus_register_interface(btd_get_dbus_connection(),
                                  host->path, INPUT_HOST_INTERFACE,
                                  NULL, NULL,
                                  input_properties, host,
                                  NULL) == FALSE) {
        error("Unable to register %s interface", INPUT_HOST_INTERFACE);
    }
}

static int cmp_host_addr(gconstpointer a, gconstpointer dst)
{
    const struct input_host *host = a;
    return bacmp(&host->dst, dst);
}

static gboolean device_property_state_changed(DBusConnection *conn, DBusMessage *msg,
                                               void *user_data) {
    struct input_host *host = user_data;
    if (host->device == NULL) {
        shutdown_channels(host);
    }
    else {
        bool connected = btd_device_is_connected(host->device);
        if (!connected) shutdown_channels(host);
    }
    return TRUE;
}


static struct input_host *create_input_host(const bdaddr_t *src, const bdaddr_t *dst){
    //create a new host
    struct btd_device *device = btd_adapter_find_device(adapter_find(src), dst, BDADDR_BREDR);
    const char *path = device_get_path(device);
    struct input_host *new_host = g_new0(struct input_host, 1);
    new_host->path = g_strdup(path);
    bacpy(&new_host->src, src);
    bacpy(&new_host->dst, dst);
    ba2str(&new_host->dst, new_host->dst_address);
    new_host->device = btd_device_ref(device);
    new_host->socket_path_ctrl = g_strjoin(NULL,"/tmp/BTIHS_", new_host->dst_address, "_Ctrl", NULL);
    new_host->socket_path_intr = g_strjoin(NULL,"/tmp/BTIHS_", new_host->dst_address, "_Intr", NULL);
    hosts= g_slist_append(hosts, new_host);


    g_dbus_add_properties_watch(btd_get_dbus_connection(), NULL,
                            new_host->path, DEVICE_INTERFACE,
                            device_property_state_changed, new_host,
                            NULL);

    return new_host;
}

static struct input_host *find_host(const bdaddr_t *src, const bdaddr_t *dst, bool create_if_not_found){
    GSList *host_list_item = g_slist_find_custom(hosts, dst, cmp_host_addr);
    struct btd_device *device = btd_adapter_find_device(adapter_find(src), dst, BDADDR_BREDR);

    if (device == NULL)
        return NULL;

    if (host_list_item == NULL) {
        if(!create_if_not_found) return NULL;
        return create_input_host(src, dst);
    }

    return host_list_item->data;
}

static gboolean ctrl_watch_cb(GIOChannel *chan, GIOCondition cond, gpointer data) {
    struct input_host *host = data;

    if (cond == G_IO_IN || cond == G_IO_PRI || cond == (G_IO_IN | G_IO_PRI)) {
        DBG("ctrl data is in ");
        hidp_recv_ctrl_message(chan, host);
        return TRUE;
    }
    else if(cond & G_IO_NVAL){
        DBG("ctrl invalid message ");
        return TRUE;
    }
    DBG("Input host ctrl %s disconnected. Shutting down channels", host->dst_address);
    shutdown_channels(host);
    //input_host_remove(&host->src, &host->dst);
    return FALSE;
}

static bool hidp_send_message(GIOChannel *chan, uint8_t hdr,
                              const uint8_t *data, size_t size)
{
    int fd;
    ssize_t len;
    uint8_t msg[size + 1];

    if (!chan) {
        error("BT socket not connected");
        return false;
    }

    if (data == NULL)
        size = 0;

    msg[0] = hdr;
    if (size > 0)
        memcpy(&msg[1], data, size);
    ++size;

    fd = g_io_channel_unix_get_fd(chan);

    len = write(fd, msg, size);
    if (len < 0) {
        error("BT socket write error: %s (%d)", strerror(errno), errno);
        return false;
    }

    if ((size_t) len < size) {
        error("BT socket write error: partial write (%zd of %zu bytes)",
              len, size);
        return false;
    }

    return true;
}


static bool hidp_send_ctrl_message(struct input_host *host, uint8_t hdr,
                                   const uint8_t *data, size_t size)
{
    return hidp_send_message(host->ctrl_io, hdr, data, size);
}

static bool hidp_send_intr_message(struct input_host *host, uint8_t hdr,
                                   const uint8_t *data, size_t size)
{
    return hidp_send_message(host->intr_io, hdr, data, size);
}



static bool hidp_recv_ctrl_message(GIOChannel *chan, struct input_host *host)
{
    int fd;
    ssize_t len;
    uint8_t hdr, type, param;
    uint8_t data[UHID_DATA_MAX + 1];

    fd = g_io_channel_unix_get_fd(chan);

    len = read(fd, data, sizeof(data));
    if (len < 0) {
        error("BT socket read error: %s (%d)", strerror(errno), errno);
        return false;
    }

    if (len == 0) {
        DBG("BT socket read returned 0 bytes");
        return true;
    }

    hdr = data[0];
    type = hdr & HIDP_HEADER_TRANS_MASK;
    param = hdr & HIDP_HEADER_PARAM_MASK;

    switch (type) {
        case HIDP_TRANS_HID_CONTROL:
            //virtual cable is not supported yet
//            if (param == HIDP_CTRL_VIRTUAL_CABLE_UNPLUG)
//                connection_disconnect(idev, 0);
//            hidp_recv_ctrl_handshake(idev, param);
            break;
        case HIDP_TRANS_GET_REPORT:
            //TODO: get last report - page 30
            hidp_send_ctrl_message(host, HIDP_TRANS_HANDSHAKE |
                                         HIDP_HSHK_SUCCESSFUL, NULL, 0);
            break;
        case HIDP_TRANS_SET_REPORT:
            //TODO: set received report - page 31
            hidp_send_ctrl_message(host, HIDP_TRANS_HANDSHAKE |
                                         HIDP_HSHK_SUCCESSFUL, NULL, 0);
            break;
        case HIDP_TRANS_DATA:
                //data is part of input (device->host)or output (host-> device) reports (get_report and set_report messages)
//            hidp_recv_ctrl_data(idev, param, data, len);
            break;

        default:
            //boot protocol is not supported - hence get_protocol and set_protocol messages are treated as unsupported
            error("unsupported HIDP control message");
            hidp_send_ctrl_message(host, HIDP_TRANS_HANDSHAKE |
                                         HIDP_HSHK_ERR_UNSUPPORTED_REQUEST, NULL, 0);
            break;
    }

    return true;
}


static bool hidp_recv_intr_data(GIOChannel *chan, struct input_host *host)
{
    int fd;
    ssize_t len;
    uint8_t hdr;
    uint8_t data[UHID_DATA_MAX + 1];

    fd = g_io_channel_unix_get_fd(chan);

    len = read(fd, data, sizeof(data));
    if (len < 0) {
        error("BT socket read error: %s (%d)", strerror(errno), errno);
        return false;
    }

    if (len == 0) {
        DBG("BT socket read returned 0 bytes");
        return true;
    }

    hdr = data[0];
    if (hdr != (HIDP_TRANS_DATA | HIDP_DATA_RTYPE_INPUT)) {
        DBG("unsupported HIDP protocol header 0x%02x", hdr);
        return true;
    }

    if (len < 2) {
        DBG("received empty HID report");
        return true;
    }

    //uhid_send_input_report(idev, data + 1, len - 1);

    return true;
}



static gboolean intr_watch_cb(GIOChannel *chan, GIOCondition cond, gpointer data) {
    struct input_host *host = data;
    if (cond == G_IO_IN || cond == G_IO_PRI || cond == (G_IO_IN | G_IO_PRI)) {
        DBG("intr data is in ");
        hidp_recv_intr_data(chan, host);
        return TRUE;
    }
    else if(cond & G_IO_NVAL){
        DBG("intr invalid message ");
        return TRUE;
    }
    DBG("Input host intr %s disconnected. Shutting down channels", host->dst_address);
    shutdown_channels(host);
    //input_host_remove(&host->src, &host->dst);
    return FALSE;
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

//            GError *gerr = NULL;
//            if (!bt_io_set(host->intr_io, &gerr,
//                           BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
//                           BT_IO_OPT_INVALID)) {
//                error("cannot set encryption required by keyboard: %s", gerr->message);
//                g_error_free(gerr);
//                return -EFAULT;
//            }

            break;
    }

    if(host->ctrl_io!= NULL && host->intr_io!=NULL){
        register_socket_and_dbus_interface(host);
    }
    return 0;

}

static void shutdown_local_connections(struct input_host *host) {
    if (host->intr_io_local_connection) {
        g_io_channel_shutdown(host->intr_io_local_connection, TRUE, NULL);
        g_io_channel_unref(host->intr_io_local_connection);
        host->intr_io_local_connection = NULL;
    }

    if (host->intr_io_local_connection_watch > 0)
        g_source_remove(host->intr_io_local_connection_watch);
    host->intr_io_local_connection_watch = 0;


    if (host->ctrl_io_local_connection) {
        g_io_channel_shutdown(host->ctrl_io_local_connection, TRUE, NULL);
        g_io_channel_unref(host->ctrl_io_local_connection);
        host->ctrl_io_local_connection = NULL;
    }

    if (host->ctrl_io_local_connection_watch > 0)
        g_source_remove(host->ctrl_io_local_connection_watch);
    host->ctrl_io_local_connection_watch = 0;
}

static void shutdown_channels(struct input_host *host){
    g_dbus_unregister_interface(btd_get_dbus_connection(),
                                host->path, INPUT_HOST_INTERFACE);

    if (host->ctrl_watch > 0)
        g_source_remove(host->ctrl_watch);
    host->ctrl_watch = 0;

    if (host->intr_watch > 0)
        g_source_remove(host->intr_watch);
    host->intr_watch = 0;

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

    shutdown_local_connections(host);

    if (host->intr_io_local_listener) {
        g_io_channel_shutdown(host->intr_io_local_listener, TRUE, NULL);
        g_io_channel_unref(host->intr_io_local_listener);
        host->intr_io_local_listener = NULL;
    }

    if (host->intr_io_local_listener_watch > 0)
        g_source_remove(host->intr_io_local_listener_watch);
    host->intr_io_local_listener_watch = 0;

    if (host->ctrl_io_local_listener) {
        g_io_channel_shutdown(host->ctrl_io_local_listener, TRUE, NULL);
        g_io_channel_unref(host->ctrl_io_local_listener);
        host->ctrl_io_local_listener = NULL;
    }

    if (host->ctrl_io_local_listener_watch > 0)
        g_source_remove(host->ctrl_io_local_listener_watch);
    host->ctrl_io_local_listener_watch = 0;


}

//since we are not getting device remove notification, remove host when connection attempt unsuccessful
int input_host_remove(const bdaddr_t *src, const bdaddr_t *dst)
{
    struct input_host *host = find_host(src, dst, FALSE);
    if (host == NULL)
        return -ENOENT;
    DBG("Removing input host %s", host->dst_address);
    shutdown_channels(host);

    //btd_adapter_remove_device(adapter_find(&host->src), host->device);
    btd_device_unref(host->device);
    g_free(host->path);
    if(host->socket_path_ctrl != NULL) g_free(host->socket_path_ctrl);
    if(host->socket_path_intr!=NULL) g_free(host->socket_path_intr);

    hosts = g_slist_remove(hosts, host);
    g_free(host);
    return 0;
}

//TODO: host removal on channel disconnect or device removal ?