#include "hw.h"
#include "usb.h"
#include "qdev.h"
#include "sysemu.h"
#include "monitor.h"
#include "trace.h"

static void usb_bus_dev_print(Monitor *mon, DeviceState *qdev, int indent);

static char *usb_get_dev_path(DeviceState *dev);
static char *usb_get_fw_dev_path(DeviceState *qdev);
static int usb_qdev_exit(DeviceState *qdev);

static struct BusInfo usb_bus_info = {
    .name      = "USB",
    .size      = sizeof(USBBus),
    .print_dev = usb_bus_dev_print,
    .get_dev_path = usb_get_dev_path,
    .get_fw_dev_path = usb_get_fw_dev_path,
    .props      = (Property[]) {
        DEFINE_PROP_STRING("port", USBDevice, port_path),
        DEFINE_PROP_END_OF_LIST()
    },
};
static int next_usb_bus = 0;
static QTAILQ_HEAD(, USBBus) busses = QTAILQ_HEAD_INITIALIZER(busses);

const VMStateDescription vmstate_usb_device = {
    .name = "USBDevice",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField []) {
        VMSTATE_UINT8(addr, USBDevice),
        VMSTATE_INT32(state, USBDevice),
        VMSTATE_INT32(remote_wakeup, USBDevice),
        VMSTATE_INT32(setup_state, USBDevice),
        VMSTATE_INT32(setup_len, USBDevice),
        VMSTATE_INT32(setup_index, USBDevice),
        VMSTATE_UINT8_ARRAY(setup_buf, USBDevice, 8),
        VMSTATE_END_OF_LIST(),
    }
};

void usb_bus_new(USBBus *bus, USBBusOps *ops, DeviceState *host)
{
    qbus_create_inplace(&bus->qbus, &usb_bus_info, host, NULL);
    bus->ops = ops;
    bus->busnr = next_usb_bus++;
    bus->qbus.allow_hotplug = 1; /* Yes, we can */
    QTAILQ_INIT(&bus->free);
    QTAILQ_INIT(&bus->used);
    QTAILQ_INSERT_TAIL(&busses, bus, next);
}

USBBus *usb_bus_find(int busnr)
{
    USBBus *bus;

    if (-1 == busnr)
        return QTAILQ_FIRST(&busses);
    QTAILQ_FOREACH(bus, &busses, next) {
        if (bus->busnr == busnr)
            return bus;
    }
    return NULL;
}

static int usb_device_init(USBDevice *dev)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->init) {
        return klass->init(dev);
    }
    return 0;
}

USBDevice *usb_device_find_device(USBDevice *dev, uint8_t addr)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->find_device) {
        return klass->find_device(dev, addr);
    }
    return NULL;
}

static void usb_device_handle_destroy(USBDevice *dev)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->handle_destroy) {
        klass->handle_destroy(dev);
    }
}

void usb_device_cancel_packet(USBDevice *dev, USBPacket *p)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->cancel_packet) {
        klass->cancel_packet(dev, p);
    }
}

void usb_device_handle_attach(USBDevice *dev)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->handle_attach) {
        klass->handle_attach(dev);
    }
}

void usb_device_handle_reset(USBDevice *dev)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->handle_reset) {
        klass->handle_reset(dev);
    }
}

int usb_device_handle_control(USBDevice *dev, USBPacket *p, int request,
                              int value, int index, int length, uint8_t *data)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->handle_control) {
        return klass->handle_control(dev, p, request, value, index, length,
                                         data);
    }
    return -ENOSYS;
}

int usb_device_handle_data(USBDevice *dev, USBPacket *p)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->handle_data) {
        return klass->handle_data(dev, p);
    }
    return -ENOSYS;
}

const char *usb_device_get_product_desc(USBDevice *dev)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    return klass->product_desc;
}

const USBDesc *usb_device_get_usb_desc(USBDevice *dev)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    return klass->usb_desc;
}

void usb_device_set_interface(USBDevice *dev, int interface,
                              int alt_old, int alt_new)
{
    USBDeviceClass *klass = USB_DEVICE_GET_CLASS(dev);
    if (klass->set_interface) {
        klass->set_interface(dev, interface, alt_old, alt_new);
    }
}

static int usb_qdev_init(DeviceState *qdev)
{
    USBDevice *dev = USB_DEVICE(qdev);
    int rc;

    pstrcpy(dev->product_desc, sizeof(dev->product_desc),
            usb_device_get_product_desc(dev));
    dev->auto_attach = 1;
    QLIST_INIT(&dev->strings);
    usb_ep_init(dev);
    rc = usb_claim_port(dev);
    if (rc != 0) {
        return rc;
    }
    rc = usb_device_init(dev);
    if (rc != 0) {
        usb_release_port(dev);
        return rc;
    }
    if (dev->auto_attach) {
        rc = usb_device_attach(dev);
        if (rc != 0) {
            usb_qdev_exit(qdev);
            return rc;
        }
    }
    return 0;
}

static int usb_qdev_exit(DeviceState *qdev)
{
    USBDevice *dev = USB_DEVICE(qdev);

    if (dev->attached) {
        usb_device_detach(dev);
    }
    usb_device_handle_destroy(dev);
    if (dev->port) {
        usb_release_port(dev);
    }
    return 0;
}

typedef struct LegacyUSBFactory
{
    const char *name;
    const char *usbdevice_name;
    USBDevice *(*usbdevice_init)(const char *params);
} LegacyUSBFactory;

static GSList *legacy_usb_factory;

void usb_legacy_register(const char *typename, const char *usbdevice_name,
                         USBDevice *(*usbdevice_init)(const char *params))
{
    if (usbdevice_name) {
        LegacyUSBFactory *f = g_malloc0(sizeof(*f));
        f->name = typename;
        f->usbdevice_name = usbdevice_name;
        f->usbdevice_init = usbdevice_init;
        legacy_usb_factory = g_slist_append(legacy_usb_factory, f);
    }
}

USBDevice *usb_create(USBBus *bus, const char *name)
{
    DeviceState *dev;

#if 1
    /* temporary stopgap until all usb is properly qdev-ified */
    if (!bus) {
        bus = usb_bus_find(-1);
        if (!bus)
            return NULL;
        error_report("%s: no bus specified, using \"%s\" for \"%s\"",
                __FUNCTION__, bus->qbus.name, name);
    }
#endif

    dev = qdev_create(&bus->qbus, name);
    return USB_DEVICE(dev);
}

USBDevice *usb_create_simple(USBBus *bus, const char *name)
{
    USBDevice *dev = usb_create(bus, name);
    int rc;

    if (!dev) {
        error_report("Failed to create USB device '%s'", name);
        return NULL;
    }
    rc = qdev_init(&dev->qdev);
    if (rc < 0) {
        error_report("Failed to initialize USB device '%s'", name);
        return NULL;
    }
    return dev;
}

static void usb_fill_port(USBPort *port, void *opaque, int index,
                          USBPortOps *ops, int speedmask)
{
    port->opaque = opaque;
    port->index = index;
    port->ops = ops;
    port->speedmask = speedmask;
    usb_port_location(port, NULL, index + 1);
}

void usb_register_port(USBBus *bus, USBPort *port, void *opaque, int index,
                       USBPortOps *ops, int speedmask)
{
    usb_fill_port(port, opaque, index, ops, speedmask);
    QTAILQ_INSERT_TAIL(&bus->free, port, next);
    bus->nfree++;
}

int usb_register_companion(const char *masterbus, USBPort *ports[],
                           uint32_t portcount, uint32_t firstport,
                           void *opaque, USBPortOps *ops, int speedmask)
{
    USBBus *bus;
    int i;

    QTAILQ_FOREACH(bus, &busses, next) {
        if (strcmp(bus->qbus.name, masterbus) == 0) {
            break;
        }
    }

    if (!bus || !bus->ops->register_companion) {
        qerror_report(QERR_INVALID_PARAMETER_VALUE, "masterbus",
                      "an USB masterbus");
        if (bus) {
            error_printf_unless_qmp(
                "USB bus '%s' does not allow companion controllers\n",
                masterbus);
        }
        return -1;
    }

    for (i = 0; i < portcount; i++) {
        usb_fill_port(ports[i], opaque, i, ops, speedmask);
    }

    return bus->ops->register_companion(bus, ports, portcount, firstport);
}

void usb_port_location(USBPort *downstream, USBPort *upstream, int portnr)
{
    if (upstream) {
        snprintf(downstream->path, sizeof(downstream->path), "%s.%d",
                 upstream->path, portnr);
    } else {
        snprintf(downstream->path, sizeof(downstream->path), "%d", portnr);
    }
}

void usb_unregister_port(USBBus *bus, USBPort *port)
{
    if (port->dev)
        qdev_free(&port->dev->qdev);
    QTAILQ_REMOVE(&bus->free, port, next);
    bus->nfree--;
}

int usb_claim_port(USBDevice *dev)
{
    USBBus *bus = usb_bus_from_device(dev);
    USBPort *port;

    assert(dev->port == NULL);

    if (dev->port_path) {
        QTAILQ_FOREACH(port, &bus->free, next) {
            if (strcmp(port->path, dev->port_path) == 0) {
                break;
            }
        }
        if (port == NULL) {
            error_report("Error: usb port %s (bus %s) not found (in use?)",
                         dev->port_path, bus->qbus.name);
            return -1;
        }
    } else {
        if (bus->nfree == 1 && strcmp(object_get_typename(OBJECT(dev)), "usb-hub") != 0) {
            /* Create a new hub and chain it on */
            usb_create_simple(bus, "usb-hub");
        }
        if (bus->nfree == 0) {
            error_report("Error: tried to attach usb device %s to a bus "
                         "with no free ports", dev->product_desc);
            return -1;
        }
        port = QTAILQ_FIRST(&bus->free);
    }
    trace_usb_port_claim(bus->busnr, port->path);

    QTAILQ_REMOVE(&bus->free, port, next);
    bus->nfree--;

    dev->port = port;
    port->dev = dev;

    QTAILQ_INSERT_TAIL(&bus->used, port, next);
    bus->nused++;
    return 0;
}

void usb_release_port(USBDevice *dev)
{
    USBBus *bus = usb_bus_from_device(dev);
    USBPort *port = dev->port;

    assert(port != NULL);
    trace_usb_port_release(bus->busnr, port->path);

    QTAILQ_REMOVE(&bus->used, port, next);
    bus->nused--;

    dev->port = NULL;
    port->dev = NULL;

    QTAILQ_INSERT_TAIL(&bus->free, port, next);
    bus->nfree++;
}

int usb_device_attach(USBDevice *dev)
{
    USBBus *bus = usb_bus_from_device(dev);
    USBPort *port = dev->port;

    assert(port != NULL);
    assert(!dev->attached);
    trace_usb_port_attach(bus->busnr, port->path);

    if (!(port->speedmask & dev->speedmask)) {
        error_report("Warning: speed mismatch trying to attach "
                     "usb device %s to bus %s",
                     dev->product_desc, bus->qbus.name);
        return -1;
    }

    dev->attached++;
    usb_attach(port);

    return 0;
}

int usb_device_detach(USBDevice *dev)
{
    USBBus *bus = usb_bus_from_device(dev);
    USBPort *port = dev->port;

    assert(port != NULL);
    assert(dev->attached);
    trace_usb_port_detach(bus->busnr, port->path);

    usb_detach(port);
    dev->attached--;
    return 0;
}

int usb_device_delete_addr(int busnr, int addr)
{
    USBBus *bus;
    USBPort *port;
    USBDevice *dev;

    bus = usb_bus_find(busnr);
    if (!bus)
        return -1;

    QTAILQ_FOREACH(port, &bus->used, next) {
        if (port->dev->addr == addr)
            break;
    }
    if (!port)
        return -1;
    dev = port->dev;

    qdev_free(&dev->qdev);
    return 0;
}

static const char *usb_speed(unsigned int speed)
{
    static const char *txt[] = {
        [ USB_SPEED_LOW  ] = "1.5",
        [ USB_SPEED_FULL ] = "12",
        [ USB_SPEED_HIGH ] = "480",
        [ USB_SPEED_SUPER ] = "5000",
    };
    if (speed >= ARRAY_SIZE(txt))
        return "?";
    return txt[speed];
}

static void usb_bus_dev_print(Monitor *mon, DeviceState *qdev, int indent)
{
    USBDevice *dev = USB_DEVICE(qdev);
    USBBus *bus = usb_bus_from_device(dev);

    monitor_printf(mon, "%*saddr %d.%d, port %s, speed %s, name %s%s\n",
                   indent, "", bus->busnr, dev->addr,
                   dev->port ? dev->port->path : "-",
                   usb_speed(dev->speed), dev->product_desc,
                   dev->attached ? ", attached" : "");
}

static char *usb_get_dev_path(DeviceState *qdev)
{
    USBDevice *dev = USB_DEVICE(qdev);
    return g_strdup(dev->port->path);
}

static char *usb_get_fw_dev_path(DeviceState *qdev)
{
    USBDevice *dev = USB_DEVICE(qdev);
    char *fw_path, *in;
    ssize_t pos = 0, fw_len;
    long nr;

    fw_len = 32 + strlen(dev->port->path) * 6;
    fw_path = g_malloc(fw_len);
    in = dev->port->path;
    while (fw_len - pos > 0) {
        nr = strtol(in, &in, 10);
        if (in[0] == '.') {
            /* some hub between root port and device */
            pos += snprintf(fw_path + pos, fw_len - pos, "hub@%ld/", nr);
            in++;
        } else {
            /* the device itself */
            pos += snprintf(fw_path + pos, fw_len - pos, "%s@%ld",
                            qdev_fw_name(qdev), nr);
            break;
        }
    }
    return fw_path;
}

void usb_info(Monitor *mon)
{
    USBBus *bus;
    USBDevice *dev;
    USBPort *port;

    if (QTAILQ_EMPTY(&busses)) {
        monitor_printf(mon, "USB support not enabled\n");
        return;
    }

    QTAILQ_FOREACH(bus, &busses, next) {
        QTAILQ_FOREACH(port, &bus->used, next) {
            dev = port->dev;
            if (!dev)
                continue;
            monitor_printf(mon, "  Device %d.%d, Port %s, Speed %s Mb/s, Product %s\n",
                           bus->busnr, dev->addr, port->path, usb_speed(dev->speed),
                           dev->product_desc);
        }
    }
}

/* handle legacy -usbdevice cmd line option */
USBDevice *usbdevice_create(const char *cmdline)
{
    USBBus *bus = usb_bus_find(-1 /* any */);
    LegacyUSBFactory *f = NULL;
    GSList *i;
    char driver[32];
    const char *params;
    int len;

    params = strchr(cmdline,':');
    if (params) {
        params++;
        len = params - cmdline;
        if (len > sizeof(driver))
            len = sizeof(driver);
        pstrcpy(driver, len, cmdline);
    } else {
        params = "";
        pstrcpy(driver, sizeof(driver), cmdline);
    }

    for (i = legacy_usb_factory; i; i = i->next) {
        f = i->data;
        if (strcmp(f->usbdevice_name, driver) == 0) {
            break;
        }
    }
    if (i == NULL) {
#if 0
        /* no error because some drivers are not converted (yet) */
        error_report("usbdevice %s not found", driver);
#endif
        return NULL;
    }

    if (!f->usbdevice_init) {
        if (*params) {
            error_report("usbdevice %s accepts no params", driver);
            return NULL;
        }
        return usb_create_simple(bus, f->name);
    }
    return f->usbdevice_init(params);
}

static void usb_device_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *k = DEVICE_CLASS(klass);
    k->bus_info = &usb_bus_info;
    k->init     = usb_qdev_init;
    k->unplug   = qdev_simple_unplug_cb;
    k->exit     = usb_qdev_exit;
}

static TypeInfo usb_device_type_info = {
    .name = TYPE_USB_DEVICE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(USBDevice),
    .abstract = true,
    .class_size = sizeof(USBDeviceClass),
    .class_init = usb_device_class_init,
};

static void usb_register_types(void)
{
    type_register_static(&usb_device_type_info);
}

type_init(usb_register_types)
