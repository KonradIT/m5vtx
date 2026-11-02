// m5vtxctl drives an M5VTX board over its USB serial console, for scripts and
// agents: every command is bounded in time, prints one JSON value (send prints the
// board's own lines) and says by its exit status whether it worked.
//
// Opening the port leaves DTR and RTS low, so it does not reset the board. Only one
// program can hold the port: close `pio device monitor` first.
package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/KonradIT/m5vtx/tools/console"
	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

// Exit statuses besides 0. The flag package exits with 2 on a usage error.
const (
	exitError       = 1 // The port could not be used, or the board did not answer.
	exitUsage       = 2
	exitUnconfirmed = 3 // The board answered, and the thing asked for did not happen.
)

const usage = `Usage: m5vtxctl COMMAND [flags]

  ports [-identify]            list serial ports; -identify listens for the M5VTX's
                               periodic report, writing nothing to any port
  status [-tune-args]          the board's full report, as JSON; -tune-args prints
                               only the tune flags that put the VTX back as it is now
  tune [-band B] [-channel C] [-power P] [-freq MHz] [-pit 0|1] [-timeout T]
                               set the VTX and wait for its readback
  load [-gap D] [-timeout T] FILE
                               send a vtxtable file line by line
  send [-seconds S] [-grep TEXT] [-until REGEXP] [-gap D] [LINE...]
                               send console lines and print what the board says
  reset                        restart the board, then report

Every command takes -port NAME, or reads M5VTX_PORT.

Exit status: 0 done; 1 port or board problem; 2 usage; 3 the board answered and the
setting was not confirmed, or the command was refused (see "outcome" and "reason").
`

func main() {
	if len(os.Args) < 2 {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(exitUsage)
	}
	var err error
	switch command, args := os.Args[1], os.Args[2:]; command {
	case "ports":
		err = ports(args)
	case "status":
		err = status(args)
	case "tune":
		err = tune(args)
	case "load":
		err = load(args)
	case "send":
		err = send(args)
	case "reset":
		err = reset(args)
	case "help", "-h", "-help", "--help":
		fmt.Print(usage)
	default:
		fmt.Fprintf(os.Stderr, "m5vtxctl: unknown command %q\n\n%s", command, usage)
		os.Exit(exitUsage)
	}
	var unconfirmed unconfirmedError
	switch {
	case errors.As(err, &unconfirmed):
		os.Exit(exitUnconfirmed)
	case err != nil:
		fmt.Fprintln(os.Stderr, "m5vtxctl:", err)
		os.Exit(exitError)
	}
}

// unconfirmedError ends the program with exitUnconfirmed; the output has the details.
type unconfirmedError struct{}

func (unconfirmedError) Error() string { return "not confirmed" }

// newFlags makes a flag set with -port, and shows the command's usage on error.
func newFlags(name string) (*flag.FlagSet, *string) {
	flags := flag.NewFlagSet(name, flag.ExitOnError)
	port := flags.String("port", os.Getenv("M5VTX_PORT"), "serial port, such as COM24 or /dev/ttyACM0; default $M5VTX_PORT")
	return flags, port
}

func open(name string) (serial.Port, *console.Session, error) {
	if name == "" {
		return nil, nil, errors.New("no serial port: pass -port or set M5VTX_PORT; `m5vtxctl ports -identify` finds the board")
	}
	port, err := serial.Open(name, &serial.Mode{BaudRate: 115200, InitialStatusBits: &serial.ModemOutputBits{}})
	var portError *serial.PortError
	if errors.As(err, &portError) && portError.Code() == serial.PortBusy {
		return nil, nil, fmt.Errorf("%s: %w; only one program can hold the port, so close any monitor first", name, err)
	}
	if err != nil {
		return nil, nil, fmt.Errorf("%s: %w", name, err)
	}
	return port, console.New(port), nil
}

func printJSON(value any) error {
	text, err := json.Marshal(value)
	if err != nil {
		return err
	}
	fmt.Println(string(text))
	return nil
}

type portInfo struct {
	Name    string `json:"name"`
	USB     bool   `json:"usb"`
	VID     string `json:"vid,omitempty"`
	PID     string `json:"pid,omitempty"`
	Product string `json:"product,omitempty"`
	Serial  string `json:"serial,omitempty"`
	M5VTX   *bool  `json:"m5vtx,omitempty"`
	Error   string `json:"error,omitempty"`
}

func ports(args []string) error {
	flags := flag.NewFlagSet("ports", flag.ExitOnError)
	identify := flags.Bool("identify", false, "listen on each USB serial port for the M5VTX's report; writes nothing")
	listen := flags.Duration("listen", 6*time.Second, "how long -identify listens; the board reports every 5 s")
	flags.Parse(args)
	found, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return err
	}
	infos := make([]portInfo, len(found))
	var group sync.WaitGroup
	for i, port := range found {
		infos[i] = portInfo{Name: port.Name, USB: port.IsUSB, VID: port.VID, PID: port.PID, Product: port.Product, Serial: port.SerialNumber}
		if *identify && port.IsUSB {
			group.Add(1)
			go func(info *portInfo) {
				defer group.Done()
				identifyPort(info, *listen)
			}(&infos[i])
		}
	}
	group.Wait()
	return printJSON(infos)
}

func identifyPort(info *portInfo, listen time.Duration) {
	port, session, err := open(info.Name)
	if err != nil {
		info.Error = err.Error()
		return
	}
	defer port.Close()
	_, err = session.WaitFor(time.Now().Add(listen), "STATUS protocol=")
	isBoard := err == nil
	info.M5VTX = &isBoard
}

func status(args []string) error {
	flags, name := newFlags("status")
	tuneArgs := flags.Bool("tune-args", false, "print only the tune flags that would put the VTX back as it is now")
	flags.Parse(args)
	port, session, err := open(*name)
	if err != nil {
		return err
	}
	defer port.Close()
	snap, err := session.Snapshot()
	if err != nil {
		return err
	}
	if !*tuneArgs {
		return printJSON(snap)
	}
	text, err := snap.TuneArgs()
	if err != nil {
		fmt.Fprintln(os.Stderr, "m5vtxctl status:", err)
		return unconfirmedError{}
	}
	fmt.Println(text)
	return nil
}

func tune(args []string) error {
	flags, name := newFlags("tune")
	band := flags.Int("band", 0, "vtxtable band, 1-based")
	channel := flags.Int("channel", 0, "vtxtable channel, 1-based")
	power := flags.Int("power", 0, "vtxtable power level, 1-based")
	frequency := flags.Int("freq", 0, "frequency in MHz, sent without the vtxtable")
	pit := flags.Int("pit", -1, "pit mode, 0 or 1")
	timeout := flags.Duration("timeout", 25*time.Second, "how long to wait for a connection and readback")
	flags.Parse(args)

	target := console.Target{Band: *band, Channel: *channel, Power: *power, Frequency: *frequency}
	if *pit == 0 || *pit == 1 {
		enabled := *pit == 1
		target.Pit = &enabled
	} else if *pit != -1 {
		return usageError(flags, "-pit takes 0 or 1")
	}
	if *band < 0 || *channel < 0 || *power < 0 || *frequency < 0 || *frequency > 65535 {
		return usageError(flags, "values out of range")
	}
	if *frequency != 0 && (*band != 0 || *channel != 0) {
		return usageError(flags, "-freq and -band or -channel are two ways of saying the same thing; pick one")
	}
	if *band == 0 && *channel == 0 && *power == 0 && *frequency == 0 && target.Pit == nil {
		return usageError(flags, "nothing to set")
	}
	port, session, err := open(*name)
	if err != nil {
		return err
	}
	defer port.Close()
	result, err := session.Tune(target, *timeout)
	if err != nil {
		return err
	}
	if err := printJSON(result); err != nil {
		return err
	}
	if result.Outcome != console.Confirmed {
		return unconfirmedError{}
	}
	return nil
}

func usageError(flags *flag.FlagSet, message string) error {
	fmt.Fprintf(os.Stderr, "m5vtxctl %s: %s\n", flags.Name(), message)
	flags.Usage()
	os.Exit(exitUsage)
	return nil
}

func load(args []string) error {
	flags, name := newFlags("load")
	gap := flags.Duration("gap", 150*time.Millisecond, "pause between lines")
	timeout := flags.Duration("timeout", 25*time.Second, "how long to wait for the file's drafted settings to be confirmed")
	flags.Parse(args)
	if flags.NArg() != 1 {
		return usageError(flags, "takes one file")
	}
	text, err := os.ReadFile(flags.Arg(0))
	if err != nil {
		return err
	}
	lines := strings.Split(strings.ReplaceAll(string(text), "\r\n", "\n"), "\n")
	// Refuse a bad file before touching the port.
	if err := console.CheckLoadable(lines); err != nil {
		printJSON(&console.Result{Outcome: console.Refused, Reason: err.Error()})
		return unconfirmedError{}
	}
	port, session, err := open(*name)
	if err != nil {
		return err
	}
	defer port.Close()
	result, err := session.Load(lines, *gap, *timeout)
	if err != nil {
		return err
	}
	if err := printJSON(result); err != nil {
		return err
	}
	if result.Outcome != console.Loaded {
		return unconfirmedError{}
	}
	return nil
}

func send(args []string) error {
	flags, name := newFlags("send")
	seconds := flags.Float64("seconds", 1, "how long to keep reading after the last line")
	grep := flags.String("grep", "", "print only lines containing this")
	until := flags.String("until", "", "stop at the first line matching this regular expression; exit 3 if none does")
	gap := flags.Duration("gap", 150*time.Millisecond, "pause between lines")
	allowDefaults := flags.Bool("allow-defaults", false, "permit `defaults`, which erases the VTX configuration and restarts the board")
	flags.Parse(args)
	var pattern *regexp.Regexp
	if *until != "" {
		compiled, err := regexp.Compile(*until)
		if err != nil {
			return usageError(flags, err.Error())
		}
		pattern = compiled
	}
	for _, line := range flags.Args() {
		if strings.HasPrefix(line, "-") {
			return usageError(flags, fmt.Sprintf("flags go before the console lines, and %q came after one", line))
		}
		if strings.TrimSpace(line) == "defaults" && !*allowDefaults {
			return usageError(flags, "`defaults` erases the VTX configuration; it needs -allow-defaults")
		}
	}
	port, session, err := open(*name)
	if err != nil {
		return err
	}
	defer port.Close()
	matched := false
	session.Tee = func(line string) {
		if pattern != nil && pattern.MatchString(line) {
			matched = true
		}
		if *grep == "" || strings.Contains(line, *grep) || (pattern != nil && pattern.MatchString(line)) {
			fmt.Println(line)
		}
	}
	for _, line := range flags.Args() {
		if err := session.Send(line); err != nil {
			return err
		}
		session.Pause(*gap)
	}
	deadline := time.Now().Add(time.Duration(*seconds * float64(time.Second)))
	for !matched {
		if _, err := session.Next(deadline); errors.Is(err, console.ErrNoReply) {
			break
		} else if err != nil {
			return err
		}
	}
	if pattern != nil && !matched {
		fmt.Fprintf(os.Stderr, "m5vtxctl: no line matched %q\n", *until)
		return unconfirmedError{}
	}
	return nil
}

func reset(args []string) error {
	flags, name := newFlags("reset")
	flags.Parse(args)
	port, session, err := open(*name)
	if err != nil {
		return err
	}
	defer port.Close()
	if err := port.SetRTS(true); err != nil {
		return err
	}
	time.Sleep(100 * time.Millisecond)
	if err := port.SetRTS(false); err != nil {
		return err
	}
	if _, err := session.WaitFor(time.Now().Add(8*time.Second), "M5VTX boot:"); err != nil {
		return err
	}
	session.Pause(2500 * time.Millisecond)
	snap, err := session.Snapshot()
	if err != nil {
		return err
	}
	return printJSON(snap)
}
