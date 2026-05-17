with import ./config.nix;

rec {

  # Want to ensure that "out" doesn't get a suffix on it's path.
  nameCheck = mkDerivation {
    name = "multiple-outputs-a";
    outputs = [
      "out"
      "dev"
    ];
    builder = builtins.toFile "builder.sh" ''
      mkdir $first $second
      test -z $all
      echo "first" > $first/file
      echo "second" > $second/file
      ln -s $first $second/link
    '';
    helloString = "Hello, world!";
  };

  a = mkDerivation {
    name = "multiple-outputs-a";
    outputs = [
      "first"
      "second"
    ];
    builder = builtins.toFile "builder.sh" ''
      mkdir $first $second
      test -z $all
      echo "first" > $first/file
      echo "second" > $second/file
      ln -s $first $second/link
    '';
    helloString = "Hello, world!";
  };

  use-a = mkDerivation {
    name = "use-a";
    inherit (a) first second;
    builder = builtins.toFile "builder.sh" ''
      cat $first/file $second/file >$out
    '';
  };

  b = mkDerivation {
    defaultOutput =
      assert a.second.helloString == "Hello, world!";
      a;
    firstOutput =
      assert a.outputName == "first";
      a.first.first;
    secondOutput =
      assert a.second.outputName == "second";
      a.second.first.first.second.second.first.second;
    allOutputs = a.all;
    name = "multiple-outputs-b";
    builder = builtins.toFile "builder.sh" ''
      mkdir $out
      test "$firstOutput $secondOutput" = "$allOutputs"
      test "$defaultOutput" = "$firstOutput"
      test "$(cat $firstOutput/file)" = "first"
      test "$(cat $secondOutput/file)" = "second"
      echo "success" > $out/file
    '';
  };

  c = mkDerivation {
    name = "multiple-outputs-c";
    drv = b.drvPath;
    builder = builtins.toFile "builder.sh" ''
      mkdir $out
      ln -s $drv $out/drv
    '';
  };

  d = mkDerivation {
    name = "multiple-outputs-d";
    drv = builtins.unsafeDiscardOutputDependency b.drvPath;
    builder = builtins.toFile "builder.sh" ''
      mkdir $out
      echo $drv > $out/drv
    '';
  };

  # Test for cycle detection with detailed error messages
  cyclic =
    (mkDerivation {
      name = "cyclic-outputs";
      outputs = [
        "out"
        "dev"
        "bin"
      ];
      builder = builtins.toFile "builder.sh" ''
        mkdir $out $dev $bin

        # cycles: out → dev → bin → out

        name=fullpaths
        mkdir {$out,$dev,$bin}/$name
        echo $dev/$name/dev-to-bin > $out/$name/out-to-dev
        echo $bin/$name/bin-to-out > $dev/$name/dev-to-bin
        echo $out/$name/out-to-dev > $bin/$name/bin-to-out

        name=relpaths
        mkdir {$out,$dev,$bin}/$name
        echo ../../$(basename $dev) > $out/$name/out-to-dev
        echo ../../$(basename $bin) > $dev/$name/dev-to-bin
        echo ../../$(basename $out) > $bin/$name/bin-to-out

        name=symlinks
        mkdir {$out,$dev,$bin}/$name
        ln -s $dev $out/$name/out-to-dev
        ln -s $bin $dev/$name/dev-to-bin
        ln -s $out $bin/$name/bin-to-out

        name=hashes
        mkdir {$out,$dev,$bin}/$name
        basename $dev | head -c32 > $out/$name/out-to-dev
        basename $bin | head -c32 > $dev/$name/dev-to-bin
        basename $out | head -c32 > $bin/$name/bin-to-out

        name=relsymlinks
        mkdir {$out,$dev,$bin}/$name
        ln -s -r $dev $out/$name/out-to-dev
        ln -s -r $bin $dev/$name/dev-to-bin
        ln -s -r $out $bin/$name/bin-to-out

        # cycles: out → bin → dev → out

        name=fullpaths2
        mkdir {$out,$dev,$bin}/$name
        echo $bin/$name/bin-to-dev > $out/$name/out-to-bin
        echo $dev/$name/dev-to-out > $bin/$name/bin-to-dev
        echo $out/$name/out-to-bin > $dev/$name/dev-to-out
      '';
    }).out;

  cyclic-fullpaths-buildInputs =
    (mkDerivation {
      name = "cyclic-fullpaths-buildInputs";
      outputs = [
        "out"
        "dev"
        "bin"
      ];
      buildCommand = ''
        mkdir $out $dev $bin
        # cycle: out → dev → bin → out
        name=fullpaths
        mkdir {$out,$dev,$bin}/$name
        echo $dev/$name/dev-to-bin > $out/$name/out-to-dev
        echo $bin/$name/bin-to-out > $dev/$name/dev-to-bin
        echo $out/$name/out-to-dev > $bin/$name/bin-to-out

        # no cycle
        name=fullpaths-buildInputs
        mkdir {$out,$dev,$bin}/$name
        echo ${cyclic-fullpaths-buildInputs-a} > $out/$name/a
        echo ${cyclic-fullpaths-buildInputs-b} > $out/$name/a
        echo ${cyclic-fullpaths-buildInputs-c} > $out/$name/a
      '';
    }).out;

  cyclic-fullpaths-buildInputs-a =
    (mkDerivation {
      name = "cyclic-fullpaths-buildInputs-a";
      buildCommand = "echo a > $out";
    }).out;

  cyclic-fullpaths-buildInputs-b =
    (mkDerivation {
      name = "cyclic-fullpaths-buildInputs-b";
      buildCommand = "echo b > $out";
    }).out;

  cyclic-fullpaths-buildInputs-c =
    (mkDerivation {
      name = "cyclic-fullpaths-buildInputs-c";
      buildCommand = "echo c > $out";
    }).out;

  e = mkDerivation {
    name = "multiple-outputs-e";
    outputs = [
      "a_a"
      "b"
      "c"
    ];
    meta.outputsToInstall = [
      "a_a"
      "b"
    ];
    buildCommand = "mkdir $a_a $b $c";
  };

  nothing-to-install = mkDerivation {
    name = "nothing-to-install";
    meta.outputsToInstall = [ ];
    buildCommand = "mkdir $out";
  };

  independent = mkDerivation {
    name = "multiple-outputs-independent";
    outputs = [
      "first"
      "second"
    ];
    builder = builtins.toFile "builder.sh" ''
      mkdir $first $second
      test -z $all
      echo "first" > $first/file
      echo "second" > $second/file
    '';
  };

  use-independent = mkDerivation {
    name = "use-independent";
    inherit (a) first second;
    builder = builtins.toFile "builder.sh" ''
      cat $first/file $second/file >$out
    '';
  };

  invalid-output-name-1 = mkDerivation {
    name = "invalid-output-name-1";
    outputs = [ "out/" ];
  };

  invalid-output-name-2 = mkDerivation {
    name = "invalid-output-name-2";
    outputs = [
      "x"
      "foo$"
    ];
  };

}
