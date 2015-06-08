struct mdns_pub;
struct mdns_pub_entry;

extern char *mdns_pub_appname;
extern int mdns_pub_termsig;
extern int mdns_pub_appquit;

/* initialization and cleanup */
struct mdns_pub *mdns_pub_init(int debug);
int mdns_pub_start(struct mdns_pub *mdns);
int mdns_pub_stop(struct mdns_pub *mdns);
void mdns_pub_fini(struct mdns_pub *mdns);

/* add and remove services */
struct mdns_pub_entry *mdns_pub_add(struct mdns_pub *mdns,
				    const char *name,
                                    const char *service,
                                    int port,
				    ...);
void mdns_pub_del(struct mdns_pub_entry *entry);
void mdns_pub_del_all(struct mdns_pub *mdns);

/* misc helper functions */
int __attribute__ ((format (printf, 3, 0)))
mdns_log_printf(struct mdns_pub *mdns, int priority,
		char *fmt, ...);
int mdns_daemonize(void);
void mdns_sigsetup(struct mdns_pub *mdns);
