Name:       longterm

# libssh is bundled privately in the app's data directory
%define __provides_exclude_from ^%{_datadir}/.*$
%define __requires_exclude ^libssh.*$

Summary:    Longterm SSH terminal
Version:    0.1.0
Release:    1
License:    GPLv3
URL:        https://github.com/klahr/longterm
Source0:    %{name}-%{version}.tar.bz2
Requires:   sailfishsilica-qt5 >= 0.10.9
BuildRequires:  pkgconfig(sailfishapp) >= 1.0.2
BuildRequires:  pkgconfig(Qt5Core)
BuildRequires:  pkgconfig(Qt5Qml)
BuildRequires:  pkgconfig(Qt5Quick)
BuildRequires:  desktop-file-utils
BuildRequires:  cmake
BuildRequires:  perl
BuildRequires:  pkgconfig(openssl)
BuildRequires:  pkgconfig(zlib)
BuildRequires:  pkgconfig(sailfishsecrets)

%description
An SSH terminal for Sailfish OS with multiple connections, saved hosts
and keys kept in the Sailfish Secrets keychain.


%prep
%setup -q -n %{name}-%{version}

%build

%qmake5 

%make_build


%install
%qmake5_install


desktop-file-install --delete-original         --dir %{buildroot}%{_datadir}/applications                %{buildroot}%{_datadir}/applications/*.desktop

%files
%defattr(-,root,root,-)
%{_bindir}/%{name}
%{_datadir}/%{name}
%{_datadir}/applications/%{name}.desktop
%{_datadir}/icons/hicolor/*/apps/%{name}.png
