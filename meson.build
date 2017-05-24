# -*-python-*-
project('vconsole', 'c')

# init configuration
config       = configuration_data()
version      = run_command('cat', 'VERSION')
config.set_quoted('VERSION', version.stdout().strip())

# depedencies
glib_dep         = dependency('glib-2.0')
gthread_dep      = dependency('gthread-2.0')
gtk3_dep         = dependency('gtk+-3.0')
vte_dep          = dependency('vte-2.91')
libvirt_dep      = dependency('libvirt')
libxml_dep       = dependency('libxml-2.0')
avahi_client_dep = dependency('avahi-client')
avahi_glib_dep   = dependency('avahi-glib')

# finish & write configuration
configure_file(output : 'config.h', configuration : config)
add_global_arguments(['-include', 'config.h'], language : 'c')

# build & install
vconsole_srcs = [ 'vconsole.c', 'connect.c', 'domain.c', 'libvirt-glib-event.c' ]
vpublish_srcs = [ 'vpublish.c', 'mdns-publish.c', 'libvirt-glib-event.c' ]

vconsole_deps = [ glib_dep, gthread_dep, gtk3_dep, vte_dep, libvirt_dep ]
vpublish_deps = [ glib_dep, gthread_dep, libvirt_dep, libxml_dep,
                  avahi_client_dep, avahi_glib_dep ]

executable('vconsole',
           sources      : vconsole_srcs,
           dependencies : vconsole_deps,
           install      : true)
install_man('vconsole.1')

executable('vpublish',
           sources      : vpublish_srcs,
           dependencies : vpublish_deps,
           install      : true)
