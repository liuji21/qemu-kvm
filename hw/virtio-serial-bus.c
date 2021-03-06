/*
 * A bus for connecting virtio serial and console ports
 *
 * Copyright (C) 2009, 2010 Red Hat, Inc.
 *
 * Author(s):
 *  Amit Shah <amit.shah@redhat.com>
 *
 * Some earlier parts are:
 *  Copyright IBM, Corp. 2008
 * authored by
 *  Christian Ehrhardt <ehrhardt@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#include "qemu/iov.h"
#include "monitor/monitor.h"
#include "qemu/queue.h"
#include "hw/sysbus.h"
#include "trace.h"
#include "hw/virtio-serial.h"

static VirtIOSerialPort *find_port_by_id(VirtIOSerial *vser, uint32_t id)
{
    VirtIOSerialPort *port;

    if (id == VIRTIO_CONSOLE_BAD_ID) {
        return NULL;
    }

    QTAILQ_FOREACH(port, &vser->ports, next) {
        if (port->id == id)
            return port;
    }
    return NULL;
}

static VirtIOSerialPort *find_port_by_vq(VirtIOSerial *vser, VirtQueue *vq)
{
    VirtIOSerialPort *port;

    QTAILQ_FOREACH(port, &vser->ports, next) {
        if (port->ivq == vq || port->ovq == vq)
            return port;
    }
    return NULL;
}

static bool use_multiport(VirtIOSerial *vser)
{
    return vser->vdev.guest_features & (1 << VIRTIO_CONSOLE_F_MULTIPORT);
}

static size_t write_to_port(VirtIOSerialPort *port,
                            const uint8_t *buf, size_t size)
{
    VirtQueueElement elem;
    VirtQueue *vq;
    size_t offset;

    vq = port->ivq;
    if (!virtio_queue_ready(vq)) {
        return 0;
    }

    offset = 0;
    while (offset < size) {
        size_t len;

        if (!virtqueue_pop(vq, &elem)) {
            break;
        }

        len = iov_from_buf(elem.in_sg, elem.in_num, 0,
                           buf + offset, size - offset);
        offset += len;

        virtqueue_push(vq, &elem, len);
    }

    virtio_notify(&port->vser->vdev, vq);
    return offset;
}

static void discard_vq_data(VirtQueue *vq, VirtIODevice *vdev)
{
    VirtQueueElement elem;

    if (!virtio_queue_ready(vq)) {
        return;
    }
    while (virtqueue_pop(vq, &elem)) {
        virtqueue_push(vq, &elem, 0);
    }
    virtio_notify(vdev, vq);
}

static void do_flush_queued_data(VirtIOSerialPort *port, VirtQueue *vq,
                                 VirtIODevice *vdev)
{
    VirtIOSerialPortClass *vsc;

    assert(port);
    assert(virtio_queue_ready(vq));

    vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);

    while (!port->throttled) {
        unsigned int i;

        /* Pop an elem only if we haven't left off a previous one mid-way */
        if (!port->elem.out_num) {
            if (!virtqueue_pop(vq, &port->elem)) {
                break;
            }
            port->iov_idx = 0;
            port->iov_offset = 0;
        }

        for (i = port->iov_idx; i < port->elem.out_num; i++) {
            size_t buf_size;
            ssize_t ret;

            buf_size = port->elem.out_sg[i].iov_len - port->iov_offset;
            ret = vsc->have_data(port,
                                  port->elem.out_sg[i].iov_base
                                  + port->iov_offset,
                                  buf_size);
            if (port->throttled) {
                port->iov_idx = i;
                if (ret > 0) {
                    port->iov_offset += ret;
                }
                break;
            }
            port->iov_offset = 0;
        }
        if (port->throttled) {
            break;
        }
        virtqueue_push(vq, &port->elem, 0);
        port->elem.out_num = 0;
    }
    virtio_notify(vdev, vq);
}

static void flush_queued_data(VirtIOSerialPort *port)
{
    assert(port);

    if (!virtio_queue_ready(port->ovq)) {
        return;
    }
    do_flush_queued_data(port, port->ovq, &port->vser->vdev);
}

static size_t send_control_msg(VirtIOSerial *vser, void *buf, size_t len)
{
    VirtQueueElement elem;
    VirtQueue *vq;

    vq = vser->c_ivq;
    if (!virtio_queue_ready(vq)) {
        return 0;
    }
    if (!virtqueue_pop(vq, &elem)) {
        return 0;
    }

    memcpy(elem.in_sg[0].iov_base, buf, len);

    virtqueue_push(vq, &elem, len);
    virtio_notify(&vser->vdev, vq);
    return len;
}

static size_t send_control_event(VirtIOSerial *vser, uint32_t port_id,
                                 uint16_t event, uint16_t value)
{
    struct virtio_console_control cpkt;

    stl_p(&cpkt.id, port_id);
    stw_p(&cpkt.event, event);
    stw_p(&cpkt.value, value);

    trace_virtio_serial_send_control_event(port_id, event, value);
    return send_control_msg(vser, &cpkt, sizeof(cpkt));
}

/* Functions for use inside qemu to open and read from/write to ports */
int virtio_serial_open(VirtIOSerialPort *port)
{
    /* Don't allow opening an already-open port */
    if (port->host_connected) {
        return 0;
    }
    /* Send port open notification to the guest */
    port->host_connected = true;
    send_control_event(port->vser, port->id, VIRTIO_CONSOLE_PORT_OPEN, 1);

    return 0;
}

int virtio_serial_close(VirtIOSerialPort *port)
{
    port->host_connected = false;
    /*
     * If there's any data the guest sent which the app didn't
     * consume, reset the throttling flag and discard the data.
     */
    port->throttled = false;
    discard_vq_data(port->ovq, &port->vser->vdev);

    send_control_event(port->vser, port->id, VIRTIO_CONSOLE_PORT_OPEN, 0);

    return 0;
}

/* Individual ports/apps call this function to write to the guest. */
ssize_t virtio_serial_write(VirtIOSerialPort *port, const uint8_t *buf,
                            size_t size)
{
    if (!port || !port->host_connected || !port->guest_connected) {
        return 0;
    }
    return write_to_port(port, buf, size);
}

/*
 * Readiness of the guest to accept data on a port.
 * Returns max. data the guest can receive
 */
size_t virtio_serial_guest_ready(VirtIOSerialPort *port)
{
    VirtQueue *vq = port->ivq;
    unsigned int bytes;

    if (!virtio_queue_ready(vq) ||
        !(port->vser->vdev.status & VIRTIO_CONFIG_S_DRIVER_OK) ||
        virtio_queue_empty(vq)) {
        return 0;
    }
    if (use_multiport(port->vser) && !port->guest_connected) {
        return 0;
    }
    virtqueue_get_avail_bytes(vq, &bytes, NULL, 4096, 0);
    return bytes;
}

static void flush_queued_data_bh(void *opaque)
{
    VirtIOSerialPort *port = opaque;

    flush_queued_data(port);
}

void virtio_serial_throttle_port(VirtIOSerialPort *port, bool throttle)
{
    if (!port) {
        return;
    }

    trace_virtio_serial_throttle_port(port->id, throttle);
    port->throttled = throttle;
    if (throttle) {
        return;
    }
    qemu_bh_schedule(port->bh);
}

/* Guest wants to notify us of some event */
static void handle_control_message(VirtIOSerial *vser, void *buf, size_t len)
{
    struct VirtIOSerialPort *port;
    VirtIOSerialPortClass *vsc;
    struct virtio_console_control cpkt, *gcpkt;
    uint8_t *buffer;
    size_t buffer_len;

    gcpkt = buf;

    if (len < sizeof(cpkt)) {
        /* The guest sent an invalid control packet */
        return;
    }

    cpkt.event = lduw_p(&gcpkt->event);
    cpkt.value = lduw_p(&gcpkt->value);

    trace_virtio_serial_handle_control_message(cpkt.event, cpkt.value);

    if (cpkt.event == VIRTIO_CONSOLE_DEVICE_READY) {
        if (!cpkt.value) {
            error_report("virtio-serial-bus: Guest failure in adding device %s",
                         vser->bus.qbus.name);
            return;
        }
        /*
         * The device is up, we can now tell the device about all the
         * ports we have here.
         */
        QTAILQ_FOREACH(port, &vser->ports, next) {
            send_control_event(vser, port->id, VIRTIO_CONSOLE_PORT_ADD, 1);
        }
        return;
    }

    port = find_port_by_id(vser, ldl_p(&gcpkt->id));
    if (!port) {
        error_report("virtio-serial-bus: Unexpected port id %u for device %s",
                     ldl_p(&gcpkt->id), vser->bus.qbus.name);
        return;
    }

    trace_virtio_serial_handle_control_message_port(port->id);

    vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);

    switch(cpkt.event) {
    case VIRTIO_CONSOLE_PORT_READY:
        if (!cpkt.value) {
            error_report("virtio-serial-bus: Guest failure in adding port %u for device %s",
                         port->id, vser->bus.qbus.name);
            break;
        }
        /*
         * Now that we know the guest asked for the port name, we're
         * sure the guest has initialised whatever state is necessary
         * for this port. Now's a good time to let the guest know if
         * this port is a console port so that the guest can hook it
         * up to hvc.
         */
        if (vsc->is_console) {
            send_control_event(vser, port->id, VIRTIO_CONSOLE_CONSOLE_PORT, 1);
        }

        if (port->name) {
            stl_p(&cpkt.id, port->id);
            stw_p(&cpkt.event, VIRTIO_CONSOLE_PORT_NAME);
            stw_p(&cpkt.value, 1);

            buffer_len = sizeof(cpkt) + strlen(port->name) + 1;
            buffer = g_malloc(buffer_len);

            memcpy(buffer, &cpkt, sizeof(cpkt));
            memcpy(buffer + sizeof(cpkt), port->name, strlen(port->name));
            buffer[buffer_len - 1] = 0;

            send_control_msg(vser, buffer, buffer_len);
            g_free(buffer);
        }

        if (port->host_connected) {
            send_control_event(vser, port->id, VIRTIO_CONSOLE_PORT_OPEN, 1);
        }

        /*
         * When the guest has asked us for this information it means
         * the guest is all setup and has its virtqueues
         * initialised. If some app is interested in knowing about
         * this event, let it know.
         */
        if (vsc->guest_ready) {
            vsc->guest_ready(port);
        }
        break;

    case VIRTIO_CONSOLE_PORT_OPEN:
        port->guest_connected = cpkt.value;
        if (vsc->set_guest_connected) {
            /* Send the guest opened notification if an app is interested */
            vsc->set_guest_connected(port, cpkt.value);
        }
        break;
    }
}

static void control_in(VirtIODevice *vdev, VirtQueue *vq)
{
}

static void control_out(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtQueueElement elem;
    VirtIOSerial *vser;
    uint8_t *buf;
    size_t len;

    vser = DO_UPCAST(VirtIOSerial, vdev, vdev);

    len = 0;
    buf = NULL;
    while (virtqueue_pop(vq, &elem)) {
        size_t cur_len;

        cur_len = iov_size(elem.out_sg, elem.out_num);
        /*
         * Allocate a new buf only if we didn't have one previously or
         * if the size of the buf differs
         */
        if (cur_len > len) {
            g_free(buf);

            buf = g_malloc(cur_len);
            len = cur_len;
        }
        iov_to_buf(elem.out_sg, elem.out_num, 0, buf, cur_len);

        handle_control_message(vser, buf, cur_len);
        virtqueue_push(vq, &elem, 0);
    }
    g_free(buf);
    virtio_notify(vdev, vq);
}

/* Guest wrote something to some port. */
static void handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    VirtIOSerial *vser;
    VirtIOSerialPort *port;

    vser = DO_UPCAST(VirtIOSerial, vdev, vdev);
    port = find_port_by_vq(vser, vq);

    if (!port || !port->host_connected) {
        discard_vq_data(vq, vdev);
        return;
    }

    if (!port->throttled) {
        do_flush_queued_data(port, vq, vdev);
        return;
    }
}

static void handle_input(VirtIODevice *vdev, VirtQueue *vq)
{
}

static uint32_t get_features(VirtIODevice *vdev, uint32_t features)
{
    VirtIOSerial *vser;

    vser = DO_UPCAST(VirtIOSerial, vdev, vdev);

    if (vser->bus.max_nr_ports > 1) {
        features |= (1 << VIRTIO_CONSOLE_F_MULTIPORT);
    }
    return features;
}

/* Guest requested config info */
static void get_config(VirtIODevice *vdev, uint8_t *config_data)
{
    VirtIOSerial *vser;

    vser = DO_UPCAST(VirtIOSerial, vdev, vdev);
    memcpy(config_data, &vser->config, sizeof(struct virtio_console_config));
}

static void set_config(VirtIODevice *vdev, const uint8_t *config_data)
{
    struct virtio_console_config config;

    memcpy(&config, config_data, sizeof(config));
}

static void guest_reset(VirtIOSerial *vser)
{
    VirtIOSerialPort *port;
    VirtIOSerialPortClass *vsc;

    QTAILQ_FOREACH(port, &vser->ports, next) {
        vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);
        if (port->guest_connected) {
            port->guest_connected = false;
            if (vsc->set_guest_connected) {
                vsc->set_guest_connected(port, false);
            }
        }
    }
}

static void set_status(VirtIODevice *vdev, uint8_t status)
{
    VirtIOSerial *vser;
    VirtIOSerialPort *port;

    vser = DO_UPCAST(VirtIOSerial, vdev, vdev);
    port = find_port_by_id(vser, 0);

    if (port && !use_multiport(port->vser)
        && (status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        /*
         * Non-multiport guests won't be able to tell us guest
         * open/close status.  Such guests can only have a port at id
         * 0, so set guest_connected for such ports as soon as guest
         * is up.
         */
        port->guest_connected = true;
    }
    if (!(status & VIRTIO_CONFIG_S_DRIVER_OK)) {
        guest_reset(vser);
    }
}

static void vser_reset(VirtIODevice *vdev)
{
    VirtIOSerial *vser;

    vser = DO_UPCAST(VirtIOSerial, vdev, vdev);
    guest_reset(vser);
}

static void virtio_serial_save(QEMUFile *f, void *opaque)
{
    VirtIOSerial *s = opaque;
    VirtIOSerialPort *port;
    uint32_t nr_active_ports;
    unsigned int i, max_nr_ports;

    /* The virtio device */
    virtio_save(&s->vdev, f);

    /* The config space */
    qemu_put_be16s(f, &s->config.cols);
    qemu_put_be16s(f, &s->config.rows);

    qemu_put_be32s(f, &s->config.max_nr_ports);

    /* The ports map */
    max_nr_ports = tswap32(s->config.max_nr_ports);
    for (i = 0; i < (max_nr_ports + 31) / 32; i++) {
        qemu_put_be32s(f, &s->ports_map[i]);
    }

    /* Ports */

    nr_active_ports = 0;
    QTAILQ_FOREACH(port, &s->ports, next) {
        nr_active_ports++;
    }

    qemu_put_be32s(f, &nr_active_ports);

    /*
     * Items in struct VirtIOSerialPort.
     */
    QTAILQ_FOREACH(port, &s->ports, next) {
        uint32_t elem_popped;

        qemu_put_be32s(f, &port->id);
        qemu_put_byte(f, port->guest_connected);
        qemu_put_byte(f, port->host_connected);

	elem_popped = 0;
        if (port->elem.out_num) {
            elem_popped = 1;
        }
        qemu_put_be32s(f, &elem_popped);
        if (elem_popped) {
            qemu_put_be32s(f, &port->iov_idx);
            qemu_put_be64s(f, &port->iov_offset);

            qemu_put_buffer(f, (unsigned char *)&port->elem,
                            sizeof(port->elem));
        }
    }
}

static void virtio_serial_post_load_timer_cb(void *opaque)
{
    uint32_t i;
    VirtIOSerial *s = opaque;
    VirtIOSerialPort *port;
    uint8_t host_connected;
    VirtIOSerialPortClass *vsc;

    if (!s->post_load) {
        return;
    }
    for (i = 0 ; i < s->post_load->nr_active_ports; ++i) {
        port = s->post_load->connected[i].port;
        host_connected = s->post_load->connected[i].host_connected;
        if (host_connected != port->host_connected) {
            /*
             * We have to let the guest know of the host connection
             * status change
             */
            send_control_event(s, port->id, VIRTIO_CONSOLE_PORT_OPEN,
                               port->host_connected);
        }
        vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);
        if (vsc->set_guest_connected) {
            vsc->set_guest_connected(port, port->guest_connected);
        }
    }
    g_free(s->post_load->connected);
    qemu_free_timer(s->post_load->timer);
    g_free(s->post_load);
    s->post_load = NULL;
}

static int fetch_active_ports_list(QEMUFile *f, int version_id,
                                   VirtIOSerial *s, uint32_t nr_active_ports)
{
    uint32_t i;

    s->post_load = g_malloc0(sizeof(*s->post_load));
    s->post_load->nr_active_ports = nr_active_ports;
    s->post_load->connected =
        g_malloc0(sizeof(*s->post_load->connected) * nr_active_ports);

    s->post_load->timer = qemu_new_timer_ns(vm_clock,
                                            virtio_serial_post_load_timer_cb,
                                            s);

    /* Items in struct VirtIOSerialPort */
    for (i = 0; i < nr_active_ports; i++) {
        VirtIOSerialPort *port;
        uint32_t id;

        id = qemu_get_be32(f);
        port = find_port_by_id(s, id);
        if (!port) {
            return -EINVAL;
        }

        port->guest_connected = qemu_get_byte(f);
        s->post_load->connected[i].port = port;
        s->post_load->connected[i].host_connected = qemu_get_byte(f);

        if (version_id > 2) {
            uint32_t elem_popped;

            qemu_get_be32s(f, &elem_popped);
            if (elem_popped) {
                qemu_get_be32s(f, &port->iov_idx);
                qemu_get_be64s(f, &port->iov_offset);

                qemu_get_buffer(f, (unsigned char *)&port->elem,
                                sizeof(port->elem));
                virtqueue_map_sg(port->elem.in_sg, port->elem.in_addr,
                                 port->elem.in_num, 1);
                virtqueue_map_sg(port->elem.out_sg, port->elem.out_addr,
                                 port->elem.out_num, 1);

                /*
                 *  Port was throttled on source machine.  Let's
                 *  unthrottle it here so data starts flowing again.
                 */
                virtio_serial_throttle_port(port, false);
            }
        }
    }
    qemu_mod_timer(s->post_load->timer, 1);
    return 0;
}

static int virtio_serial_load(QEMUFile *f, void *opaque, int version_id)
{
    VirtIOSerial *s = opaque;
    uint32_t max_nr_ports, nr_active_ports, ports_map;
    unsigned int i;
    int ret;

    if (version_id > 3) {
        return -EINVAL;
    }

    /* The virtio device */
    ret = virtio_load(&s->vdev, f);
    if (ret) {
        return ret;
    }

    if (version_id < 2) {
        return 0;
    }

    /* The config space */
    qemu_get_be16s(f, &s->config.cols);
    qemu_get_be16s(f, &s->config.rows);

    qemu_get_be32s(f, &max_nr_ports);
    tswap32s(&max_nr_ports);
    if (max_nr_ports > tswap32(s->config.max_nr_ports)) {
        /* Source could have had more ports than us. Fail migration. */
        return -EINVAL;
    }

    for (i = 0; i < (max_nr_ports + 31) / 32; i++) {
        qemu_get_be32s(f, &ports_map);

        if (ports_map != s->ports_map[i]) {
            /*
             * Ports active on source and destination don't
             * match. Fail migration.
             */
            return -EINVAL;
        }
    }

    qemu_get_be32s(f, &nr_active_ports);

    if (nr_active_ports) {
        ret = fetch_active_ports_list(f, version_id, s, nr_active_ports);
        if (ret) {
            return ret;
        }
    }
    return 0;
}

static void virtser_bus_dev_print(Monitor *mon, DeviceState *qdev, int indent);

static Property virtser_props[] = {
    DEFINE_PROP_UINT32("nr", VirtIOSerialPort, id, VIRTIO_CONSOLE_BAD_ID),
    DEFINE_PROP_STRING("name", VirtIOSerialPort, name),
    DEFINE_PROP_END_OF_LIST()
};

#define TYPE_VIRTIO_SERIAL_BUS "virtio-serial-bus"
#define VIRTIO_SERIAL_BUS(obj) \
      OBJECT_CHECK(VirtIOSerialBus, (obj), TYPE_VIRTIO_SERIAL_BUS)

static void virtser_bus_class_init(ObjectClass *klass, void *data)
{
    BusClass *k = BUS_CLASS(klass);
    k->print_dev = virtser_bus_dev_print;
}

static const TypeInfo virtser_bus_info = {
    .name = TYPE_VIRTIO_SERIAL_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(VirtIOSerialBus),
    .class_init = virtser_bus_class_init,
};

static void virtser_bus_dev_print(Monitor *mon, DeviceState *qdev, int indent)
{
    VirtIOSerialPort *port = DO_UPCAST(VirtIOSerialPort, dev, qdev);

    monitor_printf(mon, "%*sport %d, guest %s, host %s, throttle %s\n",
                   indent, "", port->id,
                   port->guest_connected ? "on" : "off",
                   port->host_connected ? "on" : "off",
                   port->throttled ? "on" : "off");
}

/* This function is only used if a port id is not provided by the user */
static uint32_t find_free_port_id(VirtIOSerial *vser)
{
    unsigned int i, max_nr_ports;

    max_nr_ports = tswap32(vser->config.max_nr_ports);
    for (i = 0; i < (max_nr_ports + 31) / 32; i++) {
        uint32_t map, bit;

        map = vser->ports_map[i];
        bit = ffs(~map);
        if (bit) {
            return (bit - 1) + i * 32;
        }
    }
    return VIRTIO_CONSOLE_BAD_ID;
}

static void mark_port_added(VirtIOSerial *vser, uint32_t port_id)
{
    unsigned int i;

    i = port_id / 32;
    vser->ports_map[i] |= 1U << (port_id % 32);
}

static void add_port(VirtIOSerial *vser, uint32_t port_id)
{
    mark_port_added(vser, port_id);
    send_control_event(vser, port_id, VIRTIO_CONSOLE_PORT_ADD, 1);
}

static void remove_port(VirtIOSerial *vser, uint32_t port_id)
{
    VirtIOSerialPort *port;
    unsigned int i;

    i = port_id / 32;
    vser->ports_map[i] &= ~(1U << (port_id % 32));

    port = find_port_by_id(vser, port_id);
    /*
     * This function is only called from qdev's unplug callback; if we
     * get a NULL port here, we're in trouble.
     */
    assert(port);

    /* Flush out any unconsumed buffers first */
    discard_vq_data(port->ovq, &port->vser->vdev);

    send_control_event(vser, port->id, VIRTIO_CONSOLE_PORT_REMOVE, 1);
}

static int virtser_port_qdev_init(DeviceState *qdev)
{
    VirtIOSerialPort *port = DO_UPCAST(VirtIOSerialPort, dev, qdev);
    VirtIOSerialPortClass *vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);
    VirtIOSerialBus *bus = DO_UPCAST(VirtIOSerialBus, qbus, qdev->parent_bus);
    int ret, max_nr_ports;
    bool plugging_port0;

    port->vser = bus->vser;
    port->bh = qemu_bh_new(flush_queued_data_bh, port);

    assert(vsc->have_data);

    /*
     * Is the first console port we're seeing? If so, put it up at
     * location 0. This is done for backward compatibility (old
     * kernel, new qemu).
     */
    plugging_port0 = vsc->is_console && !find_port_by_id(port->vser, 0);

    if (find_port_by_id(port->vser, port->id)) {
        error_report("virtio-serial-bus: A port already exists at id %u",
                     port->id);
        return -1;
    }

    if (port->id == VIRTIO_CONSOLE_BAD_ID) {
        if (plugging_port0) {
            port->id = 0;
        } else {
            port->id = find_free_port_id(port->vser);
            if (port->id == VIRTIO_CONSOLE_BAD_ID) {
                error_report("virtio-serial-bus: Maximum port limit for this device reached");
                return -1;
            }
        }
    }

    max_nr_ports = tswap32(port->vser->config.max_nr_ports);
    if (port->id >= max_nr_ports) {
        error_report("virtio-serial-bus: Out-of-range port id specified, max. allowed: %u",
                     max_nr_ports - 1);
        return -1;
    }

    ret = vsc->init(port);
    if (ret) {
        return ret;
    }

    port->elem.out_num = 0;

    QTAILQ_INSERT_TAIL(&port->vser->ports, port, next);
    port->ivq = port->vser->ivqs[port->id];
    port->ovq = port->vser->ovqs[port->id];

    add_port(port->vser, port->id);

    /* Send an update to the guest about this new port added */
    virtio_notify_config(&port->vser->vdev);

    return ret;
}

static int virtser_port_qdev_exit(DeviceState *qdev)
{
    VirtIOSerialPort *port = DO_UPCAST(VirtIOSerialPort, dev, qdev);
    VirtIOSerialPortClass *vsc = VIRTIO_SERIAL_PORT_GET_CLASS(port);
    VirtIOSerial *vser = port->vser;

    qemu_bh_delete(port->bh);
    remove_port(port->vser, port->id);

    QTAILQ_REMOVE(&vser->ports, port, next);

    if (vsc->exit) {
        vsc->exit(port);
    }
    return 0;
}

VirtIODevice *virtio_serial_init(DeviceState *dev, virtio_serial_conf *conf)
{
    VirtIOSerial *vser;
    VirtIODevice *vdev;
    uint32_t i, max_supported_ports;

    if (!conf->max_virtserial_ports)
        return NULL;

    /* Each port takes 2 queues, and one pair is for the control queue */
    max_supported_ports = VIRTIO_PCI_QUEUE_MAX / 2 - 1;

    if (conf->max_virtserial_ports > max_supported_ports) {
        error_report("maximum ports supported: %u", max_supported_ports);
        return NULL;
    }

    vdev = virtio_common_init("virtio-serial", VIRTIO_ID_CONSOLE,
                              sizeof(struct virtio_console_config),
                              sizeof(VirtIOSerial));

    vser = DO_UPCAST(VirtIOSerial, vdev, vdev);

    /* Spawn a new virtio-serial bus on which the ports will ride as devices */
    qbus_create_inplace(&vser->bus.qbus, TYPE_VIRTIO_SERIAL_BUS, dev, NULL);
    vser->bus.qbus.allow_hotplug = 1;
    vser->bus.vser = vser;
    QTAILQ_INIT(&vser->ports);

    vser->bus.max_nr_ports = conf->max_virtserial_ports;
    vser->ivqs = g_malloc(conf->max_virtserial_ports * sizeof(VirtQueue *));
    vser->ovqs = g_malloc(conf->max_virtserial_ports * sizeof(VirtQueue *));

    /* Add a queue for host to guest transfers for port 0 (backward compat) */
    vser->ivqs[0] = virtio_add_queue(vdev, 128, handle_input);
    /* Add a queue for guest to host transfers for port 0 (backward compat) */
    vser->ovqs[0] = virtio_add_queue(vdev, 128, handle_output);

    /* TODO: host to guest notifications can get dropped
     * if the queue fills up. Implement queueing in host,
     * this might also make it possible to reduce the control
     * queue size: as guest preposts buffers there,
     * this will save 4Kbyte of guest memory per entry. */

    /* control queue: host to guest */
    vser->c_ivq = virtio_add_queue(vdev, 32, control_in);
    /* control queue: guest to host */
    vser->c_ovq = virtio_add_queue(vdev, 32, control_out);

    for (i = 1; i < vser->bus.max_nr_ports; i++) {
        /* Add a per-port queue for host to guest transfers */
        vser->ivqs[i] = virtio_add_queue(vdev, 128, handle_input);
        /* Add a per-per queue for guest to host transfers */
        vser->ovqs[i] = virtio_add_queue(vdev, 128, handle_output);
    }

    vser->config.max_nr_ports = tswap32(conf->max_virtserial_ports);
    vser->ports_map = g_malloc0(((conf->max_virtserial_ports + 31) / 32)
        * sizeof(vser->ports_map[0]));
    /*
     * Reserve location 0 for a console port for backward compat
     * (old kernel, new qemu)
     */
    mark_port_added(vser, 0);

    vser->vdev.get_features = get_features;
    vser->vdev.get_config = get_config;
    vser->vdev.set_config = set_config;
    vser->vdev.set_status = set_status;
    vser->vdev.reset = vser_reset;

    vser->qdev = dev;

    vser->post_load = NULL;

    /*
     * Register for the savevm section with the virtio-console name
     * to preserve backward compat
     */
    register_savevm(dev, "virtio-console", -1, 3, virtio_serial_save,
                    virtio_serial_load, vser);

    return vdev;
}

void virtio_serial_exit(VirtIODevice *vdev)
{
    VirtIOSerial *vser = DO_UPCAST(VirtIOSerial, vdev, vdev);

    unregister_savevm(vser->qdev, "virtio-console", vser);

    g_free(vser->ivqs);
    g_free(vser->ovqs);
    g_free(vser->ports_map);
    if (vser->post_load) {
        g_free(vser->post_load->connected);
        qemu_del_timer(vser->post_load->timer);
        qemu_free_timer(vser->post_load->timer);
        g_free(vser->post_load);
    }
    virtio_cleanup(vdev);
}

static void virtio_serial_port_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *k = DEVICE_CLASS(klass);
    k->init = virtser_port_qdev_init;
    k->bus_type = TYPE_VIRTIO_SERIAL_BUS;
    k->exit = virtser_port_qdev_exit;
    k->unplug = qdev_simple_unplug_cb;
    k->props = virtser_props;
}

static const TypeInfo virtio_serial_port_type_info = {
    .name = TYPE_VIRTIO_SERIAL_PORT,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(VirtIOSerialPort),
    .abstract = true,
    .class_size = sizeof(VirtIOSerialPortClass),
    .class_init = virtio_serial_port_class_init,
};

static void virtio_serial_register_types(void)
{
    type_register_static(&virtser_bus_info);
    type_register_static(&virtio_serial_port_type_info);
}

type_init(virtio_serial_register_types)
