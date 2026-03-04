{
  description = "Leanstore flake";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-25.11";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = inputs @ { self, nixpkgs, flake-utils }: 
  (flake-utils.lib.eachDefaultSystem (system:
  let 
    pkgs = nixpkgs.legacyPackages.${system};
  in {
  devShells = {
    default = pkgs.mkShell {
      name = "leanstore-devshell";
      buildInputs = with pkgs; [
      	just
	gdb

        cmake
        onetbb
        coreutils
        gflags
        gtest
        gbenchmark
        liburing
        zstd
        openssl
        fuse
	peazip

        wiredtiger
        sqlite
        rocksdb
        libaio
      ];
    };
  };
  }));
}
