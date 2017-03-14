Name:         vconsole
License:      GPLv2+
Version:      0.7
Release:      1%{?dist}
Summary:      Virtual machine serial console manager
Group:        Applications/System
URL:          http://www.kraxel.org/blog/linux/%{name}/
Source:       http://www.kraxel.org/releases/%{name}/%{name}-%{version}.tar.gz

BuildRequires: pkgconfig(glib-2.0)
BuildRequires: pkgconfig(gthread-2.0)
BuildRequires: pkgconfig(gtk+-3.0)
BuildRequires: pkgconfig(libvirt)
BuildRequires: pkgconfig(libxml-2.0)
BuildRequires: pkgconfig(avahi-client)
BuildRequires: pkgconfig(avahi-glib)
BuildRequires: pkgconfig(vte-2.91)

%description
Virtual machine serial console manager

%package -n vpublish
Summary: Virtual machine gfx console publisher
%description -n vpublish
Publish virtual machine gfx console (vnc) via avahi.

%prep
%setup -q

%build
export CFLAGS="%{optflags}"
make prefix=/usr

%install
make prefix=/usr DESTDIR=%{buildroot} STRIP="" install

%files
%{_bindir}/vconsole
%{_mandir}/man1/vconsole.1*
/usr/share/applications/vconsole.desktop

%files -n vpublish
%{_bindir}/vpublish
%{_unitdir}/vpublish.service

%changelog
