# nice!nano Nordic UART Service bridge

Bidirectional, binary-safe bridge between the nice!nano hardware UART and
Nordic UART Service (NUS), based on the nRF Connect SDK `peripheral_uart`
sample.

## Interface

- BLE peripheral name: `niceUART`
- BLE service: Nordic UART Service (`6E400001-B5A3-F393-E0A9-E50E24DCCA9E`)
- UART: 115200 baud, 8 data bits, no parity, 1 stop bit, no flow control
- nice!nano TX: D1 / P0.06
- nice!nano RX: D0 / P0.08
- Logic level: 3.3 V

Wire TX to the peer's RX, RX to the peer's TX, and connect grounds.

The blue LED blinks while advertising and stays on while connected. UART data
is forwarded after 10 ms of idle time or when a 244-byte buffer fills. BLE
packet boundaries are not preserved; the interface is a byte stream.

The default `paired` profile requires authenticated LE Secure Connections.
Pass the PIN through a private Kconfig fragment using `NUS_UART_EXTRA_CONF`.
Bonds for up to eight clients are retained, so each client
enters the passkey only once. The `open` profile disables pairing. NUS
notifications must be enabled by the BLE central before UART-to-BLE data can be
delivered.

To erase all stored client keys, hold `D0/RX` connected to GND while powering
or resetting the nice!nano. Keep it low for two seconds while the blue LED
flashes quickly, then disconnect the jumper before using the UART.

## Build and flash

The project is pinned to nRF Connect SDK v3.4.1 and Zephyr SDK 1.0.1.

```sh
nix develop -c ./scripts/bootstrap.sh
./scripts/flash-private.sh /path/to/pin-file
```

Set the advertised BLE name for the private build with an environment variable:

```sh
NUS_UART_DEVICE_NAME='Heewing T1' ./scripts/flash-private.sh /path/to/pin-file
```

`flash-private.sh` requires exactly six digits. It creates the PIN fragment and
the complete build under RAM-backed `/dev/shm`, verifies the effective Kconfig,
flashes the UF2, and removes the temporary directory. The PIN is never written
to the project or its normal build directory.

For an unpaired development build, replace `paired` with `open`.

On Linux, `ble-serial` can expose NUS as a pseudo-terminal:

```sh
ble-serial -d niceUART -s 6e400001-b5a3-f393-e0a9-e50e24dcca9e
```
