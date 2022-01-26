#! /bin/bash
pip uninstall rst2html5
apt install pkg-config libglib2.0-dev libgio-cil python-gobject-dev libgnutls28-dev libxml2-dev libxml2-utils xsltproc libdevmapper-dev libpciaccess-dev python3-docutils
./autogen.sh --enable-debug
#glib-2.0 gobject-2.0 gio-2.0 >= 2.48.0
(cd build-aux; ../configure --enable-debug --prefix=/;make -j )
