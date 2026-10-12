#!/usr/bin/env bash
set -euo pipefail

# KWin's Overlay layer (9) is above active fullscreen windows (5).
rule=blur-rectangle
rules="$(kreadconfig6 --file kwinrulesrc --group General --key rules --default '')"
case ",$rules," in
    *,"$rule",*) ;;
    *)
        rules="${rules:+$rules,}$rule"
        kwriteconfig6 --file kwinrulesrc --group General --key rules "$rules"
        IFS=',' read -r -a rule_ids <<< "$rules"
        kwriteconfig6 --file kwinrulesrc --group General --key count "${#rule_ids[@]}"
        ;;
esac
kwriteconfig6 --file kwinrulesrc --group "$rule" --key Description 'Blur Spotlight overlay'
kwriteconfig6 --file kwinrulesrc --group "$rule" --key wmclass blur-rectangle
kwriteconfig6 --file kwinrulesrc --group "$rule" --key wmclasscomplete false
kwriteconfig6 --file kwinrulesrc --group "$rule" --key wmclassmatch 1
kwriteconfig6 --file kwinrulesrc --group "$rule" --key layer 9
kwriteconfig6 --file kwinrulesrc --group "$rule" --key layerrule 2
qdbus6 org.kde.KWin /KWin org.kde.KWin.reconfigure
