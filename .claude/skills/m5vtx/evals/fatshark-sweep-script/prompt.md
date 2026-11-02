---
tags: [skill-creator]
max_turns: 20
timeout_seconds: 600
allowed_tools: [Read, Glob, Grep, Skill, Write, Edit, Bash]
---

Write me sweep.sh: I want to walk my VTX through all 8 fatshark channels and log my receiver's RSSI on each so I can see if its front end has a dead spot. M5VTX is on COM24, RSSI comes from `rssi-cli read` which prints a number. Log to a csv. Just write the script to the outputs folder, I'm not at the bench so don't run it or touch any serial port. Table on the board is:

vtxtable bands 6
vtxtable channels 8
vtxtable band 1 BOSCAM_A A FACTORY 5865 5845 5825 5805 5785 5765 5745 5725
vtxtable band 2 BOSCAM_B B FACTORY 5733 5752 5771 5790 5809 5828 5847 5866
vtxtable band 3 BOSCAM_E E FACTORY 5705 5685 5665    0 5885 5905    0    0
vtxtable band 4 FATSHARK F FACTORY 5740 5760 5780 5800 5820 5840 5860 5880
vtxtable band 5 RACEBAND R FACTORY 5658 5695 5732 5769 5806 5843 5880 5917
vtxtable band 6 IMD6     I CUSTOM  5732 5765 5828 5840 5866 5740    0    0
vtxtable powerlevels 3
vtxtable powervalues 0 1 2
vtxtable powerlabels 25 100 200
