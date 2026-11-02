// The vtxtable parser and printer against two real Betaflight dumps: a DarwinFPV
// quad's, with a FACTORY/CUSTOM mix and unavailable channels, and GEPRC's
// RAD Mini file, with six bands and milliwatt power values.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "vtx_table.h"

static std::vector<std::string> printed(const VtxTable& table) {
  std::vector<std::string> lines;
  char text[160];
  for (int n = 0; table.line(n, text, sizeof(text)); ++n) lines.push_back(text);
  return lines;
}

static void apply(VtxTable& table, const char* line) {
  assert(!strncmp(line, "vtxtable ", 9));
  const char* error = table.command(line + 9);
  if (error) printf("%s: %s\n", line, error);
  assert(!error);
}

int main() {
  VtxTable table;
  assert(table.valid() && table.bands == 0 && !table.frequency(1, 1));
  table.reset();
  assert(table.bands == 5 && table.channels == 8 && table.powerLevels == 0);
  assert(table.frequency(5, 5) == 5806 && table.frequency(1, 1) == 5865 && !table.frequency(6, 1) && !table.frequency(1, 9));
  uint8_t band, channel;
  assert(table.lookup(5880, band, channel) && band == 4 && channel == 8);  // F8 before R7
  assert(!table.lookup(5362, band, channel) && !table.lookup(0, band, channel));
  assert(table.powerIndex(0) == -1);
  assert(table.factory[0] && !strcmp(table.names[4], "RACEBAND") && table.letters[4] == 'R');

  // The DarwinFPV dump, with its padding, as Betaflight prints it.
  const char* darwin[] = {
      "vtxtable bands 6",
      "vtxtable channels 8",
      "vtxtable band 1 BOSCAM_A A FACTORY 5865 5845 5825 5805 5785 5765 5745 5725",
      "vtxtable band 2 BOSCAM_B B FACTORY 5733 5752 5771 5790 5809 5828 5847 5866",
      "vtxtable band 3 BOSCAM_E E FACTORY 5705 5685 5665    0 5885 5905    0    0",
      "vtxtable band 4 FATSHARK F FACTORY 5740 5760 5780 5800 5820 5840 5860 5880",
      "vtxtable band 5 RACEBAND R FACTORY 5658 5695 5732 5769 5806 5843 5880 5917",
      "vtxtable band 6 IMD6     I CUSTOM  5732 5765 5828 5840 5866 5740    0    0",
      "vtxtable powerlevels 3",
      "vtxtable powervalues 0 1 2",
      "vtxtable powerlabels 25 100 200"};
  for (const char* line : darwin) apply(table, line);
  assert(table.bands == 6 && table.frequency(3, 4) == 0 && table.frequency(6, 1) == 5732 && !table.factory[5]);
  assert(table.lookup(5732, band, channel) && band == 5 && channel == 3);
  assert(table.powerLevels == 3 && table.powerIndex(2) == 2 && table.powerIndex(3) == -1);
  assert(!strcmp(table.powerLabels[2], "200") && !strcmp(table.names[5], "IMD6") && table.letters[5] == 'I');
  const auto lines = printed(table);
  assert(lines.size() == sizeof(darwin) / sizeof(darwin[0]));
  for (size_t n = 0; n < lines.size(); ++n) {
    if (lines[n] != darwin[n]) printf("%s\n%s\n", lines[n].c_str(), darwin[n]);
    assert(lines[n] == darwin[n]);
  }

  // GEPRC's RAD Mini file: extra spaces, a trailing space, a sixth low band.
  const char* radMini[] = {
      "vtxtable bands 6",
      "vtxtable channels 8",
      "vtxtable band 1 BOSCAM_A A CUSTOM  5865 5845 5825 5805 5785 5765 5745 5725",
      "vtxtable band 2 BOSCAM_B B CUSTOM  5733 5752 5771 5790 5809 5828 5847 5866",
      "vtxtable band 3 BOSCAM_E E CUSTOM  5705 5685 5665 5665 5885 5905 5905 5905",
      "vtxtable band 4 FATSHARK_F F CUSTOM  5740 5760 5780 5800 5820 5840 5860 5880",
      "vtxtable band 5 RACEBAND_R R CUSTOM  5658 5695 5732 5769 5806 5843 5880 5917",
      "vtxtable band 6 LOWRACE L CUSTOM 5362 5399 5436 5473 5510 5547 5584 5621",
      "vtxtable powerlevels 4",
      "vtxtable powervalues 25 200 500 1000 ",
      "vtxtable powerlabels 25 200 500 MAX"};
  VtxTable geprc;
  geprc.reset();
  for (const char* line : radMini) apply(geprc, line);
  assert(geprc.frequency(6, 1) == 5362 && !geprc.factory[0] && geprc.lookup(5362, band, channel) && band == 6);
  assert(geprc.powerLevels == 4 && geprc.powerIndex(1000) == 3 && !strcmp(geprc.powerLabels[3], "MAX"));
  // Names are cut to eight characters, as Betaflight cuts them.
  assert(!strcmp(geprc.names[3], "FATSHARK") && !strcmp(geprc.names[4], "RACEBAND") && geprc.letters[4] == 'R');
  assert(printed(geprc)[7] == "vtxtable band 6 LOWRACE  L CUSTOM  5362 5399 5436 5473 5510 5547 5584 5621");

  // Errors, with Betaflight's messages, leave the table as it was.
  const VtxTable before = geprc;
  const char* bad[][2] = {{"", "MISSING SUBCOMMAND"}, {"bands 9", "BAND COUNT NOT BETWEEN 0 AND 8"},
                          {"channels -1", "CHANNEL COUNT NOT BETWEEN 0 AND 8"}, {"powerlevels 9", "POWER LEVEL COUNT NOT BETWEEN 0 AND 8"},
                          {"band 7 X X FACTORY 1 2 3 4 5 6 7 8", "BAND NUMBER NOT BETWEEN 1 AND 6"},
                          {"band 1 OK X BOGUS 1 2 3 4 5 6 7 8", "INVALID FACTORY FLAG BOGUS (EXPECTED FACTORY OR CUSTOM)"},
                          {"band 1 OK X ABCDEFGHIJKLMNOPQRSTUVWXYZ01234 1 2 3 4 5 6 7 8", "INVALID FACTORY FLAG ABCDEFGHIJKLMNOPQRSTUVWXYZ01234 (EXPECTED FACTORY OR CUSTOM)"},
                          {"band 1 OK X CUSTOM 1 2 3 4 5 6 7", "NOT ENOUGH FREQUENCIES (EXPECTED 8)"},
                          {"band 1 OK X CUSTOM 1 2 3 4 5 6 7 8 9", "TOO MANY FREQUENCIES (EXPECTED 8)"},
                          {"band 1 OK X CUSTOM 1 2 3 four 5 6 7 8", "INVALID FREQUENCY four"},
                          {"band 1", "MISSING BAND NAME"}, {"band 1 OK", "MISSING BAND LETTER"},
                          {"powervalues 1 2 3", "NOT ENOUGH VALUES (EXPECTED 4)"}, {"powervalues 1 2 3 4 5", "TOO MANY VALUES (EXPECTED 4)"},
                          {"powervalues 1 2 3 70000", "INVALID POWER VALUE 70000"}, {"powerlabels a b c", "NOT ENOUGH LABELS (EXPECTED 4)"},
                          {"powerlabels a b c d e", "TOO MANY LABELS (EXPECTED 4)"}, {"frobnicate 1", "INVALID SUBCOMMAND frobnicate"}};
  for (const auto& c : bad) {
    const char* error = geprc.command(c[0]);
    if (!error || strcmp(error, c[1])) printf("%s: %s\n", c[0], error ? error : "accepted");
    assert(error && !strcmp(error, c[1]));
    assert(!memcmp(&geprc, &before, sizeof(geprc)));
  }
  // Without the flag a band is CUSTOM; the flag, names, letters and labels are
  // not case sensitive, and long names and labels are cut, as Betaflight cuts them.
  assert(!geprc.command("band 1 NOFLAG N 1 2 3 4 5 6 7 8") && !geprc.factory[0] && geprc.frequency(1, 8) == 8);
  assert(!geprc.command("BAND 1 flagged fx factory 1 2 3 4 5 6 7 8") && geprc.factory[0]);
  assert(!strcmp(geprc.names[0], "FLAGGED") && geprc.letters[0] == 'F');
  assert(!geprc.command("band 1 TOOLONGNAME X CUSTOM 1 2 3 4 5 6 7 8") && !strcmp(geprc.names[0], "TOOLONGN"));
  assert(!geprc.command("powerlabels a b c dddd") && !strcmp(geprc.powerLabels[0], "A") && !strcmp(geprc.powerLabels[3], "DDD"));
  // Shrinking clears what is beyond, so a table never keeps stale entries.
  assert(!geprc.command("channels 4") && geprc.frequency(6, 4) == 5473 && !geprc.frequencies[5][4]);
  assert(!geprc.command("bands 2") && !geprc.names[5][0] && !geprc.frequencies[5][0]);
  assert(!geprc.command("powerlevels 2") && !geprc.powerValues[3] && !geprc.powerLabels[3][0]);
  assert(printed(geprc).size() == 2 + 2 + 3);
  assert(!geprc.command("bands 0") && printed(geprc).size() == 5 && !geprc.frequency(1, 1));
  puts("vtxtable tests passed: Betaflight dumps parsed and printed back verbatim, lookups, errors leave the table unchanged");
}
