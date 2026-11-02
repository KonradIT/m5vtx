package console

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"strings"
	"testing"
	"time"
)

// fakeBoard answers like the firmware, stepping one VTX reply per status request.
type fakeBoard struct {
	out io.Writer

	setupDone, connected, probing       bool
	band, channel, power, frequency     int
	draftBand, draftChannel, draftPower int
	edited, pending, queued             int
	pit, valid                          int
	bands, channels, levels             int
	frequencies                         map[[2]int]int

	ignoreSets  bool // The VTX acknowledges and stays where it is.
	rejectFreq  int  // How many freq commands to refuse before taking one.
	staleReport bool // A periodic report goes out just before `save` is processed.
	queuedFreq  int
	lines       []string
}

func newFakeBoard(out io.Writer) *fakeBoard {
	return &fakeBoard{
		out: out, setupDone: true, connected: true,
		band: 5, channel: 1, power: 2, frequency: 5658,
		draftBand: 5, draftChannel: 1, draftPower: 2,
		bands: 6, channels: 8, levels: 3,
		frequencies: map[[2]int]int{{5, 1}: 5658, {4, 4}: 5800, {6, 1}: 5732},
	}
}

func (b *fakeBoard) say(format string, args ...any) { fmt.Fprintf(b.out, format+"\r\n", args...) }

func flag01(value bool) int {
	if value {
		return 1
	}
	return 0
}

// report prints the lines of the firmware's report(), with its radio traffic around them.
func (b *fakeBoard) report() {
	b.say("VTX TX t=1 [7] 00 AA 55 03 00 9F 00")
	b.say("VTX STATUS protocol=SmartAudio version=2 freq=%d channel=32 power_mW=0 power_index=1 power_dBm=-1 pit=0 mode=0", b.frequency)
	b.say("STATUS protocol=SmartAudio connected=%d probing=%d baud=4800 inverted=0 frequency=%d band=%d channel=%d channel_raw=32 power_mw=0 power_index=%d power_raw=%d power_value=%d power_levels=%d sa2_offset=1 pit=%d queries=9 rx=99 valid=%d crc_errors=0 echoes=0 video=1 standard=NTSC frames=1234 heap=100000 pattern=0",
		flag01(b.connected), flag01(b.probing), b.frequency, b.band, b.channel, b.power-1, b.power, b.power-1, b.levels, b.pit, b.valid)
	b.say("STATE Settings read at 1 ms")
	b.say("LINE pin=16 settled=0 rx_edges=40 pullup=1 pulldown=0 floating=0")
	b.say("UI_STATE selected=Pit mode vtx_band=%d vtx_channel=%d draft_mhz=%d vtx_power=%d draft_pit=0 edited=%d pending=%d queued=%d control_pin=16 draft_pin=16 protocol_mode=SmartAudio draft_protocol=SmartAudio setup_bands=%d setup_power=%d setup_protocol=%d setup_done=%d",
		b.draftBand, b.draftChannel, b.frequencies[[2]int{b.draftBand, b.draftChannel}], b.draftPower, b.edited, b.pending, b.queued,
		flag01(b.setupDone), flag01(b.setupDone), flag01(b.setupDone), flag01(b.setupDone))
	b.say("UI_RENDER buffered=1 row_transfers=10")
	b.say("MEDIA mounted=0 scanning=0 playing=0 loading=0 files=0 selected=-1 frame=0/0 fps=0/0 presented=0 underruns=0 errors=0 rejected=0 read_ms=0 max_read_us=0 image=0 flips=0 format=none header_bytes=0")
	b.say("MEDIA_STATE ")
}

// step is one settings reply from the VTX: the pending draft is decided, the next goes out.
func (b *fakeBoard) step() {
	if !b.connected {
		return
	}
	b.valid++
	if b.pending != 0 {
		switch {
		case b.ignoreSets:
			b.say("UI Setting not confirmed; showing readback")
			b.draftBand, b.draftChannel, b.draftPower = b.band, b.channel, b.power
		case b.pending == 1:
			b.band, b.channel = b.draftBand, b.draftChannel
			b.frequency = b.frequencies[[2]int{b.band, b.channel}]
			b.say("UI Setting confirmed by VTX readback")
		default:
			b.power = b.draftPower
			b.say("UI Setting confirmed by VTX readback")
		}
		if b.pending == 1 {
			b.edited &^= editedChannel
		} else {
			b.edited &^= editedPower
		}
		b.pending = 0
	}
	switch {
	case b.queued&editedChannel != 0:
		if b.frequencies[[2]int{b.draftBand, b.draftChannel}] == 0 {
			b.say("UI Channel not available in the vtxtable")
			b.queued &^= editedChannel
			return
		}
		b.queued &^= editedChannel
		b.pending = 1
		b.say("UI Sent; check reported settings above")
	case b.queued&editedPower != 0:
		b.queued &^= editedPower
		b.pending = 2
		b.say("UI Sent; check reported settings above")
	}
	if b.queuedFreq != 0 {
		if !b.ignoreSets {
			b.frequency = b.queuedFreq
		}
		b.queuedFreq = 0
	}
}

func (b *fakeBoard) execute(line string) {
	b.lines = append(b.lines, line)
	var value int
	switch {
	case line == "status":
		b.step()
		b.report()
	case line == "save":
		if b.staleReport {
			b.report()
		}
		b.queued |= b.edited & (editedChannel | editedPower)
		if b.queued != 0 {
			b.say("UI Saved; sending the drafts")
		} else {
			b.say("UI vtxtable saved")
		}
	case strings.HasPrefix(line, "set "):
		name, text, _ := strings.Cut(strings.TrimPrefix(line, "set "), " = ")
		fmt.Sscanf(text, "%d", &value)
		most := map[string]int{"vtx_band": b.bands, "vtx_channel": b.channels, "vtx_power": b.levels}[name]
		if value < 1 || value > most {
			b.say("###ERROR: %s MUST BE 1..%d", name, most)
			return
		}
		switch name {
		case "vtx_band":
			b.draftBand, b.edited = value, b.edited|editedChannel
		case "vtx_channel":
			b.draftChannel, b.edited = value, b.edited|editedChannel
		case "vtx_power":
			b.draftPower, b.edited = value, b.edited|editedPower
		}
		b.say("%s = %d", name, value)
	case strings.HasPrefix(line, "vtxtable bands 99"):
		b.say("###ERROR: INVALID BAND COUNT")
	case strings.HasPrefix(line, "freq "):
		fmt.Sscanf(line, "freq %d", &value)
		if b.rejectFreq > 0 {
			b.rejectFreq--
			b.say("COMMAND %s rejected", line)
			return
		}
		b.queuedFreq = value
		b.say("COMMAND %s queued (requires readback)", line)
	case strings.HasPrefix(line, "pit "):
		b.say("COMMAND %s rejected", line)
	}
}

// start connects a Session to a fakeBoard.
func start(t *testing.T, configure func(*fakeBoard)) (*Session, *fakeBoard) {
	t.Helper()
	boardReads, sessionWrites := io.Pipe()
	sessionReads, boardWrites := io.Pipe()
	board := newFakeBoard(boardWrites)
	if configure != nil {
		configure(board)
	}
	go func() {
		scanner := bufio.NewScanner(boardReads)
		for scanner.Scan() {
			board.execute(scanner.Text())
		}
	}()
	t.Cleanup(func() { boardReads.Close(); boardWrites.Close() })
	session := New(struct {
		io.Reader
		io.Writer
	}{sessionReads, sessionWrites})
	session.timing = Timing{Poll: time.Millisecond, Report: 2 * time.Second, Confirm: 50 * time.Millisecond, Reject: 50 * time.Millisecond, Fresh: 50 * time.Millisecond}
	return session, board
}

func TestParseFields(t *testing.T) {
	fields := ParseFields("selected=Pit mode vtx_band=5 power_index=-1 fps=15/1 protocol=SmartAudio")
	if fields["selected"] != "Pit mode" {
		t.Errorf("selected = %v", fields["selected"])
	}
	if fields.Int("vtx_band") != 5 || fields.Int("power_index") != -1 {
		t.Errorf("integers: %v", fields)
	}
	if fields["fps"] != "15/1" || fields["protocol"] != "SmartAudio" {
		t.Errorf("strings: %v", fields)
	}
	if fields.Int("missing") != 0 || fields.Int("protocol") != 0 {
		t.Error("Int of a missing or text field should be 0")
	}
}

func TestSnapshotSkipsRadioTraffic(t *testing.T) {
	session, _ := start(t, nil)
	snap, err := session.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	if snap.Status["standard"] != "NTSC" || snap.Status.Int("frequency") != 5658 {
		t.Errorf("status: %v", snap.Status)
	}
	if snap.State != "Settings read at 1 ms" || snap.Line.Int("rx_edges") != 40 || snap.UI["selected"] != "Pit mode" {
		t.Errorf("snapshot: %+v", snap)
	}
	if snap.MediaState != "" || snap.Media.Int("selected") != -1 {
		t.Errorf("media: %+v", snap)
	}
	if snap.Rebooted {
		t.Error("no boot banner was printed")
	}
}

func TestSnapshotFlagsReboot(t *testing.T) {
	session, board := start(t, nil)
	go func() { board.say("\r\nM5VTX boot: flash=16777216 XTAL=40 MHz heap=200000") }()
	session.Pause(10 * time.Millisecond)
	snap, err := session.Snapshot()
	if err != nil {
		t.Fatal(err)
	}
	if !snap.Rebooted {
		t.Error("the boot banner should set rebooted")
	}
}

func TestSnapshotNotAnM5VTX(t *testing.T) {
	boardReads, sessionWrites := io.Pipe()
	sessionReads, boardWrites := io.Pipe()
	t.Cleanup(func() { boardReads.Close(); boardWrites.Close() })
	go io.Copy(io.Discard, boardReads)
	session := New(struct {
		io.Reader
		io.Writer
	}{sessionReads, sessionWrites})
	session.timing.Report = 20 * time.Millisecond
	if _, err := session.Snapshot(); err == nil || !strings.Contains(err.Error(), "no STATUS report") {
		t.Errorf("err = %v", err)
	}
}

func TestTuneArgs(t *testing.T) {
	report := func(connected, band, edited int) *Snapshot {
		return &Snapshot{
			Status: Fields{"connected": connected, "band": band, "channel": 1, "frequency": 5658, "power_levels": 3, "power_value": 1},
			UI:     Fields{"vtx_power": 2, "edited": edited},
		}
	}
	for _, test := range []struct {
		name string
		snap *Snapshot
		want string
	}{
		{"in the table", report(1, 5, 0), "-band 5 -channel 1 -power 2"},
		{"outside the table", report(1, 0, 0), "-freq 5658 -power 2"},
		{"not connected", report(0, 5, 0), ""},
		{"edit on the board", report(1, 5, editedPower), ""},
		{"edit on another row", report(1, 5, 8), "-band 5 -channel 1 -power 2"},
	} {
		got, err := test.snap.TuneArgs()
		if got != test.want || (err != nil) != (test.want == "") {
			t.Errorf("%s: %q, %v", test.name, got, err)
		}
	}
	unknown := report(1, 5, 0)
	unknown.Status["power_value"] = -1
	if got, _ := unknown.TuneArgs(); got != "-band 5 -channel 1" {
		t.Errorf("a power the VTX has not reported should be left alone: %q", got)
	}
}

func TestTuneConfirmed(t *testing.T) {
	session, _ := start(t, nil)
	result, err := session.Tune(Target{Band: 4, Channel: 4, Power: 1}, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != Confirmed {
		t.Fatalf("outcome %s: %s, events %q", result.Outcome, result.Reason, result.Events)
	}
	if result.Before.Frequency != 5658 || result.After.Frequency != 5800 || result.After.Band != 4 || result.After.PowerLevel != 1 {
		t.Errorf("before %+v, after %+v", result.Before, result.After)
	}
}

func TestTuneDoesNotTrustAReportFromBeforeSave(t *testing.T) {
	session, _ := start(t, func(b *fakeBoard) { b.staleReport = true })
	result, err := session.Tune(Target{Power: 3}, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != Confirmed || result.After.PowerLevel != 3 {
		t.Errorf("outcome %s: %s, after %+v", result.Outcome, result.Reason, result.After)
	}
}

func TestTuneVtxIgnoresTheSetting(t *testing.T) {
	session, _ := start(t, func(b *fakeBoard) { b.ignoreSets = true })
	result, err := session.Tune(Target{Band: 4, Channel: 4}, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != NotConfirmed || !strings.Contains(result.Reason, "not confirmed") {
		t.Errorf("outcome %s: %s", result.Outcome, result.Reason)
	}
	if result.After.Frequency != 5658 {
		t.Errorf("after: %+v", result.After)
	}
}

func TestTuneUnavailableChannel(t *testing.T) {
	session, _ := start(t, nil)
	result, err := session.Tune(Target{Band: 3, Channel: 4}, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != NotConfirmed || !strings.Contains(result.Reason, "Channel not available") {
		t.Errorf("outcome %s: %s", result.Outcome, result.Reason)
	}
}

func TestTuneRefusesDuringSetup(t *testing.T) {
	session, board := start(t, func(b *fakeBoard) { b.setupDone = false; b.connected = false })
	result, err := session.Tune(Target{Power: 1}, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != Refused || !strings.Contains(result.Reason, "protocol") {
		t.Errorf("outcome %s: %s", result.Outcome, result.Reason)
	}
	if strings.Join(board.lines, "|") != "status" {
		t.Errorf("sent %q", board.lines)
	}
}

func TestTuneNotConnected(t *testing.T) {
	session, board := start(t, func(b *fakeBoard) { b.connected = false })
	result, err := session.Tune(Target{Power: 1}, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != NotConnected {
		t.Errorf("outcome %s: %s", result.Outcome, result.Reason)
	}
	if strings.Join(board.lines, "|") != "status" {
		t.Errorf("sent %q", board.lines)
	}
}

func TestTuneGivesUpWaitingForAProbe(t *testing.T) {
	session, _ := start(t, func(b *fakeBoard) { b.connected = false; b.probing = true })
	result, err := session.Tune(Target{Power: 1}, 20*time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != NotConnected {
		t.Errorf("outcome %s: %s", result.Outcome, result.Reason)
	}
}

func TestTuneRefusesToSendSomeoneElsesEdit(t *testing.T) {
	session, board := start(t, func(b *fakeBoard) { b.edited = editedPower; b.draftPower = 3 })
	result, err := session.Tune(Target{Band: 4, Channel: 4}, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != Refused || !strings.Contains(result.Reason, "unsent edit") {
		t.Errorf("outcome %s: %s", result.Outcome, result.Reason)
	}
	for _, line := range board.lines {
		if line == "save" {
			t.Error("save would have sent the edit")
		}
	}
}

func TestTuneOverwritesItsOwnEarlierEdit(t *testing.T) {
	// A channel the table marks unavailable leaves its edit behind; retrying must work.
	session, _ := start(t, func(b *fakeBoard) { b.edited = editedChannel; b.draftBand, b.draftChannel = 3, 4 })
	result, err := session.Tune(Target{Band: 4, Channel: 4}, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != Confirmed {
		t.Errorf("outcome %s: %s", result.Outcome, result.Reason)
	}
}

func TestTuneReportsFirmwareErrors(t *testing.T) {
	session, board := start(t, nil)
	result, err := session.Tune(Target{Power: 9}, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != Refused || !strings.Contains(result.Reason, "MUST BE 1..3") {
		t.Errorf("outcome %s: %s", result.Outcome, result.Reason)
	}
	for _, line := range board.lines {
		if line == "save" {
			t.Error("save should not follow a refused setting")
		}
	}
}

func TestTuneDirectFrequency(t *testing.T) {
	session, _ := start(t, func(b *fakeBoard) { b.rejectFreq = 2 })
	result, err := session.Tune(Target{Frequency: 5800}, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != Confirmed || result.After.Frequency != 5800 {
		t.Errorf("outcome %s: %s, after %+v", result.Outcome, result.Reason, result.After)
	}
}

func TestTuneDirectFrequencyIgnored(t *testing.T) {
	session, _ := start(t, func(b *fakeBoard) { b.ignoreSets = true })
	result, err := session.Tune(Target{Frequency: 5800}, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != NotConfirmed || !strings.Contains(result.Reason, "did not change") {
		t.Errorf("outcome %s: %s", result.Outcome, result.Reason)
	}
}

func TestTuneDirectRejected(t *testing.T) {
	session, _ := start(t, nil)
	enabled := true
	result, err := session.Tune(Target{Pit: &enabled}, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != NotConfirmed || !strings.Contains(result.Reason, "rejected") {
		t.Errorf("outcome %s: %s", result.Outcome, result.Reason)
	}
}

func TestLoad(t *testing.T) {
	session, board := start(t, nil)
	lines := []string{"# a comment", "", "batch start", "vtxtable bands 6", "set vtx_power = 3", "batch end", "save", ""}
	result, err := session.Load(lines, time.Millisecond, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != Loaded || !result.Setup.Done || result.Reason != "" {
		t.Errorf("result: %+v", result)
	}
	for _, line := range board.lines {
		if line == "" {
			t.Error("blank lines need not be sent")
		}
	}
}

func TestLoadReportsErrors(t *testing.T) {
	session, _ := start(t, nil)
	result, err := session.Load([]string{"vtxtable bands 99", "save"}, time.Millisecond, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != Errors || !strings.Contains(result.Reason, "INVALID BAND COUNT") {
		t.Errorf("result: %+v", result)
	}
}

func TestLoadWithoutSave(t *testing.T) {
	session, _ := start(t, nil)
	result, err := session.Load([]string{"vtxtable bands 6"}, time.Millisecond, 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != Loaded || !strings.Contains(result.Reason, "no save") {
		t.Errorf("result: %+v", result)
	}
}

func TestLoadRefusesWhatMustNotBeSent(t *testing.T) {
	session, board := start(t, nil)
	for _, lines := range [][]string{{"vtxtable bands 6", "  defaults "}, {strings.Repeat("x", MaxLine+1)}} {
		result, err := session.Load(lines, time.Millisecond, 2*time.Second)
		if err != nil {
			t.Fatal(err)
		}
		if result.Outcome != Refused {
			t.Errorf("%.20q: %+v", lines, result)
		}
	}
	if len(board.lines) != 0 {
		t.Errorf("sent %q", board.lines)
	}
	if err := CheckLoadable([]string{"defaults nosave"}); err != nil {
		t.Errorf("a dump's defaults nosave is harmless: %v", err)
	}
}

func TestSendRefusesAnOverlongLine(t *testing.T) {
	session, _ := start(t, nil)
	if err := session.Send(strings.Repeat("x", MaxLine+1)); err == nil {
		t.Error("the firmware would cut this line in two")
	}
}

// The firmware is what this package reads. Keep the two together.
func TestFirmwarePrintsWhatThisReads(t *testing.T) {
	source, err := os.ReadFile("../../src/main.cpp")
	if err != nil {
		t.Skip("firmware source not found:", err)
	}
	text := string(source)
	for _, want := range []string{
		`"STATUS protocol=%s connected=%u probing=%u`, "frequency=%u band=%u channel=%u", "power_index=%d power_raw=%d power_value=%d",
		"pit=%u", "valid=%lu", "standard=%s", "pattern=%u",
		`"STATE %s\n"`, `"LINE pin=%u`,
		`"UI_STATE selected=%s`, "vtx_band=%u vtx_channel=%u draft_mhz=%u vtx_power=%u", "edited=%u pending=%u queued=%u",
		"setup_bands=%u setup_power=%u setup_protocol=%u setup_done=%u",
		`"MEDIA mounted=%u`, `"MEDIA_STATE %s\n"`, `"\nM5VTX boot:`,
		`"COMMAND %s %s\n"`, `"queued (requires readback)" : "rejected"`, `"###ERROR: `,
		`"UI %s\n"`, `"Setting confirmed by VTX readback"`, `"Setting not confirmed; showing readback"`,
		`"Unavailable; see VTX status"`, `"Channel not available in the vtxtable"`, `"vtxtable not saved"`,
		`"Saved; sending the drafts"`, `"Saved; drafts are sent once connected"`, `"vtxtable saved"`,
		`char commandLine[192]`, `Serial.printf("%s = %d\n", name, value)`,
	} {
		if !strings.Contains(text, want) {
			t.Errorf("src/main.cpp no longer has %s", want)
		}
	}
}
