libsys4
=======

This is a library to facilitate code sharing between xsystem4 and alice-tools.
You could use it if you want but the interfaces are neither documented nor
stable.

Building
--------

First install the dependencies (corresponding Debian package in parentheses):

* bison (bison)
* flex (flex)
* meson (meson)
* libturbojpeg (libturbojpeg0-dev)
* libwebp (libwebp-dev)
* libpng (libpng-dev)
* zlib (zlib1g-dev)

Then build the libsys4.a static library with meson,

    mkdir build
    meson build
    ninja -C build

Usage
-----

This project is meant to be used as a subproject in meson. Otherwise you can
link it as you would any other static library.
