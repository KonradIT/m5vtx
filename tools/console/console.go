// Package console talks to the M5VTX firmware's serial console. It sends command
// lines, reads the reports the firmware prints back, and waits for VTX settings to
// be confirmed by readback. The report formats and notices it depends on are the
// ones src/main.cpp prints; console_test.go checks them against that file.
package console

import (
	"bufio"
	"errors"
	"fmt"
	"io"
	"strconv"
	"strings"
	"time"
)

// Outcomes of Tune and Load.
const (
	Confirmed    = "confirmed"     // the VTX's own readback matches what was asked for
	NotConfirmed = "not_confirmed" // the VTX answered, but did not report the setting
	NotConnected = "not_connected" // no VTX is answering on the control wire
	Refused      = "refused"       // nothing was sent, or the firmware would not take it
	Loaded       = "loaded"        // a file went in without errors
	Errors       = "errors"        // a file went in, and the firmware reported errors
)

// MaxLine is the longest command the firmware's line buffer takes. A longer one is
// dropped, and the rest of it then runs as a command of its own.
const MaxLine = 191

// The bits of UI_STATE's edited that `save` sends to the VTX.
const (
	editedChannel = 1<<0 | 1<<1
	editedPower   = 1 << 2
)

// Notices that say a drafted setting did not reach the VTX.
var failureNotices = []string{
	"UI Setting not confirmed",
	"UI Unavailable",
	"UI Channel not available",
	"UI vtxtable not saved",
}

// ErrNoReply is returned when the board does not print what was waited for.
var ErrNoReply = errors.New("no reply from the board")

// Fields holds the key=value pairs of one report line, with integers as integers.
type Fields map[string]any

// ParseFields reads key=value pairs. A token without "=" continues the previous
// value, as in "selected=Pit mode".
func ParseFields(text string) Fields {
	fields := Fields{}
	last := ""
	for _, token := range strings.Fields(text) {
		if key, value, found := strings.Cut(token, "="); found {
			fields[key], last = value, key
		} else if last != "" {
			fields[last] = fields[last].(string) + " " + token
		}
	}
	for key, value := range fields {
		if number, err := strconv.Atoi(value.(string)); err == nil {
			fields[key] = number
		}
	}
	return fields
}

// Int returns an integer field, or 0 if it is missing or is not one.
func (f Fields) Int(key string) int {
	number, _ := f[key].(int)
	return number
}

// Snapshot is one complete report from the firmware.
type Snapshot struct {
	Time       time.Time `json:"time"`
	Status     Fields    `json:"status"`
	State      string    `json:"state"`
	Line       Fields    `json:"line"`
	UI         Fields    `json:"ui"`
	Media      Fields    `json:"media"`
	MediaState string    `json:"media_state"`
	Rebooted   bool      `json:"rebooted"`
}

func (snap *Snapshot) connected() bool { return snap.Status.Int("connected") != 0 }
func (snap *Snapshot) probing() bool   { return snap.Status.Int("probing") != 0 }
func (snap *Snapshot) busy() bool {
	return snap.UI.Int("pending") != 0 || snap.UI.Int("queued") != 0
}

// missingSetup names the setup items still open on the board.
func (snap *Snapshot) missingSetup() string {
	var missing []string
	for _, item := range []struct{ key, name string }{
		{"setup_bands", "bands and channels"},
		{"setup_power", "power levels"},
		{"setup_protocol", "protocol"},
	} {
		if snap.UI.Int(item.key) == 0 {
			missing = append(missing, item.name)
		}
	}
	return strings.Join(missing, ", ")
}

// TuneArgs gives the flags of `m5vtxctl tune` that put the VTX back as this report
// has it. It prefers the band and channel, since some VTXs ignore a frequency
// command but follow a channel; only a frequency outside the vtxtable goes by MHz.
func (snap *Snapshot) TuneArgs() (string, error) {
	if !snap.connected() {
		return "", errors.New("no VTX is connected, so there are no settings to read")
	}
	if snap.UI.Int("edited")&(editedChannel|editedPower) != 0 || snap.busy() {
		return "", errors.New("the board holds an edit that is not on the VTX yet, so its drafts are not the VTX's settings")
	}
	var args string
	if band, channel := snap.Status.Int("band"), snap.Status.Int("channel"); band != 0 && channel != 0 {
		args = fmt.Sprintf("-band %d -channel %d", band, channel)
	} else {
		args = fmt.Sprintf("-freq %d", snap.Status.Int("frequency"))
	}
	if snap.Status.Int("power_levels") != 0 && snap.Status.Int("power_value") >= 0 {
		args += fmt.Sprintf(" -power %d", snap.UI.Int("vtx_power"))
	}
	return args, nil
}

// Brief is the part of a report that says what the VTX is doing. Everything but
// PowerLevel, Pattern and Standard is what the VTX reported over its control wire,
// which says nothing about what it radiates.
type Brief struct {
	Connected  bool   `json:"connected"`
	Frequency  int    `json:"frequency"`
	Band       int    `json:"band"`
	Channel    int    `json:"channel"`
	PowerLevel int    `json:"power_level"`
	PowerIndex int    `json:"power_index"`
	PowerRaw   int    `json:"power_raw"`
	PowerValue int    `json:"power_value"`
	PowerMW    int    `json:"power_mw"`
	Pit        bool   `json:"pit"`
	Pattern    int    `json:"pattern"`
	Standard   any    `json:"standard"`
	State      string `json:"state"`
}

// Summarize picks the Brief out of a Snapshot.
func Summarize(snap *Snapshot) *Brief {
	return &Brief{
		Connected:  snap.connected(),
		Frequency:  snap.Status.Int("frequency"),
		Band:       snap.Status.Int("band"),
		Channel:    snap.Status.Int("channel"),
		PowerLevel: snap.UI.Int("vtx_power"),
		PowerIndex: snap.Status.Int("power_index"),
		PowerRaw:   snap.Status.Int("power_raw"),
		PowerValue: snap.Status.Int("power_value"),
		PowerMW:    snap.Status.Int("power_mw"),
		Pit:        snap.Status.Int("pit") != 0,
		Pattern:    snap.Status.Int("pattern"),
		Standard:   snap.Status["standard"],
		State:      snap.State,
	}
}

// Setup is the board's first-time setup list.
type Setup struct {
	Bands    bool `json:"bands"`
	Power    bool `json:"power"`
	Protocol bool `json:"protocol"`
	Done     bool `json:"done"`
}

// Result is what Tune and Load report.
type Result struct {
	Outcome  string   `json:"outcome"`
	Reason   string   `json:"reason,omitempty"`
	Before   *Brief   `json:"before,omitempty"`
	After    *Brief   `json:"after,omitempty"`
	Setup    *Setup   `json:"setup,omitempty"`
	Events   []string `json:"events,omitempty"`
	Rebooted bool     `json:"rebooted"`
}

// Timing holds how long the session waits for things. The defaults suit the
// firmware, which polls the VTX every 1.5 s and gives up on a setting after 6.5 s.
type Timing struct {
	Poll    time.Duration // between status requests
	Report  time.Duration // for a report or a reply to arrive
	Confirm time.Duration // for a directly set value to be read back
	Reject  time.Duration // retrying a command the firmware refused while busy
	Fresh   time.Duration // for one more settings reply once everything has settled
}

// DefaultTiming is the Timing a new Session starts with.
var DefaultTiming = Timing{
	Poll:    time.Second,
	Report:  8 * time.Second, // A rebooting board reports within about 5 s.
	Confirm: 6500 * time.Millisecond,
	Reject:  3 * time.Second,
	Fresh:   3 * time.Second,
}

// Session is one conversation with the board.
type Session struct {
	port     io.Writer
	lines    chan string
	timing   Timing
	Events   []string // Notices, command results and errors, in order.
	Rebooted bool     // The board was seen booting during this session.
	Tee      func(line string)
}

// New starts reading from port. The reader stops when port's Read fails.
func New(port io.ReadWriter) *Session {
	s := &Session{port: port, lines: make(chan string, 256), timing: DefaultTiming}
	go func() {
		defer close(s.lines)
		scanner := bufio.NewScanner(port)
		for scanner.Scan() {
			s.lines <- strings.TrimRight(scanner.Text(), "\r")
		}
	}()
	return s
}

// Send writes one command line.
func (s *Session) Send(line string) error {
	if len(line) > MaxLine {
		return fmt.Errorf("line is %d characters, the firmware takes %d: %.40s...", len(line), MaxLine, line)
	}
	_, err := io.WriteString(s.port, line+"\n")
	return err
}

// Next returns the next line the board prints, or ErrNoReply at the deadline.
func (s *Session) Next(deadline time.Time) (string, error) {
	timer := time.NewTimer(time.Until(deadline))
	defer timer.Stop()
	select {
	case line, open := <-s.lines:
		if !open {
			return "", errors.New("the serial port closed")
		}
		s.note(line)
		return line, nil
	case <-timer.C:
		return "", ErrNoReply
	}
}

func (s *Session) note(line string) {
	if s.Tee != nil {
		s.Tee(line)
	}
	if strings.HasPrefix(line, "M5VTX boot:") || strings.HasPrefix(line, "rst:0x") {
		s.Rebooted = true
	}
	for _, prefix := range []string{"UI ", "COMMAND ", "###ERROR"} {
		if strings.HasPrefix(line, prefix) {
			s.Events = append(s.Events, line)
		}
	}
}

// Pause keeps reading for a while, so that nothing the board prints is missed.
func (s *Session) Pause(duration time.Duration) {
	deadline := time.Now().Add(duration)
	for {
		if _, err := s.Next(deadline); err != nil {
			return
		}
	}
}

// WaitFor reads until a line starts with one of the prefixes, and returns that line.
func (s *Session) WaitFor(deadline time.Time, prefixes ...string) (string, error) {
	for {
		line, err := s.Next(deadline)
		if err != nil {
			return "", fmt.Errorf("waiting for %q: %w", prefixes, err)
		}
		for _, prefix := range prefixes {
			if strings.HasPrefix(line, prefix) {
				return line, nil
			}
		}
	}
}

// Snapshot asks for a report and returns the next complete one.
func (s *Session) Snapshot() (*Snapshot, error) {
	if err := s.Send("status"); err != nil {
		return nil, err
	}
	return s.report(time.Now().Add(s.timing.Report))
}

func (s *Session) report(deadline time.Time) (*Snapshot, error) {
	var snap *Snapshot
	for {
		line, err := s.Next(deadline)
		if err != nil {
			return nil, fmt.Errorf("no STATUS report; is this an M5VTX at 115200 baud? %w", err)
		}
		if rest, found := strings.CutPrefix(line, "STATUS "); found {
			snap = &Snapshot{Time: time.Now(), Status: ParseFields(rest)}
			continue
		}
		if snap == nil {
			continue
		}
		key, rest, _ := strings.Cut(line, " ")
		switch key {
		case "STATE":
			snap.State = rest
		case "LINE":
			snap.Line = ParseFields(rest)
		case "UI_STATE":
			snap.UI = ParseFields(rest)
		case "MEDIA":
			snap.Media = ParseFields(rest)
		case "MEDIA_STATE":
			snap.MediaState = rest
			snap.Rebooted = s.Rebooted
			return snap, nil
		}
	}
}

// waitReady waits until the board could take a setting, or never will without help.
func (s *Session) waitReady(deadline time.Time) (*Snapshot, error) {
	for {
		snap, err := s.Snapshot()
		if err != nil {
			return nil, err
		}
		if snap.UI.Int("setup_done") == 0 || (!snap.connected() && !snap.probing()) ||
			(snap.connected() && !snap.busy()) || !time.Now().Before(deadline) {
			return snap, nil
		}
		s.Pause(s.timing.Poll)
	}
}

// waitSettled waits until the board has nothing left to send and nothing awaiting
// readback, or will not get there: the queue only moves while a VTX is connected.
func (s *Session) waitSettled(deadline time.Time) (*Snapshot, error) {
	for {
		snap, err := s.Snapshot()
		if err != nil {
			return nil, err
		}
		if !snap.busy() || (!snap.connected() && !snap.probing()) || !time.Now().Before(deadline) {
			return snap, nil
		}
		s.Pause(s.timing.Poll)
	}
}

// waitFresh returns a report holding at least one settings reply newer than snap's.
func (s *Session) waitFresh(snap *Snapshot) (*Snapshot, error) {
	deadline := time.Now().Add(s.timing.Fresh)
	valid := snap.Status.Int("valid")
	for snap.connected() && snap.Status.Int("valid") == valid && time.Now().Before(deadline) {
		s.Pause(s.timing.Poll)
		next, err := s.Snapshot()
		if err != nil {
			return nil, err
		}
		snap = next
	}
	return snap, nil
}

// Target says what Tune should set. Band, Channel and Power are the 1-based vtxtable
// numbers, as in Betaflight's set vtx_*; a zero or nil field is left alone.
type Target struct {
	Band, Channel, Power int
	Frequency            int // Sent as it is, without the vtxtable.
	Pit                  *bool
}

func (t Target) edits() int {
	mask := 0
	if t.Band != 0 || t.Channel != 0 {
		mask |= editedChannel
	}
	if t.Power != 0 {
		mask |= editedPower
	}
	return mask
}

func (t Target) draftLines() []string {
	var lines []string
	for _, setting := range []struct {
		name  string
		value int
	}{{"vtx_band", t.Band}, {"vtx_channel", t.Channel}, {"vtx_power", t.Power}} {
		if setting.value != 0 {
			lines = append(lines, fmt.Sprintf("set %s = %d", setting.name, setting.value))
		}
	}
	return lines
}

// mismatch says how a report differs from the target.
func (t Target) mismatch(snap *Snapshot) string {
	var wrong []string
	check := func(name string, wanted, got int) {
		if wanted != 0 && wanted != got {
			wrong = append(wrong, fmt.Sprintf("%s is %d, not %d", name, got, wanted))
		}
	}
	check("band", t.Band, snap.Status.Int("band"))
	check("channel", t.Channel, snap.Status.Int("channel"))
	check("frequency", t.Frequency, snap.Status.Int("frequency"))
	if t.Band != 0 || t.Channel != 0 {
		check("frequency", snap.UI.Int("draft_mhz"), snap.Status.Int("frequency"))
	}
	check("power level", t.Power, snap.UI.Int("vtx_power"))
	if t.Pit != nil && *t.Pit != (snap.Status.Int("pit") != 0) {
		wrong = append(wrong, fmt.Sprintf("pit is %d", snap.Status.Int("pit")))
	}
	if wrong == nil {
		return ""
	}
	return "VTX readback: " + strings.Join(wrong, ", ")
}

// Tune sets the VTX and waits for the VTX's own readback to agree. The outcome
// describes the control wire: a receiver is the only proof of what is on the air.
func (s *Session) Tune(t Target, timeout time.Duration) (*Result, error) {
	deadline := time.Now().Add(timeout)
	start := len(s.Events)
	snap, err := s.waitReady(deadline)
	if err != nil {
		return nil, err
	}
	result := &Result{Before: Summarize(snap)}
	finish := func(outcome, reason string, last *Snapshot) (*Result, error) {
		result.Outcome, result.Reason = outcome, reason
		result.After = Summarize(last)
		result.Events = s.Events[start:]
		result.Rebooted = s.Rebooted
		return result, nil
	}
	switch {
	case snap.UI.Int("setup_done") == 0:
		return finish(Refused, "first-time setup is not done, still missing: "+snap.missingSetup(), snap)
	case !snap.connected():
		return finish(NotConnected, "no VTX is answering on the control wire: "+snap.State, snap)
	case snap.busy():
		return finish(Refused, "the board is still busy with an earlier setting", snap)
	}
	if stale := snap.UI.Int("edited") & (editedChannel | editedPower) &^ t.edits(); stale != 0 {
		return finish(Refused, fmt.Sprintf("the board holds an unsent edit (edited=%d) that save would also send; the probe command clears it", stale), snap)
	}

	if t.Frequency != 0 {
		reason, err := s.direct(fmt.Sprintf("freq %d", t.Frequency), func(snap *Snapshot) bool {
			return snap.Status.Int("frequency") == t.Frequency
		}, deadline)
		if err != nil {
			return nil, err
		}
		if reason != "" {
			return s.finishDirect(finish, reason)
		}
	}
	if drafts := t.draftLines(); len(drafts) > 0 {
		reason, err := s.sendDrafts(drafts, deadline)
		if err != nil {
			return nil, err
		}
		if reason != "" {
			return s.finishDirect(finish, reason)
		}
	}
	if t.Pit != nil {
		command, wanted := "pit 0", 0
		if *t.Pit {
			command, wanted = "pit 1", 1
		}
		reason, err := s.direct(command, func(snap *Snapshot) bool { return snap.Status.Int("pit") == wanted }, deadline)
		if err != nil {
			return nil, err
		}
		if reason != "" {
			return s.finishDirect(finish, reason)
		}
	}

	snap, err = s.waitSettled(deadline)
	if err != nil {
		return nil, err
	}
	snap, err = s.waitFresh(snap)
	if err != nil {
		return nil, err
	}
	sends := 0
	if t.edits()&editedChannel != 0 {
		sends++
	}
	if t.edits()&editedPower != 0 {
		sends++
	}
	events := s.Events[start:]
	if failure := firstFailure(events); failure != "" {
		return finish(NotConfirmed, failure, snap)
	}
	if snap.busy() {
		return finish(NotConfirmed, "timed out with settings still queued: "+snap.State, snap)
	}
	if count(events, "UI Setting confirmed by VTX readback") < sends {
		return finish(NotConfirmed, "the firmware did not confirm every setting", snap)
	}
	if wrong := t.mismatch(snap); wrong != "" {
		return finish(NotConfirmed, wrong, snap)
	}
	return finish(Confirmed, "", snap)
}

// finishDirect ends a Tune that failed part way, with a last look at the VTX.
func (s *Session) finishDirect(finish func(string, string, *Snapshot) (*Result, error), reason string) (*Result, error) {
	snap, err := s.Snapshot()
	if err != nil {
		return nil, err
	}
	outcome := NotConfirmed
	if strings.HasPrefix(reason, "###ERROR") {
		outcome = Refused
	}
	return finish(outcome, reason, snap)
}

// sendDrafts sends `set` lines and `save`, and returns why it could not.
func (s *Session) sendDrafts(lines []string, deadline time.Time) (string, error) {
	wait := later(deadline, time.Now().Add(s.timing.Report))
	for _, line := range lines {
		if err := s.Send(line); err != nil {
			return "", err
		}
		// The firmware echoes "vtx_band = 5", without the "set ".
		reply, err := s.WaitFor(wait, strings.TrimPrefix(line, "set "), "###ERROR")
		if err != nil {
			return "", err
		}
		if strings.HasPrefix(reply, "###ERROR") {
			return reply, nil
		}
	}
	if err := s.Send("save"); err != nil {
		return "", err
	}
	// A periodic report from before the save would look settled: start after it.
	_, err := s.WaitFor(wait, "UI Saved", "UI vtxtable")
	return "", err
}

// direct sends a command the firmware does not track itself, and watches the
// readback. The firmware refuses it while another setting is in flight.
func (s *Session) direct(command string, reached func(*Snapshot) bool, deadline time.Time) (string, error) {
	wait := later(deadline, time.Now().Add(s.timing.Report))
	retryUntil := earlier(deadline, time.Now().Add(s.timing.Reject))
	for {
		if err := s.Send(command); err != nil {
			return "", err
		}
		reply, err := s.WaitFor(wait, "COMMAND "+command+" ")
		if err != nil {
			return "", err
		}
		if !strings.HasSuffix(reply, "rejected") {
			break
		}
		if !time.Now().Before(retryUntil) {
			return "the firmware or the VTX does not take this now: " + reply, nil
		}
		s.Pause(s.timing.Poll)
	}
	confirmBy := earlier(deadline, time.Now().Add(s.timing.Confirm))
	for {
		snap, err := s.Snapshot()
		if err != nil {
			return "", err
		}
		if reached(snap) {
			return "", nil
		}
		if !time.Now().Before(confirmBy) {
			return "the VTX took `" + command + "` and its readback did not change", nil
		}
		s.Pause(s.timing.Poll)
	}
}

// CheckLoadable rejects what must not be sent from a file: a line the firmware
// would cut in two, and the command that erases the VTX configuration. A dump's
// `defaults nosave` is fine, since the firmware ignores it.
func CheckLoadable(lines []string) error {
	for number, line := range lines {
		if len(line) > MaxLine {
			return fmt.Errorf("line %d is %d characters, the firmware takes %d", number+1, len(line), MaxLine)
		}
		if strings.TrimSpace(line) == "defaults" {
			return fmt.Errorf("line %d is `defaults`, which erases the VTX configuration and restarts the board", number+1)
		}
	}
	return nil
}

// Load sends a vtxtable file line by line, then waits for what its `save` started.
func (s *Session) Load(lines []string, gap, timeout time.Duration) (*Result, error) {
	if err := CheckLoadable(lines); err != nil {
		return &Result{Outcome: Refused, Reason: err.Error()}, nil
	}
	start := len(s.Events)
	saved := false
	for _, line := range lines {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		if err := s.Send(line); err != nil {
			return nil, err
		}
		saved = saved || line == "save"
		s.Pause(gap)
	}
	snap, err := s.waitSettled(time.Now().Add(timeout))
	if err != nil {
		return nil, err
	}
	events := s.Events[start:]
	result := &Result{
		Outcome: Loaded,
		After:   Summarize(snap),
		Events:  events,
		Setup: &Setup{
			Bands:    snap.UI.Int("setup_bands") != 0,
			Power:    snap.UI.Int("setup_power") != 0,
			Protocol: snap.UI.Int("setup_protocol") != 0,
			Done:     snap.UI.Int("setup_done") != 0,
		},
		Rebooted: s.Rebooted,
	}
	switch {
	case firstError(events) != "":
		result.Outcome, result.Reason = Errors, firstError(events)
	case firstFailure(events) != "":
		result.Outcome, result.Reason = NotConfirmed, firstFailure(events)
	case !saved:
		result.Reason = "the file has no save line, so nothing was stored and the table is lost at the next reset"
	case snap.busy():
		result.Reason = "the table is stored, and its drafted settings wait for a connected VTX"
	}
	return result, nil
}

func firstFailure(events []string) string {
	for _, event := range events {
		for _, notice := range failureNotices {
			if strings.HasPrefix(event, notice) {
				return event
			}
		}
	}
	return ""
}

func firstError(events []string) string {
	for _, event := range events {
		if strings.HasPrefix(event, "###ERROR") {
			return event
		}
	}
	return ""
}

func count(events []string, event string) int {
	total := 0
	for _, candidate := range events {
		if candidate == event {
			total++
		}
	}
	return total
}

func earlier(a, b time.Time) time.Time {
	if a.Before(b) {
		return a
	}
	return b
}

func later(a, b time.Time) time.Time {
	if a.After(b) {
		return a
	}
	return b
}
