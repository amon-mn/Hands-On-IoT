#include <linux/module.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/configfs.h>
#include <linux/init.h>
#include <linux/uaccess.h>

MODULE_AUTHOR("DevTITANS <devtitans@icomp.ufam.edu.br>");
MODULE_DESCRIPTION("SmartLamp USB Driver via ConfigFS");
MODULE_LICENSE("GPL");

#define VENDOR_ID   0x10c4
#define PRODUCT_ID  0xea60
#define MAX_RECV_LINE 100

static struct usb_device *smartlamp_device;
static uint usb_in, usb_out;
static char *usb_in_buffer, *usb_out_buffer;
static int usb_max_size;

static int smartlamp_send_command(const char *cmd, char *response, int response_len) {
    int actual_size;

    if (!smartlamp_device)
        return -ENODEV;

    memset(usb_out_buffer, 0, usb_max_size);
    strncpy(usb_out_buffer, cmd, usb_max_size - 1);

    usb_bulk_msg(smartlamp_device, usb_sndbulkpipe(smartlamp_device, usb_out),
                 usb_out_buffer, strlen(usb_out_buffer), &actual_size, 1000);

    memset(usb_in_buffer, 0, usb_max_size);
    usb_bulk_msg(smartlamp_device, usb_rcvbulkpipe(smartlamp_device, usb_in),
                 usb_in_buffer, usb_max_size, &actual_size, 1000);

    if (response && response_len > 0)
        strncpy(response, usb_in_buffer, response_len - 1);

    return 0;
}

// ======================= ConfigFS =======================
#include <linux/configfs.h>

struct smartlamp_item {
    struct config_group item;
};

static ssize_t led_value_show(struct config_item *item, char *buf) {
    // Read LED state is not implemented by the firmware, return dummy
    return sprintf(buf, "0\n");
}

static ssize_t led_value_store(struct config_item *item, const char *buf, size_t count) {
    if (buf[0] == '0' || buf[0] == '1') {
        char cmd[16];
        snprintf(cmd, sizeof(cmd), "SET_LED %c", buf[0]);
        smartlamp_send_command(cmd, NULL, 0);

        // Controlar o LED do teclado ScrollLock
        if (buf[0] == '1')
            printk(KERN_INFO "\033[1 q");
        else
            printk(KERN_INFO "\033[0 q");

        return count;
    }
    return -EINVAL;
}
CONFIGFS_ATTR(led_, value);

static struct configfs_attribute *led_attrs[] = {
    &led_attr_value,
    NULL,
};

static struct configfs_item_operations led_item_ops = {};

static struct config_item_type led_type = {
    .ct_item_ops = &led_item_ops,
    .ct_attrs = led_attrs,
    .ct_owner = THIS_MODULE,
};

// LDR
static ssize_t ldr_value_show(struct config_item *item, char *buf) {
    char response[64] = {0};
    smartlamp_send_command("GET_LDR", response, sizeof(response));
    return sprintf(buf, "%s\n", response);
}
CONFIGFS_ATTR_RO(ldr_, value);

static struct configfs_attribute *ldr_attrs[] = {
    &ldr_attr_value,
    NULL,
};

static struct configfs_item_operations ldr_item_ops = {};

static struct config_item_type ldr_type = {
    .ct_item_ops = &ldr_item_ops,
    .ct_attrs = ldr_attrs,
    .ct_owner = THIS_MODULE,
};

// DHT
static ssize_t dht_temperature_show(struct config_item *item, char *buf) {
    char response[64] = {0};
    smartlamp_send_command("GET_TEMP", response, sizeof(response));
    return sprintf(buf, "%s\n", response);
}
CONFIGFS_ATTR_RO(dht_, temperature);

static ssize_t dht_humidity_show(struct config_item *item, char *buf) {
    char response[64] = {0};
    smartlamp_send_command("GET_HUM", response, sizeof(response));
    return sprintf(buf, "%s\n", response);
}
CONFIGFS_ATTR_RO(dht_, humidity);

static struct configfs_attribute *dht_attrs[] = {
    &dht_attr_temperature,
    &dht_attr_humidity,
    NULL,
};

static struct configfs_item_operations dht_item_ops = {};

static struct config_item_type dht_type = {
    .ct_item_ops = &dht_item_ops,
    .ct_attrs = dht_attrs,
    .ct_owner = THIS_MODULE,
};

// Raiz
static struct config_group *smartlamp_make_group(struct config_group *group, const char *name) {
    struct smartlamp_item *item;

    item = kzalloc(sizeof(*item), GFP_KERNEL);
    if (!item)
        return NULL;

    if (strcmp(name, "led") == 0)
        config_item_init_type_name(&item->item.cg_item, name, &led_type);
    else if (strcmp(name, "ldr") == 0)
        config_item_init_type_name(&item->item.cg_item, name, &ldr_type);
    else if (strcmp(name, "dht") == 0)
        config_item_init_type_name(&item->item.cg_item, name, &dht_type);
    else {
        kfree(item);
        return NULL;
    }

    return &item->item;
}

static void smartlamp_drop_item(struct config_group *group, struct config_item *item) {
    kfree(container_of(to_config_group(item), struct smartlamp_item, item));
}

static struct configfs_group_operations smartlamp_group_ops = {
    .make_group = smartlamp_make_group,
    .drop_item = smartlamp_drop_item,
};

static struct config_item_type smartlamp_type = {
    .ct_group_ops = &smartlamp_group_ops,
    .ct_owner = THIS_MODULE,
};

static struct configfs_subsystem smartlamp_subsys = {
    .su_group = {
        .cg_item = {
            .ci_namebuf = "smartlamp",
            .ci_type = &smartlamp_type,
        },
    },
};

// USB probe/disconnect
static int smartlamp_probe(struct usb_interface *interface, const struct usb_device_id *id) {
    struct usb_host_interface *iface_desc;
    struct usb_endpoint_descriptor *endpoint;
    int i;

    smartlamp_device = interface_to_usbdev(interface);
    iface_desc = interface->cur_altsetting;

    for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
        endpoint = &iface_desc->endpoint[i].desc;
        if (usb_endpoint_is_bulk_in(endpoint)) {
            usb_in = endpoint->bEndpointAddress;
            usb_max_size = usb_endpoint_maxp(endpoint);
        } else if (usb_endpoint_is_bulk_out(endpoint)) {
            usb_out = endpoint->bEndpointAddress;
        }
    }

    usb_in_buffer = kmalloc(usb_max_size, GFP_KERNEL);
    usb_out_buffer = kmalloc(usb_max_size, GFP_KERNEL);

    return 0;
}

static void smartlamp_disconnect(struct usb_interface *interface) {
    kfree(usb_in_buffer);
    kfree(usb_out_buffer);
    smartlamp_device = NULL;
}

static struct usb_device_id smartlamp_table[] = {
    { USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
    {}
};
MODULE_DEVICE_TABLE(usb, smartlamp_table);

static struct usb_driver smartlamp_driver = {
    .name = "smartlamp",
    .id_table = smartlamp_table,
    .probe = smartlamp_probe,
    .disconnect = smartlamp_disconnect,
};

static int __init smartlamp_init(void) {
    int ret;
    configfs_register_subsystem(&smartlamp_subsys);
    ret = usb_register(&smartlamp_driver);
    return ret;
}

static void __exit smartlamp_exit(void) {
    usb_deregister(&smartlamp_driver);
    configfs_unregister_subsystem(&smartlamp_subsys);
}

module_init(smartlamp_init);
module_exit(smartlamp_exit);
