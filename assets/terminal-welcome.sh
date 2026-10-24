#!/bin/sh
printf '\033[2J\033[H'
printf '\033[38;2;97;231;198m  ◇  CHROMEDRIVE\033[0m\n'
printf '\033[38;2;118;134;158m  NATIVE SHELL / DISPLAY 01\033[0m\n\n'
printf '  Your desktop.\n  Fully customized.\n\n'
printf '\033[38;2;118;134;158m  ────────────────────────────────────\033[0m\n\n'
printf '  \033[38;2;97;231;198m●\033[0m Wayland connection established\n'
printf '  \033[38;2;97;231;198m●\033[0m Real terminal. Real processes.\n'
printf '  \033[38;2;97;231;198m●\033[0m One app. One spatial display.\n\n'
printf '\033[38;2;118;134;158m  Click here to type. F3 to focus.\n  Launch more clients from this shell.\033[0m\n\n'
export PS1='\[\033[38;2;97;231;198m\]driver\[\033[0m\] \w \[\033[38;2;181;155;250m\]❯\[\033[0m\] '
exec /bin/bash --noprofile --norc -i
