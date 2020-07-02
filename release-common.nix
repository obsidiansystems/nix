{ pkgs, enableStatic }:

with pkgs;

rec {
  # Use "busybox-sandbox-shell" if present,
  # if not (legacy) fallback and hope it's sufficient.
  sh = pkgs.busybox-sandbox-shell or (busybox.override {
    useMusl = true;
    enableStatic = true;
    enableMinimal = true;
    extraConfig = ''
      CONFIG_FEATURE_FANCY_ECHO y
      CONFIG_FEATURE_SH_MATH y
      CONFIG_FEATURE_SH_MATH_64 y

      CONFIG_ASH y
      CONFIG_ASH_OPTIMIZE_FOR_SIZE y

      CONFIG_ASH_ALIAS y
      CONFIG_ASH_BASH_COMPAT y
      CONFIG_ASH_CMDCMD y
      CONFIG_ASH_ECHO y
      CONFIG_ASH_GETOPTS y
      CONFIG_ASH_INTERNAL_GLOB y
      CONFIG_ASH_JOB_CONTROL y
      CONFIG_ASH_PRINTF y
      CONFIG_ASH_TEST y
    '';
  });

  configureFlags =
    lib.optionals (!enableStatic && stdenv.isLinux) [
      "--with-sandbox-shell=${sh}/bin/busybox"
    ];

  nativeBuildDeps =
    [
      buildPackages.bison
      buildPackages.flex
      buildPackages.libxml2
      buildPackages.libxslt
      buildPackages.docbook5
      buildPackages.docbook_xsl_ns
      buildPackages.autoreconfHook
      buildPackages.pkgconfig

      # Tests
      buildPackages.git
      buildPackages.mercurial
      buildPackages.ipfs
      buildPackages.jq
    ];

  buildDeps =
    [ autoreconfHook
      autoconf-archive

      curl
      bzip2 xz brotli zlib editline
      openssl sqlite
      libarchive
      boost
      (nlohmann_json.overrideAttrs (_: rec {
        version = "3.8.0";
        src = fetchFromGitHub {
          owner = "nlohmann";
          repo = "json";
          rev = "v${version}";
          sha256 = "09l2kf7hrnf8xb7k6b2271imggyqcyvz83xf9k730prdzxy997y7";
        };
      }))
      gmock
    ]
    ++ lib.optionals stdenv.isLinux [libseccomp utillinuxMinimal]
    ++ lib.optional (stdenv.isLinux || stdenv.isDarwin) libsodium
    ++ lib.optional (!enableStatic && (stdenv.isLinux || stdenv.isDarwin))
      ((aws-sdk-cpp.override {
        apis = ["s3" "transfer"];
        customMemoryManagement = false;
      }).overrideDerivation (args: {
        /*
        patches = args.patches or [] ++ [ (fetchpatch {
          url = https://github.com/edolstra/aws-sdk-cpp/commit/3e07e1f1aae41b4c8b340735ff9e8c735f0c063f.patch;
          sha256 = "1pij0v449p166f9l29x7ppzk8j7g9k9mp15ilh5qxp29c7fnvxy2";
        }) ];
        */
      }));

  propagatedDeps =
    [ (boehmgc.override { enableLargeConfig = true; })
    ];

  perlDeps =
    [ perl
      perlPackages.DBDSQLite
    ];
}
