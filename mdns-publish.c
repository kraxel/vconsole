#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <syslog.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/signal.h>

#include <avahi-client/client.h>
#include <avahi-client/publish.h>

#include <avahi-common/alternative.h>
#include <avahi-common/thread-watch.h>
#include <avahi-common/malloc.h>
#include <avahi-common/error.h>

#include "list.h"
#include "mdns-publish.h"

/* --------------------------------------------------------------------- */

struct mdns_pub {
    int                    have_tty;
    int                    have_syslog;
    int                    debug;

    AvahiThreadedPoll      *thread_poll;
    AvahiClient            *client;

    struct list_head       entries;
};

struct mdns_pub_entry {
    struct list_head       next;

    char                   *name;
    const char             *service;
    int                    port;
    char                   *txt[4];

    struct mdns_pub        *mdns;
    AvahiEntryGroup        *group;
};

char *mdns_pub_appname;
int mdns_pub_termsig;
int mdns_pub_appquit;

/* ------------------------------------------------------------------ */

static char *group_state_name[] = {
    [ AVAHI_ENTRY_GROUP_UNCOMMITED ]  = "uncommited",
    [ AVAHI_ENTRY_GROUP_REGISTERING ] = "registering",
    [ AVAHI_ENTRY_GROUP_ESTABLISHED ] = "established",
    [ AVAHI_ENTRY_GROUP_COLLISION ]   = "collision",
    [ AVAHI_ENTRY_GROUP_FAILURE ]     = "failure",
};

static char *client_state_name[] = {
    [ AVAHI_CLIENT_S_REGISTERING ] = "server registering",
    [ AVAHI_CLIENT_S_RUNNING ]     = "server running",
    [ AVAHI_CLIENT_S_COLLISION ]   = "server collision",
    [ AVAHI_CLIENT_FAILURE ]       = "failure",
    [ AVAHI_CLIENT_CONNECTING ]    = "connecting",
};

static void update_services(AvahiClient *c, struct mdns_pub *mdns);

static void entry_group_callback(AvahiEntryGroup *g,
				 AvahiEntryGroupState state,
				 void *userdata)
{
    struct mdns_pub_entry *entry = userdata;
    char *n;

    mdns_log_printf(entry->mdns, LOG_DEBUG, "%s: %s: state %d [%s]\n", __FUNCTION__,
		    entry->name, state, group_state_name[state]);

    switch (state) {
    case AVAHI_ENTRY_GROUP_COLLISION:
	n = avahi_alternative_service_name(entry->name);
	mdns_log_printf(entry->mdns, LOG_NOTICE,
			"service name collision, renaming '%s' to '%s'\n",
			entry->name, n);
	avahi_free(entry->name);
	entry->name = n;
	avahi_entry_group_reset(entry->group);
	update_services(avahi_entry_group_get_client(g), entry->mdns);
	break;
    case AVAHI_ENTRY_GROUP_FAILURE:
	mdns_pub_appquit++;
	break;
    default:
	break;
    }
}

static void update_services(AvahiClient *c, struct mdns_pub *mdns)
{
    struct list_head *item;
    struct mdns_pub_entry *entry;
    AvahiEntryGroupState state;
    int ret;

    if (AVAHI_CLIENT_S_RUNNING != avahi_client_get_state(c))
	return;

    list_for_each(item, &mdns->entries) {
	entry = list_entry(item, struct mdns_pub_entry, next);
	
	/* If this is the first time we're called, let's create a new entry group */
	if (!entry->group) {
	    entry->group = avahi_entry_group_new(c, entry_group_callback, entry);
	    if (!entry->group) {
		mdns_log_printf(mdns, LOG_ERR, "avahi_entry_group_new() failed: %s\n",
				avahi_strerror(avahi_client_errno(c)));
		goto fail;
	    }
	}

	/* something to do ? */
	state = avahi_entry_group_get_state(entry->group);
	mdns_log_printf(mdns, LOG_DEBUG, "%s: %s: %d [%s]\n", __FUNCTION__,
			entry->name, state, group_state_name[state]);
	if (AVAHI_ENTRY_GROUP_UNCOMMITED != state)
	    continue;

	/* Add the service */
	ret = avahi_entry_group_add_service(entry->group,
					    AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0,
					    entry->name, entry->service,
					    NULL, NULL,
					    entry->port,
					    entry->txt[0], entry->txt[1],
					    entry->txt[2], entry->txt[3],
					    NULL);
	if (ret < 0) {
	    mdns_log_printf(mdns, LOG_ERR, "failed to add '%s' service: %s\n",
			    entry->name, avahi_strerror(ret));
	    goto fail;
	}

	/* Tell the server to register the service */
	ret = avahi_entry_group_commit(entry->group);
	if (ret < 0) {
	    mdns_log_printf(mdns, LOG_ERR, "failed to commit entry_group: %s\n",
			    avahi_strerror(ret));
	    goto fail;
	}
    }
    return;

fail:
    mdns_pub_appquit++;
}

static void reset_services(struct mdns_pub *mdns, int free_groups)
{
    struct list_head *item;
    struct mdns_pub_entry *entry;

    list_for_each(item, &mdns->entries) {
	entry = list_entry(item, struct mdns_pub_entry, next);
	avahi_entry_group_reset(entry->group);
	if (!free_groups)
	    continue;
	avahi_entry_group_free(entry->group);
	entry->group = NULL;
    }
}

static void client_callback(AvahiClient *c,
			    AvahiClientState state,
			    void * userdata)
{
    struct mdns_pub *mdns = userdata;
    int error;
    
    mdns_log_printf(mdns, LOG_DEBUG, "%s: state %d [%s]\n", __FUNCTION__,
		    state, client_state_name[state]);

    switch (state) {
    case AVAHI_CLIENT_CONNECTING:
	mdns_log_printf(mdns, LOG_NOTICE,
			"avahi daemon not running (yet), I'll keep trying ...\n");
	break;
    case AVAHI_CLIENT_S_RUNNING:
	update_services(c, mdns);
	break;
    case AVAHI_CLIENT_S_COLLISION:
	reset_services(mdns, 0);
	break;
    case AVAHI_CLIENT_FAILURE:
	switch (avahi_client_errno(c)) {
	case AVAHI_ERR_DISCONNECTED:
	    reset_services(mdns, 1);
	    avahi_client_free(c);

	    mdns_log_printf(mdns, LOG_NOTICE, "disconnected from avahi daemon, reconnecting ...\n");
	    mdns->client = avahi_client_new(avahi_threaded_poll_get(mdns->thread_poll),
					    AVAHI_CLIENT_NO_FAIL,
					    client_callback, mdns, &error);
	    if (!mdns->client) {
		mdns_log_printf(mdns, LOG_ERR, "failed to create client: %s\n",
				avahi_strerror(error));
		goto fail;
	    }
	    break;
	default:
	    mdns_log_printf(mdns, LOG_ERR, "client failure: %s\n",
			    avahi_strerror(avahi_client_errno(c)));
	    goto fail;
	    break;
	}
	break;
    default:
	break;
    }
    return;

 fail:
    mdns_pub_appquit++;
}

/* ------------------------------------------------------------------ */

struct mdns_pub *mdns_pub_init(int debug)
{
    struct mdns_pub *mdns;
    int error;

    mdns = avahi_malloc(sizeof(*mdns));
    if (NULL == mdns) {
        fprintf(stderr, "%s: out of memory\n", mdns_pub_appname);
	goto fail;
    }
    memset(mdns, 0, sizeof(*mdns));
    INIT_LIST_HEAD(&mdns->entries);
    mdns->debug = debug;
    mdns->have_tty = isatty(2);
    
    openlog(mdns_pub_appname, 0, LOG_LOCAL0);
    mdns->have_syslog = 1;

    mdns->thread_poll = avahi_threaded_poll_new();
    if (!mdns->thread_poll) {
	mdns_log_printf(mdns, LOG_ERR, "failed to create simple poll object\n");
	goto fail;
    }

    mdns->client = avahi_client_new(avahi_threaded_poll_get(mdns->thread_poll),
				    AVAHI_CLIENT_NO_FAIL,
				    client_callback, mdns, &error);
    if (!mdns->client) {
	mdns_log_printf(mdns, LOG_ERR, "failed to create client: %s\n", avahi_strerror(error));
	goto fail;
    }
    return mdns;

 fail:
    mdns_pub_fini(mdns);
    return NULL;
}

int mdns_pub_start(struct mdns_pub *mdns)
{
    return avahi_threaded_poll_stop(mdns->thread_poll);
}

int mdns_pub_stop(struct mdns_pub *mdns)
{
    return avahi_threaded_poll_stop(mdns->thread_poll);
}

void mdns_pub_fini(struct mdns_pub *mdns)
{
    if (!mdns)
	return;
    mdns_pub_del_all(mdns);
    if (mdns->client)
        avahi_client_free(mdns->client);
    if (mdns->thread_poll)
        avahi_threaded_poll_free(mdns->thread_poll);
    avahi_free(mdns);
}

/* --------------------------------------------------------------------- */

struct mdns_pub_entry *mdns_pub_add(struct mdns_pub *mdns,
				    const char *name,
                                    const char *service,
                                    int port,
				    ...)
{
    struct mdns_pub_entry *entry;
    va_list args;
    char *txt;
    int i;

    entry = avahi_malloc(sizeof(*entry));
    if (NULL == entry)
	return NULL;
    memset(entry, 0, sizeof(*entry));

    entry->name = avahi_strdup(name);
    entry->service = service;
    entry->port = port;
    entry->mdns = mdns;

    va_start(args, port);
    for (i = 0; i < sizeof(entry->txt)/sizeof(entry->txt[0]); i++) {
	txt = va_arg(args,char*);
	if (NULL == txt)
	    break;
        entry->txt[i] = txt;
    }
    va_end(args);

    list_add_tail(&entry->next, &mdns->entries);
    if (mdns->client)
	update_services(mdns->client, mdns);

    mdns_log_printf(entry->mdns, LOG_DEBUG, "%s: %s\n", __FUNCTION__, entry->name);
    return entry;
}

void mdns_pub_del(struct mdns_pub_entry *entry)
{
    mdns_log_printf(entry->mdns, LOG_DEBUG, "%s: %s\n", __FUNCTION__, entry->name);
    if (entry->group) {
	avahi_entry_group_reset(entry->group);
	avahi_entry_group_free(entry->group);
	entry->group = NULL;
    }
    avahi_free(entry->name);
    list_del(&entry->next);
    avahi_free(entry);
}

void mdns_pub_del_all(struct mdns_pub *mdns)
{
    struct mdns_pub_entry *entry;
    
    while (!list_empty(&mdns->entries)) {
	entry = list_entry(mdns->entries.next, struct mdns_pub_entry, next);
	mdns_pub_del(entry);
    }
}

/* --------------------------------------------------------------------- */

int mdns_log_printf(struct mdns_pub *mdns, int priority,
		    char *fmt, ...)
{
    va_list args;
    char msgbuf[1024];
    int rc;

    va_start(args, fmt);
    rc = vsnprintf(msgbuf, sizeof(msgbuf), fmt, args);
    va_end(args);

    if (!mdns || (mdns->have_tty && (mdns->debug ||
                                     priority != LOG_DEBUG)))
	fprintf(stderr, "%s: %s", mdns_pub_appname, msgbuf);
    if (mdns && mdns->have_syslog)
	syslog(priority, "%s", msgbuf);
    return rc;
}

int mdns_daemonize(void)
{
    switch (fork()) {
    case -1:
	mdns_log_printf(NULL, LOG_ERR, "fork: %s", strerror(errno));
	return -1;
    case 0:
	/* child */
	close(0); close(1); close(2);
	setsid();
	open("/dev/null", O_RDWR); dup(0); dup(0);
	return 0;
    default:
	/* parent */
	exit(0);
    }
}

static void catchsig(int signal)
{
    mdns_pub_termsig = signal;
    mdns_pub_appquit = 1;
}

void mdns_sigsetup(struct mdns_pub *mdns)
{
    struct sigaction act,old;

    memset(&act,0,sizeof(act));
    sigemptyset(&act.sa_mask);
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE,&act,&old);
    act.sa_handler = catchsig;
    sigaction(SIGHUP,&act,&old);
    sigaction(SIGUSR1,&act,&old);
    sigaction(SIGTERM,&act,&old);
    if (mdns->debug)
	sigaction(SIGINT,&act,&old);
}
