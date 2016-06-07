#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include <inttypes.h>

#include <sys/stat.h>
#include <sys/time.h>

#include <glib.h>

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#include "list.h"
#include "mdns-publish.h"
#include "libvirt-glib-event.h"

#define APPNAME "vpublish"

/* ------------------------------------------------------------------ */

int debug = 0;

/* ------------------------------------------------------------------ */

typedef struct mdns_service mdns_service;
struct mdns_service {
    char uuid[VIR_UUID_STRING_BUFLEN];
    struct mdns_pub_entry *entry;
    struct list_head next;
};

static struct list_head services = LIST_HEAD_INIT(services);
static struct mdns_pub *mdns;

static void add_service(virDomainPtr d, xmlChar *service, xmlChar *port)
{
    const char *name = virDomainGetName(d);
    mdns_service *s = g_new0(mdns_service, 1);

    virDomainGetUUIDString(d, s->uuid);
    s->entry = mdns_pub_add(mdns, name, service, atoi(port), NULL);

    list_add(&s->next, &services);
}

static void del_services(virDomainPtr d)
{
    char uuid[VIR_UUID_STRING_BUFLEN];
    struct list_head *item, *tmp;
    mdns_service *s;

    virDomainGetUUIDString(d, uuid);
    list_for_each_safe(item, tmp, &services) {
	s = list_entry(item, mdns_service, next);
        if (strcmp(s->uuid, uuid) != 0)
            continue;
        mdns_pub_del(s->entry);
        list_del(&s->next);
        g_free(s);
    }
}

/* ------------------------------------------------------------------ */

static void domain_display(virDomainPtr d, xmlXPathObjectPtr obj,
                           xmlChar *service)
{
    const char *name = virDomainGetName(d);
    xmlChar *listen, *port;
    xmlNodePtr cur;
    int i;

    for (i = 0; i < obj->nodesetval->nodeNr; i++) {
        cur = obj->nodesetval->nodeTab[i];
        listen = xmlGetProp(cur, "listen");
        port = xmlGetProp(cur, "port");
        if (strcmp(listen, "127.0.0.1") == 0) {
            if (debug)
                fprintf(stderr, "   %d: skip (@localhost, ipv4)\n", i + 1);
            continue;
        }
        if (strcmp(listen, "::1") == 0) {
            if (debug)
                fprintf(stderr, "   %d: skip (@localhost, ipv6)\n", i + 1);
            continue;
        }
        if (debug)
            fprintf(stderr, "   %d: announce \"%s\", port \"%s\", guest \"%s\"\n",
                    i + 1, service, port, name);
        add_service(d, service, port);
    }
}

static void domain_check(virDomainPtr d)
{
    static const unsigned char *xpath_spice =
        "//domain//graphics[@type='spice']";
    static const unsigned char *xpath_vnc =
        "//domain//graphics[@type='vnc']";
    const char *name = virDomainGetName(d);
    xmlXPathContextPtr ctx;
    xmlXPathObjectPtr obj;
    xmlDocPtr xml;
    char *domain;

    domain = virDomainGetXMLDesc(d, 0);
    if (!domain) {
        if (debug)
            fprintf(stderr, "%s: %s virDomainGetXMLDesc failure\n", __func__, name);
        return;
    }

    xml = xmlReadMemory(domain, strlen(domain), NULL, NULL, 0);
    if (!xml) {
        if (debug)
            fprintf(stderr, "%s: %s xmlReadMemory failure\n", __func__, name);
        goto err_domain;
    }

    ctx = xmlXPathNewContext(xml);

    obj = xmlXPathEvalExpression(xpath_spice, ctx);
    if (obj && obj->nodesetval && obj->nodesetval->nodeNr) {
        if (debug)
            fprintf(stderr, "%s: %s, %d spice nodes\n", __func__, name,
                    obj->nodesetval->nodeNr);
        /* TODO */
    }
    xmlXPathFreeObject(obj);

    obj = xmlXPathEvalExpression(xpath_vnc, ctx);
    if (obj && obj->nodesetval && obj->nodesetval->nodeNr) {
        if (debug)
            fprintf(stderr, "%s: %s, %d vnc nodes\n", __func__, name,
                    obj->nodesetval->nodeNr);
        domain_display(d, obj, "_rfb._tcp");
    }
    xmlXPathFreeObject(obj);

    xmlXPathFreeContext(ctx);
    xmlFreeDoc(xml);
    return;

err_domain:
    free(domain);
    return;
}

static void domain_update(virConnectPtr c, virDomainPtr d, virDomainEventType event)
{
    const char *name = virDomainGetName(d);

    /* handle events */
    switch (event) {

        /* publish */
    case VIR_DOMAIN_EVENT_STARTED:
        if (debug)
            fprintf(stderr, "%s: %s: started\n", __func__, name);
        domain_check(d);
        break;

        /* unpublish */
    case VIR_DOMAIN_EVENT_STOPPED:
        if (debug)
            fprintf(stderr, "%s: %s: stopped\n", __func__, name);
        del_services(d);
        break;
    case VIR_DOMAIN_EVENT_CRASHED:
        if (debug)
            fprintf(stderr, "%s: %s: crashed\n", __func__, name);
        del_services(d);
        break;

        /* ignore */
    case VIR_DOMAIN_EVENT_SUSPENDED:
    case VIR_DOMAIN_EVENT_RESUMED:
    case VIR_DOMAIN_EVENT_PMSUSPENDED:
    case VIR_DOMAIN_EVENT_SHUTDOWN:
        break;

        /* log */
    case VIR_DOMAIN_EVENT_UNDEFINED:
        if (debug)
            fprintf(stderr, "%s: %s: undefined\n", __func__, name);
        break;
    default:
        if (debug)
            fprintf(stderr, "%s: %s: Oops, unknown (default catch): %d\n",
                    __func__, name, event);
        break;
    }
}

/* ------------------------------------------------------------------ */

static int connect_domain_event(virConnectPtr c, virDomainPtr d,
                                int event, int detail, void *opaque)
{
    domain_update(c, d, event);
    return 0;
}

static void connect_list(virConnectPtr c)
{
    virDomainPtr d;
    int i, n;
    int *active;

    n = virConnectNumOfDomains(c);
    active = malloc(sizeof(int) * n);
    n = virConnectListDomains(c, active, n);
    for (i = 0; i < n; i++) {
        d = virDomainLookupByID(c, active[i]);
        domain_check(d);
        virDomainFree(d);
    }
    free(active);
}

static void connect_close(virConnectPtr c, int reason, void *opaque)
{
    if (debug)
        fprintf(stderr, "%s:\n", __func__);
    exit(0);
}

static void connect_init(const char *uri)
{
    virConnectPtr c;

    c = virConnectOpen(uri);
    if (c == NULL) {
        fprintf(stderr, "Failed to open connection to %s\n", uri);
        exit(1);
    }
    if (debug)
        fprintf(stderr, "%s: connected to %s\n", __func__, uri);

    virConnectDomainEventRegister(c, connect_domain_event,
                                  NULL, NULL);
    virConnectRegisterCloseCallback(c, connect_close,
                                    NULL, NULL);
    connect_list(c);
}

/* ------------------------------------------------------------------ */

static void usage(FILE *fp)
{
    fprintf(fp,
	    "This is a virtual machine display publisher.\n"
	    "It'll announce vnc screens via mdns (aka zeroconf/bonjour).\n"
	    "\n"
	    "usage: %s [ options ]\n"
	    "options:\n"
	    "   -h          Print this text.\n"
	    "   -d          Enable debugging.\n"
	    "   -c <uri>    Connect to libvirt.\n"
	    "\n"
	    "-- \n"
	    "(c) 2015 Gerd Hoffmann <kraxel@redhat.com>\n",
            APPNAME);
}

int
main(int argc, char *argv[])
{
    GMainLoop *mainloop;
    char *uri = NULL;
    int c;

    for (;;) {
        if (-1 == (c = getopt(argc, argv, "hdc:")))
            break;
        switch (c) {
	case 'd':
	    debug++;
	    break;
        case 'c':
            uri = optarg;
            break;
        case 'h':
            usage(stdout);
            exit(0);
        default:
            usage(stderr);
            exit(1);
        }
    }

    if (uri == NULL)
        uri = getenv("LIBVIRT_DEFAULT_URI");
    if (uri == NULL)
        uri = getenv("VIRSH_DEFAULT_CONNECT_URI");

    if (uri == NULL) {
        fprintf(stderr, "No libvirt uri\n");
        exit(1);
    }

    /* init */
    mainloop = g_main_loop_new(NULL, false);
    gvir_event_register();

    mdns_pub_appname = APPNAME;
    mdns = mdns_pub_init(debug);
    mdns_pub_start(mdns);

    connect_init(uri);

    /* main loop */
    g_main_loop_run(mainloop);

    /* cleanup */
    exit(0);
}
