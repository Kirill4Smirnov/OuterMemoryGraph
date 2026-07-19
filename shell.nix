{pkgs ? import <nixpkgs> {}}: let
  catch2Package =
    if builtins.hasAttr "catch2_3" pkgs
    then pkgs.catch2_3
    else pkgs.catch2;

  pythonEnv = pkgs.python3.withPackages (pythonPackages:
    with pythonPackages; [
      networkx
      psutil
      pytest
    ]);
in
  pkgs.mkShell {
    nativeBuildInputs = with pkgs; [
      gcc
      cmake
      ninja
      pkg-config
      ccache

      clang-tools
      gdb
      valgrind
      strace
      perf
      lcov
      hyperfine
      time

      git
      curl
      wget
      jq
      zstd
      gnutar
      gzip
      unzip
      file

      shellcheck
      ruff
      pythonEnv
    ];

    buildInputs = [
      catch2Package
      pkgs.nlohmann_json
      pkgs.libcap
    ];

    shellHook = ''
      export CMAKE_GENERATOR=Ninja
      export CTEST_OUTPUT_ON_FAILURE=1
      export PYTHONNOUSERSITE=1
    '';
  }
