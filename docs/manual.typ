// Every .typ under docs/ is compiled by `make docs` and attached to each
// release (CI builds them with the Typst version pinned in ci.yml).
#set document(title: "eyff manual")
#set page(paper: "a4", numbering: "1")
#set text(font: "New Computer Modern", size: 11pt)
#set heading(numbering: "1.")

#align(center, text(20pt, weight: "bold")[eyff])
#align(center, "Explore Your F*cking Files")

= Usage

```sh
eyff [-h] [--completions SHELL] [-v] [-c FILE]
```

/ `-h`, `--help`: show the help.
/ `--completions SHELL`: print the completions for `bash`, `zsh` or `fish`
  (`source <(eyff --completions bash)`).
/ `-v`, `--version`: show the version.
/ `-c`, `--config FILE`: use `FILE` as the config.

= Configuration

The config is Lua: `$XDG_CONFIG_HOME/eyff/config.lua`
(`~/.config/eyff/config.lua`), or the file given with `--config`.
Without one, the defaults are used. A `config.lua` in the current directory
is only read with `--config config.lua`: being Lua, a config can run
commands.

```lua
Config = {
}
```
