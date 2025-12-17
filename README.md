## CAENHVWrapper

Small helper CLI built on top of the official **CAEN HV Wrapper 6.6** library.  
It is mainly used to control CAEN SY4527 (and compatible HV systems) from the command line:  
set per‑channel voltage/current, turn channels on/off, and read monitoring values.

### Installation

- Install the library and header (root privileges required):

```bash
sudo ./install.sh
```

- Build

```bash
cd HVWrapperDemo
make clean
make all
```

### Examples

Manual channel selection:

```bash
./HVWrappdemo --ch 0 1 2 --V0Set 650 --Pw On      # Turn channels 0,1,2 ON with V0Set=650
./HVWrappdemo --ch 0 1 2 --IMon                   # Read current monitor for channels 0,1,2
./HVWrappdemo --ch 0 1 2 --VMon                   # Read voltage monitor for channels 0,1,2
```

Operate on all channels:

```bash
./HVWrappdemo --ch all -V0Set 650 --Pw On
```

### Using config‑based channels / V0Set / I0Set

```text
ch name V0Set I0Set
0 T1 800 500
1 T2 800 500
...
```

Examples:

```bash
./HVWrappdemo --Pw On    # Apply or Modify V0Set/I0Set from config, then turn them ON
./HVWrappdemo --Pw Off   # Turn the same channels OFF
```

### Interactive demo mode

If you run the executable **without** arguments, the original demo TUI starts:

```bash
./HVWrappdemo
```

Mainly used keys:

- `LOGIN` (b): log into the HV system
- `GETCRATEMAP` (l): show board / channel layout per slot
- `GETCHPARAM` (g): read channel parameter
- `SETCHPARAM` (h): set channel parameter