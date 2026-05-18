extern "C" const char kEmbeddedRootfsSeed[] = R"SEED(md /home
md /root
md /user
md /user/binaries
md /user/shared
md /user/shared/docs
md /user/shared/docs/wirth
md /var
md /var/lib
md /var/lib/wirth
md /var/lib/wirth/pkgdb
md /etc
md /etc/apt
md /etc/apt/sources.list.d
md /optional
md /optional/demo
)SEED";

extern "C" const unsigned kEmbeddedRootfsSeedSize = sizeof(kEmbeddedRootfsSeed) - 1u;
