# Publishing to Launchpad

PPA: https://launchpad.net/~neilruffell/+archive/ubuntu/xfce-classic-menu

Launchpad does **not** accept prebuilt .deb files. Upload a signed **source**
package; Launchpad's builders compile the plugin and publish .deb packages.

The `debian/` directory is part of the GitHub repository and contains package
metadata, dependencies, the original author's copyright/license and installation
instructions. The version and Ubuntu series are set in `debian/changelog`.

## First upload (Ubuntu 24.04 / noble)

1. Register your own OpenPGP public key with Launchpad:
   https://launchpad.net/~/+editpgpkeys

2. On Linux Mint / Ubuntu, install packaging tools and build dependencies:

   ```sh
   sudo apt update
   sudo apt install devscripts dput debhelper build-essential pkg-config \
     libgtk-3-dev libxfce4panel-2.0-dev libxfce4ui-2-dev \
     libgarcon-1-0-dev libexo-2-dev libwnck-3-dev
   ```

3. Check out the approved packaging branch (or main once merged), then
   ensure that the distribution and package version in `debian/changelog`
   match the Ubuntu target. Update the maintainer email to the email
   associated with your signing key / Launchpad account.

4. Run package checks and create a signed **source-only** upload:

   ```sh
   dpkg-buildpackage -b -us -uc  # local binary build test
   debuild -S -sa               # signed source package for Launchpad
   ```

   The source build creates `../xfce4-classic-menu-plugin_*_source.changes`.
   If signing fails, use `debuild -S -sa -kYOUR_KEY_ID` with the registered key.

5. Upload the resulting signed source package:

   ```sh
   dput ppa:neilruffell/xfce-classic-menu ../xfce4-classic-menu-plugin_*_source.changes
   ```

   Verify build results on the PPA page. Only then advertise installation:

   ```sh
   sudo add-apt-repository ppa:neilruffell/xfce-classic-menu
   sudo apt update
   sudo apt install xfce4-classic-menu-plugin
   ```

## Important

- The initial changelog targets **noble (24.04)** only. Other Ubuntu releases
  require appropriate dependency/build checks and unique per-series versions.
- Do not upload until the package has passed a local build test.
- For each new release increment the package version before uploading.
- PPA uploads require an OpenPGP signature from a key registered on Launchpad.
- The package installs into the XFCE panel plugin paths through the existing
  upstream Makefile, which honours `DESTDIR` for packaging.
