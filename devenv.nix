{
  pkgs,
  config,
  ...
}:

let
  # shrubbery-lib is the reference reader we test the grammar against;
  # rhombus-lib also brings a corpus of real .rhm sources
  racketPkgs = [
    "shrubbery-lib"
    "rhombus-lib"
  ];
in
{
  # Keep user-scope Racket packages inside the project instead of ~/Library/Racket
  env.PLTADDONDIR = "${config.devenv.state}/racket";

  packages = [
    pkgs.git
    pkgs.just
  ];

  languages.racket = {
    enable = true;
    package = pkgs.racket;
  };

  scripts.racket-setup.exec = ''
    raco pkg install --auto --no-docs --skip-installed --scope user ${toString racketPkgs}
  '';

  enterShell = ''
    if [ ! -d "$PLTADDONDIR/$(racket -e '(display (version))')/pkgs/shrubbery-lib" ]; then
      echo "Shrubbery is not installed yet; installing (takes a few minutes the first time)..."
      racket-setup
    fi
  '';

  enterTest = ''
    racket -l shrubbery/parse -e '(void)'
  '';
}
