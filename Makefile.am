ACLOCAL_AMFLAGS = --install -I m4

SUBDIRS = bflibrary bfsoundlib swrendersoft hwrendersoft conf lang src util

# Do not install static libraries.
install installdirs: SUBDIRS = conf lang src util

doc_DATA = AUTHORS COPYING GPL3
docdir = $(prefix)/$(PACKAGE)

res_DATA =
resdir = $(prefix)/$(PACKAGE)

if TARGET_WINDOWS
res_DATA += res/syndwarsfx_icon.ico
else
res_DATA += res/syndwarsfx.icns res/syndwarsfx_icon.png
endif

instscript_DATA = util/install
instscriptdir = $(prefix)/$(PACKAGE)

EXTRA_DIST = doc res instscript

# Stage a Windows distribution tree for the FX3D update installer, in
# $(builddir)/pkg/syndwarsfx. Set PKGDIST_CONF to a directory to override the
# shipped conf/*.ini (e.g. PKGDIST_CONF=src/conf to use locally tuned settings).
PKGDIST_DIR = pkg
PKGDIST_CONF =
pkg-dist:
	@conf='$(PKGDIST_CONF)'; \
	if test -n "$$conf"; then opt="-c $$conf"; else opt=""; fi; \
	MAKE='$(MAKE)' $(SHELL) $(top_srcdir)/util/mkpkgdist \
	  -o '$(PKGDIST_DIR)' $$opt

# Fetch the NSIS plugins the FX3D installer needs into $(builddir)/nsis-plugins,
# where syndwarsfx-fx3d-setup.nsi picks them up with !addplugindir.
NSIS_PLUGIN_DIR = nsis-plugins
nsis-plugins:
	$(SHELL) $(top_srcdir)/util/getnsisplugins -d '$(NSIS_PLUGIN_DIR)'

.PHONY: pkg-dist nsis-plugins
