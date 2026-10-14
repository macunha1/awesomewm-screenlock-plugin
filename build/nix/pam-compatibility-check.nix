{
  package,
  pkgs,
}:

# PAM loads host modules into this process, so both sides must share an ABI.
pkgs.runCommand "awesomewm-screenlock-pam-compatibility" { } ''
  helper="${package}/bin/awesomewm-screenlock-helper"
  library="${package}/lib/libawesomewm_screenlock.so"
  expected_interpreter="${pkgs.stdenv.cc.bintools.dynamicLinker}"
  actual_interpreter="$(${pkgs.patchelf}/bin/patchelf --print-interpreter "$helper")"

  if [ "$actual_interpreter" != "$expected_interpreter" ]; then
    echo "screenlock helper uses $actual_interpreter; expected $expected_interpreter" >&2
    exit 1
  fi

  runtime_paths="$(${pkgs.patchelf}/bin/patchelf --print-rpath "$library")"
  for dependency_path in \
    "${pkgs.lib.getLib pkgs.pam}/lib" \
    "${pkgs.lib.getLib pkgs.stdenv.cc.libc}/lib"
  do
    case ":$runtime_paths:" in
      *":$dependency_path:"*) ;;
      *)
        echo "screenlock library does not use $dependency_path" >&2
        exit 1
        ;;
    esac
  done

  touch "$out"
''
