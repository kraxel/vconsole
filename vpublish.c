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

#include "mdns-publish.h"
#include "libvirt-glib-event.h"

#define APPNAME "vpublish"

/* ------------------------------------------------------------------ */

int debug = 1;

/* ------------------------------------------------------------------ */

typedef struct display display;
struct display {
    struct mdns_pub_entry *entry;
    display *next;
};

static display *domains;
static struct mdns_pub *mdns;

static void display_add_vnc(virDomainPtr d, xmlChar *port)
{
    const char *name = virDomainGetName(d);
    display *dpy = g_new0(display, 1);

    dpy->entry = mdns_pub_add(mdns, name, "_rfb._tcp", atoi(port), NULL);

    dpy->next = domains;
    domains = dpy;
}

/* ------------------------------------------------------------------ */

static void domain_check(virConnectPtr c, virDomainPtr d)
{
    static const unsigned char *xpath_spice =
        "//domain//graphics[@type='spice']";
    static const unsigned char *xpath_vnc =
        "//domain//graphics[@type='vnc']";
    const char *name = virDomainGetName(d);
    xmlXPathContextPtr ctx;
    xmlXPathObjectPtr obj;
    xmlNodePtr cur;
    xmlDocPtr xml;
    char *domain;
    int i;

    if (debug)
        fprintf(stderr, "%s: %s [enter]\n", __func__, name);

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
        fprintf(stderr, "%s: %s, %d spice nodes\n", __func__, name,
                obj->nodesetval->nodeNr);
        for (i = 0; i < obj->nodesetval->nodeNr; i++) {
            cur = obj->nodesetval->nodeTab[i];
            /* TODO */
        }
    }
    xmlXPathFreeObject(obj);

    obj = xmlXPathEvalExpression(xpath_vnc, ctx);
    if (obj && obj->nodesetval && obj->nodesetval->nodeNr) {
        fprintf(stderr, "%s: %s, %d vnc nodes\n", __func__, name,
                obj->nodesetval->nodeNr);
        for (i = 0; i < obj->nodesetval->nodeNr; i++) {
            cur = obj->nodesetval->nodeTab[i];
            fprintf(stderr, "   %d: %s:%s\n", i + 1,
                    xmlGetProp(cur, "listen"),
                    xmlGetProp(cur, "port"));
            display_add_vnc(d, xmlGetProp(cur, "port"));
        }
    }
    xmlXPathFreeObject(obj);

    xmlXPathFreeContext(ctx);
    xmlFreeDoc(xml);

    if (debug)
        fprintf(stderr, "%s: %s [done]\n", __func__, name);
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
    case VIR_DOMAIN_EVENT_UNDEFINED:
        if (debug)
            fprintf(stderr, "%s: %s: undefined\n", __func__, name);
        break;
    case VIR_DOMAIN_EVENT_STARTED:
        if (debug)
            fprintf(stderr, "%s: %s: started\n", __func__, name);
        domain_check(c, d);
        break;
    case VIR_DOMAIN_EVENT_STOPPED:
        if (debug)
            fprintf(stderr, "%s: %s: stopped\n", __func__, name);
        break;
    default:
        if (debug)
            fprintf(stderr, "%s: %s: Oops, unknown (default catch)\n",
                    __func__, name);
        break;
    }
}

/* ------------------------------------------------------------------ */

static int connect_domain_event(virConnectPtr c, virDomainPtr d,
                                int event, int detail, void *opaque)
{
    if (debug)
        fprintf(stderr, "%s: %s, event %d\n", __func__,
                virDomainGetName(d), event);
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
        domain_check(c, d);
        virDomainFree(d);
    }
    free(active);
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
    g_thread_init(NULL);
    gvir_event_register();
    mdns = mdns_pub_init(debug);
    mdns_pub_start(mdns);

    connect_init(uri);

    /* main loop */
    g_main_loop_run(mainloop);

    /* cleanup */
    exit(0);
}
